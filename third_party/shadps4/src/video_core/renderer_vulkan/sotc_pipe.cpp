// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 60): the two-stage GPU pipeline (sessions 43-57, docs/next/gpu-pipe.md)
// and its verify mode (SOTC_GPU_PIPE_VERIFY), moved from vk_rasterizer.cpp unchanged. Its
// types are in sotc_rasterizer_internal.h; the register accessors (Regs() etc.) stay in
// vk_rasterizer.cpp, next to the recording code that calls them.

#include <condition_variable>
#include <deque>
#include <mutex>
#include "common/debug.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/platform.h" // shadow-pc (session 45): IRQs signalled by stage B
#include "shader_recompiler/resource.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h" // shadow-pc (session 45): fence packets
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/sotc_cpu_copy.h" // shadow-pc (session 21)
#include "video_core/renderer_vulkan/sotc_gpu_pipe.h" // shadow-pc (session 43)
#include "video_core/renderer_vulkan/sotc_wait_timer.h" // shadow-pc (session 49)
#include "video_core/renderer_vulkan/sotc_memo_probe.h" // shadow-pc (W1)
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"
#include "sotc_debug.h" // shadow-pc: gpu/src/debug_dump.cpp
#include <chrono>
#include <unordered_set>
#include "video_core/renderer_vulkan/sotc_rasterizer_internal.h" // shadow-pc (session 60)

namespace Vulkan {

// Stage A: what Draw/Dispatch did before recording, then the packet.
void Rasterizer::SotcPipeSubmit(SotcPipeArgs args) {
    SOTC_ZONE("SotcPipeSubmit");
    auto& pipe = *sotc_pipe;
    if (pipe.threaded) {
        // Session 47: forget what B has run. Only here, between walks: a walker may still
        // read an overlay SotcPipeOverlay handed out earlier in this packet's selection.
        pipe.a_view = std::max(pipe.a_view, pipe.progress.load(std::memory_order_acquire));
        if ((pipe.submitted & 15) == 0 && !pipe.pending.Empty()) {
            pipe.pending.Retire(pipe.a_view);
        }
    }
    const Pipeline* pipeline = nullptr;
    if (args.IsCompute()) {
        const ComputePipeline* cp = pipeline_cache.GetComputePipeline();
        if (!cp || SotcSkipPipeline(cp)) { // shadow-pc: SOTC_GPU_SKIP
            return;
        }
        pipeline = cp;
    } else {
        args.filter = ClassifyDraw(liverpool->regs);
        if (args.filter.kind == SotcDrawFilter::Kind::Draw) {
            const GraphicsPipeline* gp =
                args.kind == SotcPipeKind::DrawIndirect
                    ? pipeline_cache.GetGraphicsPipeline({
                          .vertex_sgpr_offset = args.vertex_sgpr_offset,
                          .instance_sgpr_offset = args.instance_sgpr_offset,
                      })
                    : pipeline_cache.GetGraphicsPipeline();
            if (!gp || SotcSkipPipeline(gp)) { // shadow-pc: SOTC_GPU_SKIP
                return;
            }
            pipeline = gp;
        }
    }

    // Session 44: the CPU copy's dispatch writes guest memory on B that the SRT walker here
    // reads within a few packets (gpu-pipe.md, rule 2). Session 47: A computes it (B commits)
    // and reads the destination from its overlay meanwhile.
    const bool cpu_copy =
        args.IsCompute() &&
        static_cast<const ComputePipeline*>(pipeline)->GetStage(Shader::SwStage::Compute).pgm_hash ==
            SotcCpuCopy::ShaderHash;
    const u64 seq = pipe.submitted + 1;
    SotcCopyResult copy{};
    thread_local SotcWords copy_words;
    bool copied = false;
    if (cpu_copy && pipe.threaded && args.kind == SotcPipeKind::DispatchDirect) {
        const auto& cs =
            static_cast<const ComputePipeline*>(pipeline)->GetStage(Shader::SwStage::Compute);
        copied = SotcCpuCopyCompute(cs, liverpool->GetCsRegs(), copy, copy_words) &&
                 copy_words.size() * sizeof(u32) <= 4_MB;
    }
    if (pipe.threaded && pipeline) {
        SotcPipeNoteWrites(pipeline, seq, copied ? copy.dst : 0);
    }
    args.verify = pipe.verify_every && (pipe.submitted + 1) % pipe.verify_every == 0;

    auto& w = pipe.writer;
    w.Clear();
    w.Put(args);
    SotcEncodeRegDelta(liverpool->sotc_dirty, liverpool->regs.reg_array.data(), w, pipe.scratch);
    w.Put(liverpool->last_cb_extent);
    w.Put(liverpool->last_db_extent);
    if (args.IsCompute()) {
        w.Put(liverpool->GetCsRegs());
    }
    w.Put(pipeline);
    if (pipeline) {
        u64 num_stages = 0;
        for (const auto* stage : pipeline->GetStages()) {
            num_stages += stage != nullptr;
        }
        w.Put(num_stages);
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) {
                continue;
            }
            const auto snap = stage->MakeSnapshot();
            w.Put(snap.info);
            w.Put(snap.pgm_base);
            w.PutSpan(snap.user_data);
            w.PutSpan(snap.flat);
            // std::pair is not trivially copyable (its assignment), but its bytes are.
            w.Put<u64>(snap.patches.size());
            w.PutRaw(snap.patches.data(), snap.patches.size_bytes());
        }
    }
    if (args.IsCompute()) {
        // Session 47: the CPU copy A computed (count 0: none, B runs SotcCpuCopy itself).
        w.Put<u64>(copied ? copy.count : 0);
        if (copied) {
            w.Put(copy);
            w.PutRaw(copy_words.data(), copy_words.size() * sizeof(u32));
        }
    }
    if (args.verify) {
        w.PutRaw(liverpool->regs.reg_array.data(), sizeof(liverpool->regs.reg_array));
    }
    ASSERT_MSG(w.Bytes().size() <= pipe.ring.MaxPayload(), "pipe packet too large: {} bytes",
               w.Bytes().size());
    if (!pipe.threaded && w.Bytes().size() > SotcPipe::RingBytes / 4) {
        // Sync: B's unpublished pops (SotcPacketRing::PublishEvery) must not leave too little
        // room for a huge packet (a ClearState delta is ~220 KiB, so this never runs in practice).
        pipe.ring.Publish();
    }
    pipe.ring.Push(w.Bytes());
    ++pipe.submitted;
    if (!pipe.threaded) {
        SotcPipeRun(); // sync: B runs at once
        return;
    }
    if (copied) {
        ++pipe.overlay_copies;
        pipe.overlay_bytes += copy_words.size() * sizeof(u32);
        pipe.pending.AddOverlay(seq, copy.dst, std::exchange(copy_words, {}));
    } else if (cpu_copy) {
        // A copy left to the GPU (a GPU-written source, ~half of them): its destination is a
        // pending GPU write (SotcPipeNoteWrites), so an A read of it waits for B ("pending")
        // and then downloads, as the walker did before. Sync mode: B already ran it.
        ++pipe.copy_fallbacks;
    }
    if ((pipe.submitted & 4095) == 0) {
        SotcPipeReport();
    }
}

// Stage B, sync mode: runs the queued packets in order.
void Rasterizer::SotcPipeRun() {
    SOTC_ZONE("SotcPipeRun");
    auto& pipe = *sotc_pipe;
    for (auto bytes = pipe.ring.Front(); !bytes.empty(); bytes = pipe.ring.Front()) {
        SotcPipeRunPacket(bytes);
        pipe.ring.Pop();
    }
}

// Stage B, SOTC_GPU_PIPE=1: the sotc:GpuRecord thread.
void Rasterizer::SotcPipeThread() {
    Common::SetCurrentThreadName("sotc:GpuRecord");
    SotcWaitTimer::on_b = true; // session 49: B waits in the pipe: line
    SotcDebug::OnGpuThreadStart(); // its scheduler waits count as GPU-thread waits
    auto& pipe = *sotc_pipe;
    u64 run = 0;
    // Session 46: B owns its state while awake; A's drain guard takes it while B sleeps.
    std::unique_lock own{pipe.state_mutex};
    for (;;) {
        SotcPipeRunCommands(); // between packets
        auto bytes = pipe.ring.Front();
        if (bytes.empty()) {
            // Idle: hand A the state, tell a draining A, then sleep until a packet or command.
            pipe.ring.Publish();
            own.unlock();
            pipe.retired.store(run, std::memory_order_seq_cst);
            if (pipe.drain_waiting.load(std::memory_order_seq_cst)) {
                pipe.retired.notify_all();
            }
            const auto t0 = std::chrono::steady_clock::now();
            {
                SotcWaitTimer::Where w{SotcWaitTimer::b_where, "idle"}; // session 64
                bytes = pipe.ring.WaitFront(
                    [&] { return pipe.num_commands.load(std::memory_order_seq_cst) != 0; });
            }
            pipe.b_idle_ns.fetch_add(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - t0)
                                             .count()),
                                     std::memory_order_relaxed);
            {
                SotcWaitTimer::Where w{SotcWaitTimer::b_where, "A's guard"}; // session 64
                own.lock(); // waits while A holds a drain guard
            }
            if (bytes.empty()) {
                continue; // woken for a command
            }
        }
        const auto kind = SotcPacketReader{bytes}.Get<SotcPipeArgs>().kind;
        if (kind == SotcPipeKind::Stop) {
            buffer_cache.SotcReadbackSettleAll(); // session 50: wakes their faulting threads
            SotcPipeRunCommands();
            pipe.ring.Pop();
            pipe.ring.Publish();
            return;
        }
        SotcPipeRunPacket(bytes);
        pipe.ring.Pop();
        ++run;
        pipe.progress.store(run, std::memory_order_release); // after its label write or commit
    }
}

// Session 64: the stall watchdog. Once B has run no packet for 3 s (and every 10 s after), it
// logs what A, B and C are blocked on and the GPU's real timeline value, so an intermittent
// hang (session 63's base1, session 64's c3: "flip queue full", then nothing) says whether the
// GPU or a CPU wait is stuck. No debugger needed.
void Rasterizer::SotcStallWatch(std::stop_token stop) {
    Common::SetCurrentThreadName("sotc:StallWatch");
    std::mutex m;
    std::condition_variable_any cv;
    std::unique_lock lk{m};
    u64 last = ~u64{0};
    u32 still = 0;
    for (;;) {
        cv.wait_for(lk, stop, std::chrono::seconds{1}, [] { return false; });
        if (stop.stop_requested()) {
            return;
        }
        const u64 p = sotc_pipe->progress.load(std::memory_order_relaxed);
        if (p != last) {
            last = p;
            still = 0;
            continue;
        }
        if (++still < 3 || (still - 3) % 10 != 0) {
            continue;
        }
        const u64 now = SotcWaitTimer::NowNs();
        const auto where = [now](const SotcWaitTimer::WhereSlot& w) {
            const char* name = w.name.load(std::memory_order_relaxed);
            if (!name) {
                return std::string{"running (no wait marked)"};
            }
            return fmt::format("{} for {:.1f} s", name,
                               (now - w.since_ns.load(std::memory_order_relaxed)) / 1e9);
        };
        const auto* sem = scheduler.GetWorkSemaphore();
        const auto [res, gpu_value] = instance.GetDevice().getSemaphoreCounterValue(sem->Handle());
        LOG_WARNING(Render_Vulkan,
                    "stall: B ran no packet for {} s (at packet {}). B: {}; A/guest: {}; "
                    "timeline: B's tick {}, known GPU {}, GPU now {} ({}); {}",
                    still, p, where(SotcWaitTimer::b_where), where(SotcWaitTimer::a_where),
                    sem->CurrentTick(), sem->KnownGpuTick(), gpu_value, vk::to_string(res),
                    scheduler.SotcCmd().StallState());
    }
}

void Rasterizer::SotcPipeWait(SotcDrainReason reason) {
    if (!sotc_pipe || !sotc_pipe->threaded ||
        std::this_thread::get_id() != liverpool->GetGpuCommandProcessorThread()) {
        return;
    }
    auto& pipe = *sotc_pipe;
    const size_t i = size_t(reason);
    ++pipe.drains[i];
    if (pipe.retired.load(std::memory_order_acquire) == pipe.submitted) {
        return;
    }
    SOTC_ZONE("SotcPipeDrain wait");
    ++pipe.drain_waits[i];
    const auto t0 = std::chrono::steady_clock::now();
    // A pushed packets while it owned B's state: B is blocked on state_mutex, so let it run.
    const bool owned = pipe.own_depth > 0;
    if (owned) {
        pipe.state_mutex.unlock();
    }
    SotcWaitTimer::Where w{SotcWaitTimer::a_where, "pipe drain"}; // session 64
    pipe.drain_waiting.store(true, std::memory_order_seq_cst);
    for (u64 r = pipe.retired.load(std::memory_order_seq_cst); r != pipe.submitted;
         r = pipe.retired.load(std::memory_order_seq_cst)) {
        pipe.retired.wait(r, std::memory_order_acquire);
    }
    pipe.drain_waiting.store(false, std::memory_order_relaxed);
    if (owned) {
        pipe.state_mutex.lock();
    }
    pipe.drain_ns[i] += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count());
}

// Session 46: wait for B, then own its state until the guard goes (B may run a command when
// it is idle; it can't while A holds state_mutex).
Rasterizer::SotcPipeOwner Rasterizer::SotcPipeDrain(SotcDrainReason reason) {
    if (!sotc_pipe || !sotc_pipe->threaded ||
        std::this_thread::get_id() != liverpool->GetGpuCommandProcessorThread()) {
        return {};
    }
    SotcPipeWait(reason);
    auto& pipe = *sotc_pipe;
    if (pipe.own_depth++ == 0) {
        if (!pipe.state_mutex.try_lock()) {
            SOTC_ZONE("SotcPipeDrain command wait"); // B is running a command
            const auto t0 = std::chrono::steady_clock::now();
            SotcWaitTimer::Where w{SotcWaitTimer::a_where, "B's command"}; // session 64
            pipe.state_mutex.lock();
            pipe.drain_ns[size_t(reason)] += u64(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
        }
    }
    return SotcPipeOwner{this};
}

void Rasterizer::SotcPipeRelease() {
    auto& pipe = *sotc_pipe;
    if (--pipe.own_depth == 0) {
        pipe.state_mutex.unlock();
    }
}

bool Rasterizer::SotcPipeCommand(Common::UniqueFunction<void>&& func) {
    if (!sotc_pipe || !sotc_pipe->threaded) {
        return false;
    }
    auto& pipe = *sotc_pipe;
    {
        std::scoped_lock lk{pipe.command_mutex};
        pipe.commands.push_back(std::move(func));
    }
    pipe.num_commands.fetch_add(1, std::memory_order_seq_cst);
    pipe.ring.Kick();
    return true;
}

// B, between packets and on waking: the queued commands, oldest first.
void Rasterizer::SotcPipeRunCommands() {
    auto& pipe = *sotc_pipe;
    while (pipe.num_commands.load(std::memory_order_acquire) != 0) {
        Common::UniqueFunction<void> func;
        {
            std::scoped_lock lk{pipe.command_mutex};
            func = std::move(pipe.commands.front());
            pipe.commands.pop_front();
        }
        pipe.num_commands.fetch_sub(1, std::memory_order_relaxed);
        SOTC_ZONE("SotcPipe command");
        func();
        pipe.commands_run.fetch_add(1, std::memory_order_relaxed);
    }
}

// Session 45: A hands B a submit-done, idle or flip.
bool Rasterizer::SotcPipeDefer(SotcPipeEvent event) {
    if (!sotc_pipe || !sotc_pipe->threaded) {
        return false;
    }
    auto& pipe = *sotc_pipe;
    pipe.writer.Clear();
    pipe.writer.Put(SotcPipeArgs{.kind = SotcPipeKind::Event, .size = u32(event)});
    pipe.ring.Push(pipe.writer.Bytes());
    ++pipe.submitted;
    ++pipe.events;
    return true;
}

// Session 45: A hands B a fence packet (a copy: B never reads PM4 memory, which the guest may
// free once the fence signals) and notes the label write it will make.
bool Rasterizer::SotcPipeFence(const AmdGpu::PM4Header* header, u32 pipe_id) {
    if (!sotc_pipe || !sotc_pipe->threaded) {
        return false;
    }
    auto& pipe = *sotc_pipe;
    const u64 seq = pipe.submitted + 1;
    auto& pending = pipe.pending_fences;
    pending.Retire(std::max(pipe.retired.load(std::memory_order_acquire),
                            pipe.progress.load(std::memory_order_acquire)));
    const auto note = [&](u64 addr, u32 bytes, AmdGpu::DataSelect sel, u64 value) {
        const bool known =
            sel == AmdGpu::DataSelect::Data32Low || sel == AmdGpu::DataSelect::Data64;
        pending.Add(seq, addr, bytes, known ? value : 0, known);
    };
    switch (header->type3.opcode.Value()) {
    case AmdGpu::PM4ItOpcode::EventWriteEop: {
        const auto* eop = reinterpret_cast<const AmdGpu::PM4CmdEventWriteEop*>(header);
        const auto sel = eop->data_sel.Value();
        if (sel != AmdGpu::DataSelect::None) {
            note(reinterpret_cast<u64>(eop->Address<u32>()),
                 sel == AmdGpu::DataSelect::Data32Low ? 4 : 8, sel, eop->DataQWord());
        }
        break;
    }
    case AmdGpu::PM4ItOpcode::EventWriteEos: {
        const auto* eos = reinterpret_cast<const AmdGpu::PM4CmdEventWriteEos*>(header);
        const bool gds = eos->command == AmdGpu::PM4CmdEventWriteEos::Command::GdsStore;
        note(eos->Address<u64>(), 4,
             gds ? AmdGpu::DataSelect::GdsMemStore : AmdGpu::DataSelect::Data32Low,
             eos->DataDWord());
        break;
    }
    case AmdGpu::PM4ItOpcode::ReleaseMem: {
        const auto* rel = reinterpret_cast<const AmdGpu::PM4CmdReleaseMem*>(header);
        const auto sel = rel->data_sel.Value();
        note(rel->Address<u64>(),
             sel == AmdGpu::DataSelect::Data32Low     ? 4
             : sel == AmdGpu::DataSelect::GdsMemStore ? u32(rel->num_dw) * 4
                                                      : 8,
             sel, rel->DataQWord());
        break;
    }
    default:
        return false;
    }
    const u32 dwords = header->type3.NumWords() + 1;
    pipe.writer.Clear();
    pipe.writer.Put(SotcPipeArgs{.kind = SotcPipeKind::Fence, .offset = pipe_id, .size = dwords});
    pipe.writer.PutRaw(header, dwords * sizeof(u32));
    pipe.ring.Push(pipe.writer.Bytes());
    ++pipe.submitted;
    ++pipe.fences;
    return true;
}

bool Rasterizer::SotcPipeWaitMet(const AmdGpu::PM4CmdWaitRegMem& wait) {
    if (!sotc_pipe || !sotc_pipe->threaded || sotc_pipe->pending_fences.Empty() ||
        wait.mem_space.Value() != AmdGpu::PM4CmdWaitRegMem::MemSpace::Memory) {
        return false;
    }
    auto& pipe = *sotc_pipe;
    auto& pending = pipe.pending_fences;
    pending.Retire(std::max(pipe.retired.load(std::memory_order_acquire),
                            pipe.progress.load(std::memory_order_acquire)));
    u32 value = 0;
    switch (pending.Find(reinterpret_cast<u64>(wait.Address()), &value)) {
    case SotcPendingFences::Lookup::None:
        return false; // poll memory: the CPU writes it, or B already did
    case SotcPendingFences::Lookup::Value:
        if (wait.TestValue(value)) {
            ++pipe.waits_met;
            return true;
        }
        break;
    case SotcPendingFences::Lookup::Unknown:
        break;
    }
    SotcPipeWait(SotcDrainReason::WaitFence); // then memory holds what B wrote
    pending.Retire(pipe.submitted);
    return false;
}

bool Rasterizer::SotcFaultOnGpuSide() {
    const auto id = std::this_thread::get_id();
    const bool threaded = sotc_pipe && sotc_pipe->threaded;
    if (id == liverpool->GetGpuCommandProcessorThread()) {
        // Session 46: inline only while A owns B's state (B is blocked then, so a queued fault
        // would never run); otherwise queued to B, handled between packets.
        return !threaded || sotc_pipe->own_depth > 0;
    }
    return threaded && id == sotc_pipe->record_thread.get_id();
}

void Rasterizer::SotcNoteQueuedFault(std::chrono::steady_clock::time_point start) {
    if (!sotc_pipe || std::this_thread::get_id() != liverpool->GetGpuCommandProcessorThread()) {
        return;
    }
    ++sotc_pipe->a_faults;
    sotc_pipe->a_fault_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count());
}

// Session 47: A's view of pending writes (see vk_rasterizer.h).
const u8* Rasterizer::SotcPipeOverlay(VAddr addr, u64 size) {
    if (!sotc_pipe || !sotc_pipe->threaded || sotc_pipe->pending.Empty() || size == 0 ||
        std::this_thread::get_id() != liverpool->GetGpuCommandProcessorThread()) {
        return nullptr;
    }
    auto& pipe = *sotc_pipe;
    const u8* at = nullptr;
    switch (pipe.pending.Find(addr, size, pipe.a_view, &at)) {
    case SotcPendingRanges::Read::None:
        return nullptr;
    case SotcPendingRanges::Read::Overlay:
        ++pipe.overlay_reads;
        return at;
    case SotcPendingRanges::Read::Wait:
        break;
    }
    SotcPipeWait(SotcDrainReason::Pending); // then guest memory and B's ranges are current
    pipe.a_view = pipe.submitted;
    return nullptr;
}

void Rasterizer::SotcPipeMergeRead(VAddr addr, u8* dst, u64 size) {
    if (!sotc_pipe || !sotc_pipe->threaded || sotc_pipe->pending.Empty() || size == 0 ||
        std::this_thread::get_id() != liverpool->GetGpuCommandProcessorThread()) {
        return;
    }
    auto& pipe = *sotc_pipe;
    if (pipe.pending.Merge(addr, dst, size, pipe.a_view)) {
        return;
    }
    SotcPipeWait(SotcDrainReason::Pending);
    pipe.a_view = pipe.submitted;
    memory->CopySparseMemory(addr, dst, size);
}

// Session 47, A: the buffer ranges packet `seq` may write (its written bindings, clamped as
// BindBuffers clamps them), except the CPU copy's destination at `skip` (its overlay).
void Rasterizer::SotcPipeNoteWrites(const Pipeline* pipeline, u64 seq, VAddr skip) {
    auto& pipe = *sotc_pipe;
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        for (const auto& desc : stage->buffers) {
            if (!desc.is_written || desc.IsSpecial()) {
                continue;
            }
            const auto vsharp = desc.GetSharp(*stage);
            if (vsharp.base_address == 0 || vsharp.base_address == skip ||
                vsharp.GetSize() == 0) {
                continue;
            }
            pipe.pending.AddWrite(seq, vsharp.base_address,
                                  memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize()));
        }
    }
}

// Session 47, B (SOTC_GPU_PIPE_VERIFY): re-runs each stage's SRT walker now, in stream order
// (B's faults download GPU data, B's commits are in guest memory), and compares the flattened
// user data with what A's walk produced. A difference means A read memory a packet before it
// changed (a pending write the overlay or the pending set missed). Dwords the GPU patches
// (FlatPatches: A read the CPU copy on purpose) are skipped.
void Rasterizer::SotcPipeVerifyWalk(std::span<const Shader::UdSnapshot> snaps) {
    auto& pipe = *sotc_pipe;
    thread_local std::vector<u32> flat;
    for (const auto& snap : snaps) {
        const auto* info = snap.info;
        if (!info || !info->srt_info.walker_func || snap.flat.empty()) {
            continue;
        }
        flat.assign(snap.flat.size(), 0);
        std::memcpy(flat.data(), snap.user_data.data(),
                    std::min(snap.user_data.size_bytes(), flat.size() * sizeof(u32)));
        Shader::SrtSetWalkInfo(info);
        info->srt_info.walker_func(snap.user_data.data(), flat.data(), &Shader::g_srt_helpers);
        Shader::SrtSetWalkInfo(nullptr);
        pipe.verify_walks.fetch_add(1, std::memory_order_relaxed);
        u32 diffs = 0, first = 0;
        for (u32 i = 0; i < flat.size(); ++i) {
            if (flat[i] == snap.flat[i]) {
                continue;
            }
            const bool patched = std::ranges::any_of(
                snap.patches, [i](const auto& p) { return p.first == i; });
            if (!patched && diffs++ == 0) {
                first = i;
            }
        }
        if (diffs == 0) {
            continue;
        }
        if (pipe.verify_walk_diffs.fetch_add(1, std::memory_order_relaxed) < 32) {
            LOG_ERROR(Render_Vulkan,
                      "pipe verify: packet {}: shader {:#x} walker: {} flattened dwords differ "
                      "(first {}: B {:#x}, A {:#x})",
                      pipe.packets, info->pgm_hash, diffs, first, flat[first], snap.flat[first]);
        }
    }
}

// A, every 4,096 packets: every 10 s, one line with the packets, the drains by reason (calls /
// calls that waited / ms waited), and how long B slept on an empty ring.
void Rasterizer::SotcPipeReport() {
    auto& pipe = *sotc_pipe;
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - pipe.report_start).count();
    if (secs < 10.0) {
        return;
    }
    static constexpr const char* Names[] = {"fence", "submit", "flush",    "finish",
                                            "gds",   "dma",    "marker",   "command",
                                            "flip",  "idle",   "cpu-copy", "fault",
                                            "wait-fence", "pending"};
    static_assert(std::size(Names) == SotcPipe::NumReasons);
    std::string by_reason;
    u64 total = 0, waits = 0, ns = 0;
    for (size_t i = 0; i < SotcPipe::NumReasons; ++i) {
        total += pipe.drains[i];
        waits += pipe.drain_waits[i];
        ns += pipe.drain_ns[i];
        if (pipe.drains[i]) {
            by_reason += fmt::format(" {} {}/{}/{:.1f}", Names[i], pipe.drains[i],
                                     pipe.drain_waits[i], pipe.drain_ns[i] / 1e6);
        }
    }
    const u64 b_idle = pipe.b_idle_ns.load(std::memory_order_relaxed);
    const u64 commands = pipe.commands_run.load(std::memory_order_relaxed);
    LOG_INFO(Render_Vulkan,
             "pipe: {:.1f} s: {} packets ({} fences, {} events), {} drains ({} waited, {:.1f} "
             "ms), B idle {:.1f} ms, {} waits met by pending fences, {} commands on B, {} A "
             "faults queued ({:.1f} ms);{}",
             secs, pipe.submitted - pipe.report_packets, pipe.fences, pipe.events, total, waits,
             ns / 1e6, (b_idle - pipe.report_b_idle_ns) / 1e6, pipe.waits_met,
             commands - pipe.report_commands, pipe.a_faults, pipe.a_fault_ns / 1e6, by_reason);
    LOG_INFO(Render_Vulkan,
             "pipe: A copies {} ({:.1f} MB), {} left to B, {} overlay reads, {} pending ranges "
             "({} big); verify: {} walks, {} differ",
             pipe.overlay_copies, pipe.overlay_bytes / 1e6, pipe.copy_fallbacks,
             pipe.overlay_reads, pipe.pending.Size(), pipe.pending.Big(),
             pipe.verify_walks.load(std::memory_order_relaxed),
             pipe.verify_walk_diffs.load(std::memory_order_relaxed));
    {
        // Session 49: B's wall time in kernel-entering Vulkan calls (sotc_wait_timer.h).
        std::string waits;
        for (int i = 0; i < SotcWaitTimer::NumSlots; ++i) {
            const u64 n = SotcWaitTimer::ns[i].exchange(0, std::memory_order_relaxed);
            const u64 c = SotcWaitTimer::calls[i].exchange(0, std::memory_order_relaxed);
            waits += fmt::format(" {} {}x {:.1f} ms,", SotcWaitTimer::Names[i], c, n / 1e6);
        }
        LOG_INFO(Render_Vulkan, "pipe: B waits:{} hot flushes {} (denied {})", waits,
                 SotcWaitTimer::hot_flushes.exchange(0, std::memory_order_relaxed),
                 SotcWaitTimer::hot_denied.exchange(0, std::memory_order_relaxed));
        // Session 58: every queue submit by caller.
        LOG_INFO(Render_Vulkan, "pipe: submits:{}", SotcWaitTimer::TakeSubmitSites());
        // Session 51: A's wall time outside its own work (beside the drains, queued faults and
        // pending reads above), and the guest threads' GPU page faults.
        const auto [full_waits, full_ns] = pipe.ring.TakeFullWaits();
        SotcWaitTimer::AddA(SotcWaitTimer::ARingFull, full_ns, full_waits);
        std::string a_waits;
        for (int i = 0; i < SotcWaitTimer::NumASlots; ++i) {
            const u64 n = SotcWaitTimer::a_ns[i].exchange(0, std::memory_order_relaxed);
            const u64 c = SotcWaitTimer::a_calls[i].exchange(0, std::memory_order_relaxed);
            a_waits += fmt::format(" {} {}x {:.1f} ms,", SotcWaitTimer::ANames[i], c, n / 1e6);
        }
        LOG_INFO(Render_Vulkan, "pipe: A waits:{}", a_waits);
    }
    if (const auto rb = buffer_cache.SotcReadbackReport(); !rb.empty()) {
        LOG_INFO(Render_Vulkan, "pipe: {}", rb); // session 50
    }
    if (const auto cmd = scheduler.SotcCmd().TakeReport(); !cmd.empty()) {
        LOG_INFO(Render_Vulkan, "pipe: {}", cmd); // session 62: SOTC_GPU_CMD
    }
    pipe.overlay_copies = 0;
    pipe.overlay_bytes = 0;
    pipe.copy_fallbacks = 0;
    pipe.overlay_reads = 0;
    pipe.report_commands = commands;
    pipe.a_faults = 0;
    pipe.a_fault_ns = 0;
    pipe.fences = 0;
    pipe.events = 0;
    pipe.waits_met = 0;
    pipe.drains = {};
    pipe.drain_waits = {};
    pipe.drain_ns = {};
    pipe.report_packets = pipe.submitted;
    pipe.report_b_idle_ns = b_idle;
    pipe.report_start = now;
}

void Rasterizer::SotcPipeRunPacket(std::span<const u8> bytes) {
    auto& pipe = *sotc_pipe;
    {
        SotcPacketReader r{bytes};
        const auto args = r.Get<SotcPipeArgs>();
        if (args.kind == SotcPipeKind::Fence) {
            // Session 45: A's copy of the PM4 packet (offset = the compute queue's pipe id).
            liverpool->SotcRunFence(
                reinterpret_cast<const AmdGpu::PM4Header*>(r.Take(args.size * sizeof(u32))),
                args.offset);
            return;
        }
        if (args.kind == SotcPipeKind::Event) {
            switch (static_cast<SotcPipeEvent>(args.size)) {
            case SotcPipeEvent::SubmitDone: {
                ZoneScopedNC("SubmitDone (OnSubmit + Flush)", 0x2a9d8f);
                OnSubmit();
                Flush();
                break;
            }
            case SotcPipeEvent::Idle:
                Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
                break;
            case SotcPipeEvent::Flip:
                SotcMemoProbe::OnFlip(); // W1
                if (sotc_indirect_probe) { // session 66
                    sotc_indirect_probe->OnFlip();
                }
                Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
                break;
            }
            return;
        }
        SotcApplyRegDelta<AmdGpu::Regs::NumRegs>(r, pipe.regs.reg_array.data());
        pipe.cb_extent = r.Get<decltype(pipe.cb_extent)>();
        pipe.db_extent = r.Get<AmdGpu::CbDbExtent>();
        if (args.IsCompute()) {
            pipe.cs = r.Get<AmdGpu::ComputeProgram>();
        }
        const auto* pipeline = r.Get<const Pipeline*>();
        boost::container::static_vector<Shader::UdSnapshot, Shader::MaxStageTypes> snaps;
        if (pipeline) {
            const auto num_stages = r.Get<u64>();
            for (u64 i = 0; i < num_stages; ++i) {
                auto& snap = snaps.emplace_back();
                snap.info = r.Get<const Shader::Info*>();
                snap.pgm_base = r.Get<VAddr>();
                snap.user_data = r.GetSpan<u32>();
                snap.flat = r.GetSpan<u32>();
                const auto num_patches = r.Get<u64>();
                snap.patches = {reinterpret_cast<const std::pair<u16, u64>*>(
                                    r.Take(num_patches * sizeof(std::pair<u16, u64>))),
                                num_patches};
            }
        }
        SotcCopyResult copy{};
        std::span<const u32> copy_words;
        if (args.IsCompute()) {
            if (const auto count = r.Get<u64>()) {
                copy = r.Get<SotcCopyResult>();
                copy_words = {reinterpret_cast<const u32*>(r.Take(count * sizeof(u32))),
                              size_t(count)};
            }
        }
        ++pipe.packets;
        if (args.verify) {
            // Liverpool's register file as A sent this packet.
            const auto* live = reinterpret_cast<const u32*>(r.Take(sizeof(pipe.regs.reg_array)));
            u32 diffs = 0, first = 0;
            for (u32 i = 0; i < AmdGpu::Regs::NumRegs; ++i) {
                if (pipe.regs.reg_array[i] != live[i] && diffs++ == 0) {
                    first = i;
                }
            }
            if (diffs && pipe.verify_diffs++ < 32) {
                LOG_ERROR(Render_Vulkan,
                          "pipe verify: packet {}: {} register words differ (first {:#x}: "
                          "B {:#x}, A {:#x})",
                          pipe.packets, diffs, first, pipe.regs.reg_array[first], live[first]);
            }
        }
        if (args.verify && pipe.threaded) {
            SotcPipeVerifyWalk(snaps);
        }
        {
            Shader::SotcUdScope ud_scope{snaps};
            {
                SOTC_ZONE_N(pop_zone, "PopPendingOperations");
                scheduler.PopPendingOperations();
            }
            switch (args.kind) {
            case SotcPipeKind::Draw:
                if (args.filter.kind != SotcDrawFilter::Kind::Draw) {
                    RunDrawFilter(args.filter);
                } else {
                    RecordDraw(static_cast<const GraphicsPipeline*>(pipeline), args.is_indexed,
                               args.index_offset);
                }
                break;
            case SotcPipeKind::DrawIndirect:
                if (args.filter.kind != SotcDrawFilter::Kind::Draw) {
                    RunDrawFilter(args.filter);
                } else {
                    RecordDrawIndirect(static_cast<const GraphicsPipeline*>(pipeline),
                                       args.is_indexed, args.arg_address, args.offset,
                                       args.stride, args.max_count, args.count_address);
                }
                break;
            case SotcPipeKind::DispatchDirect:
                if (!copy_words.empty()) {
                    sotc_copy_commit = &copy;
                    sotc_copy_words = copy_words;
                }
                RecordDispatchDirect(static_cast<const ComputePipeline*>(pipeline));
                sotc_copy_commit = nullptr;
                sotc_copy_words = {};
                break;
            case SotcPipeKind::DispatchIndirect:
                RecordDispatchIndirect(static_cast<const ComputePipeline*>(pipeline),
                                       args.arg_address, args.offset, args.size);
                break;
            case SotcPipeKind::Stop:
                break;
            }
        }
    }
}
} // namespace Vulkan
