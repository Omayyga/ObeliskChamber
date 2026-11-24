// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "video_alloc_stats.h"

NXTEST_GROUP(gfx_video_alloc_stats)

using nxrt::gfx::VideoAllocStats;

NXTEST(no_figures_no_text) {
    VideoAllocStats s;
    char t[512] = "x";
    CHECK(nxrt::gfx::video_alloc_text(s, t, sizeof t) == 0);
    CHECK(t[0] == 0);
}

// The shape of #162: 9.2 GB from the driver, 4.3 GB of live images and buffers.
NXTEST(free_inside_blocks_and_outside) {
    VideoAllocStats s;
    s.valid = true;
    s.video = {6144ull << 20, 4096ull << 20, 90, 7000};
    s.other = {512ull << 20, 384ull << 20, 6, 40};
    s.small = {320ull << 20, 256ull << 20, 20, 5000};
    s.video_free_ranges = 1200;
    s.video_largest_free = 48ull << 20;
    s.driver_bytes = 9216ull << 20;
    s.rt_images = 300; s.rt_prefer = 280; s.rt_dedicated = 270;
    s.other_images = 7000; s.other_prefer = 0; s.other_dedicated = 12;
    CHECK(s.video.free_bytes() == (2048ull << 20));
    CHECK(nxrt::gfx::video_alloc_free_percent(s.video) == 33);
    CHECK(s.outside_bytes() == ((9216ull - 6144 - 512) << 20));
    char t[1024];
    CHECK(nxrt::gfx::video_alloc_text(s, t, sizeof t) > 0);
    std::printf("    %s\n", t);
    CHECK(std::strstr(t, "images made: render targets 300 (driver prefers memory of their own 280, given it 270), others 7000 (0, 12)"));
    CHECK(std::strstr(t, "video heap 90 blocks / 6144 MiB taken"));
    CHECK(std::strstr(t, "7000 allocations / 4096 MiB handed out"));
    CHECK(std::strstr(t, "2048 MiB free inside the blocks (33%, 1200 ranges, largest 48 MiB)"));
    CHECK(std::strstr(t, "small-texture pools 20 blocks / 320 MiB, 64 MiB free"));
    CHECK(std::strstr(t, "other heaps 512 MiB taken, 128 MiB free"));
    CHECK(std::strstr(t, "outside the allocator 2560 MiB"));
}

// A gauge read between two allocations can be behind the allocator: never negative.
NXTEST(driver_figure_behind) {
    VideoAllocStats s;
    s.valid = true;
    s.video = {100ull << 20, 120ull << 20, 2, 3};
    s.driver_bytes = 50ull << 20;
    CHECK(s.video.free_bytes() == 0);
    CHECK(s.outside_bytes() == 0);
    CHECK(nxrt::gfx::video_alloc_free_percent(nxrt::gfx::VideoAllocGroup{}) == 0);
}

// The defragmentation question: which blocks could moving textures empty?
NXTEST(blocks_by_content) {
    using nxrt::gfx::VideoAllocKind;
    using nxrt::gfx::VideoBlockEntry;
    const uint64_t M = 1ull << 20;
    std::vector<VideoBlockEntry> e = {
        // block 1, 32 MiB: textures only, 8 MiB used (under half)
        {1, 32 * M, 5 * M, VideoAllocKind::Texture}, {1, 32 * M, 3 * M, VideoAllocKind::Texture},
        // block 2, 32 MiB: a render target and a texture, 20 MiB used
        {2, 32 * M, 16 * M, VideoAllocKind::RenderTarget}, {2, 32 * M, 4 * M, VideoAllocKind::Texture},
        // block 3, 16 MiB (a small-texture pool): a buffer, 2 MiB used (under half)
        {3, 16 * M, 2 * M, VideoAllocKind::Buffer},
        // block 4, 32 MiB: textures only, 30 MiB used
        {4, 32 * M, 30 * M, VideoAllocKind::Texture},
    };
    std::swap(e[0], e[5]);   // out of order on purpose
    nxrt::gfx::VideoBlockStats s;
    nxrt::gfx::video_block_tally(e, &s);
    CHECK(s.valid);
    CHECK(s.allocs == 6);
    CHECK(s.all.blocks == 4);
    CHECK(s.all.block_bytes == 112 * M);
    CHECK(s.all.free_bytes() == 52 * M);
    CHECK(s.tex_only.blocks == 2);
    CHECK(s.tex_only.free_bytes() == 26 * M);
    CHECK(s.mixed.blocks == 2);
    CHECK(s.mixed.free_bytes() == 26 * M);
    CHECK(s.mixed.used[static_cast<int>(VideoAllocKind::RenderTarget)] == 16 * M);
    CHECK(s.sparse.blocks == 2);
    CHECK(s.sparse_tex.blocks == 1);
    CHECK(s.sparse_tex.free_bytes() == 24 * M);
    char t[1024];
    CHECK(nxrt::gfx::video_block_text(s, t, sizeof t) > 0);
    std::printf("    %s\n", t);
    CHECK(std::strstr(t, "4 blocks / 112 MiB, 6 allocations, 52 MiB free"));
    CHECK(std::strstr(t, "textures only 2 blocks / 64 MiB, 26 MiB free"));
    CHECK(std::strstr(t, "with other allocations 2 blocks / 48 MiB, 26 MiB free (render targets 16, storage images 0, buffers 2, textures 4 MiB in them)"));
    CHECK(std::strstr(t, "under half used 2 blocks / 48 MiB, 38 MiB free, of them textures only 1 blocks / 24 MiB free"));
}

NXTEST(blocks_none) {
    std::vector<nxrt::gfx::VideoBlockEntry> e;
    nxrt::gfx::VideoBlockStats s;
    nxrt::gfx::video_block_tally(e, &s);
    CHECK(s.valid && s.all.blocks == 0 && s.all.free_bytes() == 0);
    nxrt::gfx::VideoBlockStats none;
    char t[64] = "x";
    CHECK(nxrt::gfx::video_block_text(none, t, sizeof t) == 0);
}
