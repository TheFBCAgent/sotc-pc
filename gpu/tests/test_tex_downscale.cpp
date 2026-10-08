// SPDX-License-Identifier: GPL-2.0-or-later
// Session 16: VideoCore::SotcTexDownscale, which textures SOTC_TEX_DOWNSCALE creates one level
// smaller and how their sizes, view levels and minimum LOD map to the host image.
#include <cstdio>

#include "video_core/texture_cache/sotc_tex_downscale.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using namespace VideoCore::SotcTexDownscale;

int main() {
    // A 1024x1024 BC3 texture with 11 mips, sampled only: one level smaller.
    const Request tex{.sampled_only = true, .texel_access = false, .is_depth = false,
                      .is_volume = false, .num_samples = 1, .width = 1024, .height = 1024,
                      .levels = 11};
    CHECK(MipSkip(tex) == 1);
    // Anything else keeps full size.
    Request r = tex;
    r.sampled_only = false; // storage, render/depth target, video out
    CHECK(MipSkip(r) == 0);
    r = tex;
    r.texel_access = true; // a fetch or a size query in guest units
    CHECK(MipSkip(r) == 0);
    r = tex;
    r.is_depth = true;
    CHECK(MipSkip(r) == 0);
    r = tex;
    r.is_volume = true;
    CHECK(MipSkip(r) == 0);
    r = tex;
    r.num_samples = 4;
    CHECK(MipSkip(r) == 0);
    r = tex;
    r.levels = 1; // no mip to fall back to
    CHECK(MipSkip(r) == 0);
    r = tex;
    r.width = 256;
    r.height = 511; // largest side below 512
    CHECK(MipSkip(r) == 0);
    r.width = 512;
    r.height = 64; // a 512-wide strip qualifies
    CHECK(MipSkip(r) == 1);

    // Host extents follow Vulkan's mip rule, so host level k is the guest's mip k + 1.
    CHECK(HostDim(1024, 1) == 512);
    CHECK(HostDim(1, 1) == 1);
    CHECK(HostDim(1023, 1) == 511);
    CHECK(HostDim(1023, 0) == 1023);
    for (u32 w : {1024u, 1000u, 777u, 513u}) {
        for (u32 k = 0; k < 10; ++k) {
            CHECK(HostDim(HostDim(w, 1), k) == HostDim(w, k + 1));
        }
    }

    // View levels: the full chain 0..10 becomes host 0..9.
    CHECK((MapLevels(0, 11, 11, 1) == LevelRange{0, 10}));
    // A view of guest mips 3..10 reads host 2..9.
    CHECK((MapLevels(3, 8, 11, 1) == LevelRange{2, 8}));
    // A view of guest mip 0 only reads host level 0 (guest mip 1).
    CHECK((MapLevels(0, 1, 11, 1) == LevelRange{0, 1}));
    // Mips 0..1 both land on host 0.
    CHECK((MapLevels(0, 2, 11, 1) == LevelRange{0, 1}));
    // The last guest mip is host level 9.
    CHECK((MapLevels(10, 1, 11, 1) == LevelRange{9, 1}));
    // A range past the image is clamped into it.
    CHECK((MapLevels(9, 5, 11, 1) == LevelRange{8, 2}));
    CHECK((MapLevels(12, 1, 11, 1) == LevelRange{9, 1}));
    // A zero-level count still gives one level.
    CHECK((MapLevels(4, 0, 11, 1) == LevelRange{3, 1}));
    // No skip: unchanged.
    CHECK((MapLevels(3, 8, 11, 0) == LevelRange{3, 8}));

    // Minimum LOD in 1/256 levels.
    CHECK(MapMinLod(0, 1) == 0);
    CHECK(MapMinLod(128, 1) == 0);
    CHECK(MapMinLod(256, 1) == 0);
    CHECK(MapMinLod(3 * 256 + 64, 1) == 2 * 256 + 64);
    CHECK(MapMinLod(3 * 256, 0) == 3 * 256);

    if (g_failures == 0) {
        std::printf("test_tex_downscale: all passed\n");
    }
    return g_failures ? 1 : 0;
}
