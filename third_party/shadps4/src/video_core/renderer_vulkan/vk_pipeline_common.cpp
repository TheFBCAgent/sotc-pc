// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <tsl/robin_map.h>
#include <boost/container/static_vector.hpp>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "common/debug.h" // shadow-pc: Tracy zones (session 20)
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/sotc_desc_cache.h" // shadow-pc (session 55)

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() {
    // shadow-pc (session 55): cached descriptor sets are keyed by the layout.
    if (sotc_dyn_buffers && pipeline_layout) {
        SotcDescCache::Dead(SotcDescCache::Layout, u64(VkPipelineLayout(*pipeline_layout)));
    }
}

void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    SOTC_ZONE("Pipeline::BindResources (push)");
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;

    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    cmdbuf.pushConstants(*pipeline_layout, stage_flags, 0u, sizeof(push_data), &push_data);

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (sotc_dyn_buffers) { // shadow-pc (session 55): SOTC_DESC_CACHE
        SotcBindCached(set_writes, cmdbuf, bind_point);
        return;
    }

    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(bind_point, *pipeline_layout, 0, set_writes);
        return;
    }

    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    cmdbuf.bindDescriptorSets(bind_point, *pipeline_layout, 0, desc_set, {});
}

// shadow-pc patch (session 55): the descriptor-set cache, see sotc_desc_cache.h.
bool SotcDescCache::Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("SOTC_DESC_CACHE");
        // Opt-in: no gain on NVIDIA (session 55); RADV's pushes cost more (session 49).
        const bool enabled = v && *v == '1';
        LOG_INFO(Render_Vulkan, "descriptor-set cache {}",
                 enabled ? "on (SOTC_DESC_CACHE=1)" : "off");
        return enabled;
    }();
    return on;
}

bool Pipeline::SotcUseDynBuffers(const Instance& instance, u32 num_buffers) {
    static const bool logged = [&] {
        if (SotcDescCache::Enabled()) {
            LOG_INFO(Render_Vulkan,
                     "descriptor-set cache: pipelines with up to {} buffers (the device's "
                     "dynamic storage buffer limit); the rest push",
                     instance.SotcMaxDynamicStorageBuffers());
        }
        return true;
    }();
    (void)logged;
    return SotcDescCache::Enabled() && num_buffers <= instance.SotcMaxDynamicStorageBuffers();
}

namespace {

/// Handles destroyed since the last bind (any thread; B drains them). Never freed: a handle
/// can die during static destruction.
struct SotcDeadList {
    std::mutex mutex;
    std::vector<u64> handles;
    std::atomic<bool> any{false};
    std::atomic<u64> counts[SotcDescCache::NumKinds]{}; ///< for the 10 s report
};
SotcDeadList& DeadList() {
    static auto* list = new SotcDeadList;
    return *list;
}

/// The cached sets of every pipeline (stage B only, like all recording), keyed by the pipeline
/// layout and the writes without their buffer offsets. A full clear frees the pools once the
/// GPU has passed the last tick that could use their sets.
struct SotcSetCache {
    static constexpr u32 SetsPerPool = 1024;
    static constexpr u64 MaxSets = 64 * 1024; ///< a full clear beyond this bounds the memory

    struct Entry {
        std::vector<u64> words;
        vk::DescriptorSet set;
    };
    tsl::robin_map<u64, Entry> sets;                                   ///< by hash of words
    tsl::robin_map<u64, boost::container::small_vector<u64, 4>> users; ///< handle -> hashes
    std::vector<vk::DescriptorPool> live;
    std::vector<vk::DescriptorPool> spare;
    std::deque<std::pair<vk::DescriptorPool, u64>> retired; ///< pool, last tick using it
    u64 allocated = 0; ///< sets allocated from the live pools
    // The 10 s report.
    u64 binds = 0, misses = 0, evicted = 0, clears = 0;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

    void Clear(Scheduler& scheduler) {
        const u64 tick = scheduler.CurrentTick();
        for (const auto pool : live) {
            retired.emplace_back(pool, tick);
        }
        live.clear();
        sets.clear();
        users.clear();
        allocated = 0;
        ++clears;
    }

    /// Drops the sets that name a destroyed handle.
    void DropDead() {
        auto& dead = DeadList();
        static thread_local std::vector<u64> handles;
        {
            std::scoped_lock lk{dead.mutex};
            handles.swap(dead.handles);
            dead.any.store(false, std::memory_order_relaxed);
        }
        for (const u64 handle : handles) {
            const auto it = users.find(handle);
            if (it == users.end()) {
                continue;
            }
            for (const u64 hash : it->second) {
                // The hash may have been reused by a set without this handle: keep that one.
                const auto set_it = sets.find(hash);
                if (set_it != sets.end() && std::ranges::find(set_it->second.words, handle) !=
                                                set_it->second.words.end()) {
                    sets.erase(set_it);
                    ++evicted;
                }
            }
            users.erase(it);
        }
        handles.clear();
    }

    vk::DescriptorPool NewPool(vk::Device device, Scheduler& scheduler) {
        while (!retired.empty() && scheduler.IsFree(retired.front().second)) {
            const auto pool = retired.front().first;
            retired.pop_front();
            device.resetDescriptorPool(pool);
            spare.push_back(pool);
        }
        if (!spare.empty()) {
            const auto pool = spare.back();
            spare.pop_back();
            return pool;
        }
        static constexpr std::array<vk::DescriptorPoolSize, 4> sizes = {{
            {vk::DescriptorType::eStorageBufferDynamic, SetsPerPool * 12},
            {vk::DescriptorType::eSampledImage, SetsPerPool * 16},
            {vk::DescriptorType::eStorageImage, SetsPerPool * 4},
            {vk::DescriptorType::eSampler, SetsPerPool * 8},
        }};
        const vk::DescriptorPoolCreateInfo pool_ci = {
            .maxSets = SetsPerPool,
            .poolSizeCount = u32(sizes.size()),
            .pPoolSizes = sizes.data(),
        };
        auto [result, pool] = device.createDescriptorPool(pool_ci);
        ASSERT_MSG(result == vk::Result::eSuccess, "SOTC_DESC_CACHE: descriptor pool: {}",
                   vk::to_string(result));
        return pool;
    }

    vk::DescriptorSet Allocate(vk::Device device, Scheduler& scheduler,
                               vk::DescriptorSetLayout layout) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!live.empty()) {
                const vk::DescriptorSetAllocateInfo alloc_info = {
                    .descriptorPool = live.back(),
                    .descriptorSetCount = 1,
                    .pSetLayouts = &layout,
                };
                vk::DescriptorSet set;
                if (device.allocateDescriptorSets(&alloc_info, &set) == vk::Result::eSuccess) {
                    ++allocated;
                    return set;
                }
            }
            live.push_back(NewPool(device, scheduler));
        }
        UNREACHABLE_MSG("SOTC_DESC_CACHE: a set doesn't fit an empty pool");
    }

    void Report() {
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - t0).count();
        if (secs < 10.0) {
            return;
        }
        u64 dead[SotcDescCache::NumKinds];
        for (int k = 0; k < SotcDescCache::NumKinds; ++k) {
            dead[k] = DeadList().counts[k].exchange(0, std::memory_order_relaxed);
        }
        LOG_INFO(Render_Vulkan,
                 "desc-cache: {:.1f} s: {} binds, {} misses ({:.2f}%), {} sets evicted by "
                 "destroyed handles (views {}, samplers {}, buffers {}, layouts {}), {} full "
                 "clears; {} sets cached, {} allocated in {} pools ({} retired, {} spare)",
                 secs, binds, misses, binds ? 100.0 * misses / binds : 0.0, evicted,
                 dead[SotcDescCache::View], dead[SotcDescCache::Sampler],
                 dead[SotcDescCache::Buffer], dead[SotcDescCache::Layout], clears, sets.size(),
                 allocated, live.size(), retired.size(), spare.size());
        binds = misses = evicted = clears = 0;
        t0 = now;
    }
};

SotcSetCache g_sotc_sets;

} // namespace

void SotcDescCache::Dead(Kind kind, u64 handle) {
    if (!Enabled() || handle == 0) {
        return;
    }
    auto& dead = DeadList();
    dead.counts[kind].fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lk{dead.mutex};
    dead.handles.push_back(handle);
    dead.any.store(true, std::memory_order_release);
}

void Pipeline::SotcBindCached(DescriptorWrites& set_writes, vk::CommandBuffer cmdbuf,
                              vk::PipelineBindPoint bind_point) const {
    SOTC_ZONE("SotcBindCached");
    auto& cache = g_sotc_sets;
    if (DeadList().any.load(std::memory_order_acquire)) {
        cache.DropDead();
    }
    if (cache.allocated >= SotcSetCache::MaxSets) {
        cache.Clear(scheduler);
    }

    // The key: the layout and every write without its buffer offsets. A dynamic offset is 32
    // bits, so an offset past 4 GiB keeps its high part in the descriptor (and the key).
    static thread_local std::vector<u64> words;
    static thread_local boost::container::small_vector<u32, 32> offsets;
    words.clear();
    offsets.clear();
    words.push_back(u64(VkPipelineLayout(*pipeline_layout)));
    u32 num_buffers = 0;
    for (const auto& w : set_writes) {
        words.push_back(u64(w.dstBinding) | u64(w.descriptorCount) << 32);
        words.push_back(u64(w.descriptorType));
        for (u32 i = 0; i < w.descriptorCount; ++i) {
            if (w.pBufferInfo) {
                const auto& b = w.pBufferInfo[i];
                words.push_back(u64(VkBuffer(b.buffer)));
                words.push_back(b.range);
                words.push_back(b.offset & ~u64(0xFFFFFFFF));
                offsets.push_back(u32(b.offset));
                ++num_buffers;
            } else {
                const auto& im = w.pImageInfo[i];
                words.push_back(u64(VkSampler(im.sampler)));
                words.push_back(u64(VkImageView(im.imageView)));
                words.push_back(u64(im.imageLayout));
            }
        }
    }
    u64 hash = 0xCBF29CE484222325ull;
    for (const u64 v : words) {
        hash = (hash ^ v) * 0x100000001B3ull;
        hash ^= hash >> 29;
    }

    ++cache.binds;
    vk::DescriptorSet set;
    const auto it = cache.sets.find(hash);
    if (it != cache.sets.end() && it->second.words == words) {
        set = it->second.set;
    } else {
        ++cache.misses;
        const auto device = instance.GetDevice();
        set = cache.Allocate(device, scheduler, *desc_layout);
        static thread_local boost::container::small_vector<vk::DescriptorBufferInfo, 32> infos;
        infos.clear();
        infos.reserve(num_buffers);
        for (auto& w : set_writes) {
            w.dstSet = set;
            if (w.pBufferInfo) {
                const auto first = infos.size();
                for (u32 i = 0; i < w.descriptorCount; ++i) {
                    const auto& b = w.pBufferInfo[i];
                    infos.push_back({b.buffer, b.offset & ~u64(0xFFFFFFFF), b.range});
                    cache.users[u64(VkBuffer(b.buffer))].push_back(hash);
                }
                w.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
                w.pBufferInfo = &infos[first];
            } else {
                for (u32 i = 0; i < w.descriptorCount; ++i) {
                    const auto& im = w.pImageInfo[i];
                    cache.users[u64(VkSampler(im.sampler))].push_back(hash);
                    cache.users[u64(VkImageView(im.imageView))].push_back(hash);
                }
            }
        }
        cache.users[words[0]].push_back(hash);
        device.updateDescriptorSets(set_writes, {});
        cache.sets.insert_or_assign(hash, SotcSetCache::Entry{words, set});
    }
    cmdbuf.bindDescriptorSets(bind_point, *pipeline_layout, 0, set,
                              std::span<const u32>{offsets.data(), offsets.size()});
    if ((cache.binds & 4095) == 0) {
        cache.Report();
    }
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
