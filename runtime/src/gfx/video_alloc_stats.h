// SPDX-License-Identifier: GPL-3.0-or-later
// What the video memory allocator (VMA) holds against what it has handed out. The driver-allocation gauge (vk.memory_bytes) grew over long
// sessions while the live images and buffers stayed flat (#155, #162): these figures say where the difference sits. Filled by
// gfx::Device::alloc_stats (vk_device.cpp); the text is here so a test can read it without a device.
#pragma once

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace nxrt::gfx {

struct VideoAllocGroup {
    uint64_t block_bytes = 0;   // memory taken from the driver (VMA's blocks and dedicated allocations)
    uint64_t alloc_bytes = 0;   // of it handed out to images and buffers
    uint32_t blocks = 0;
    uint32_t allocs = 0;
    uint64_t free_bytes() const { return block_bytes > alloc_bytes ? block_bytes - alloc_bytes : 0; }
};

struct VideoAllocStats {
    bool valid = false;
    VideoAllocGroup video;        // the largest device-local heap (small-texture pools included)
    VideoAllocGroup other;        // every other heap (host-visible memory: staging, mapped buffers)
    VideoAllocGroup small;        // of `video` and `other`: the small-texture pools (16 MiB blocks)
    uint32_t video_free_ranges = 0;       // free ranges inside the video heap's blocks
    uint64_t video_largest_free = 0;      // the largest of them
    uint64_t driver_bytes = 0;            // every vkAllocateMemory alive (the gauge), the allocator's and the others'
    // Images made so far, by kind: render targets (colour or depth attachments) and the rest (sampled, storage). For each, how many the
    // driver said it would rather have in memory of their own (VkMemoryDedicatedRequirements::prefersDedicatedAllocation) and how many
    // the allocator gave memory of their own (that, or over half a block). #155: on NVIDIA most of the video heap is in such allocations,
    // on the RX 6600 XT almost none; an allocation of its own goes back to the driver the moment its image goes.
    uint64_t rt_images = 0, rt_prefer = 0, rt_dedicated = 0;
    uint64_t other_images = 0, other_prefer = 0, other_dedicated = 0;
    // Not from the allocator: host-memory imports of the game's pools, sparse texture chunks, the upscalers' and the overlay's own.
    uint64_t outside_bytes() const {
        const uint64_t in = video.block_bytes + other.block_bytes;
        return driver_bytes > in ? driver_bytes - in : 0;
    }
};

inline int video_alloc_free_percent(const VideoAllocGroup& g) {
    return g.block_bytes ? static_cast<int>((g.free_bytes() * 100 + g.block_bytes / 2) / g.block_bytes) : 0;
}

// One log line. Returns the length written (0: no figures).
inline int video_alloc_text(const VideoAllocStats& s, char* out, size_t n) {
    if (!out || !n) return 0;
    out[0] = 0;
    if (!s.valid) return 0;
    const double mib = 1048576.0;
    return std::snprintf(out, n,
                         "allocator: video heap %u blocks / %.0f MiB taken, %u allocations / %.0f MiB handed out, %.0f MiB free inside the blocks (%d%%, "
                         "%u ranges, largest %.0f MiB); of it small-texture pools %u blocks / %.0f MiB, %.0f MiB free; other heaps %.0f MiB taken, "
                         "%.0f MiB free; outside the allocator %.0f MiB; images made: render targets %llu (driver prefers memory of their own %llu, "
                         "given it %llu), others %llu (%llu, %llu)",
                         s.video.blocks, s.video.block_bytes / mib, s.video.allocs, s.video.alloc_bytes / mib, s.video.free_bytes() / mib,
                         video_alloc_free_percent(s.video), s.video_free_ranges, s.video_largest_free / mib, s.small.blocks, s.small.block_bytes / mib,
                         s.small.free_bytes() / mib, s.other.block_bytes / mib, s.other.free_bytes() / mib, s.outside_bytes() / mib,
                         static_cast<unsigned long long>(s.rt_images), static_cast<unsigned long long>(s.rt_prefer),
                         static_cast<unsigned long long>(s.rt_dedicated), static_cast<unsigned long long>(s.other_images),
                         static_cast<unsigned long long>(s.other_prefer), static_cast<unsigned long long>(s.other_dedicated));
}

// ---- What sits in the allocator's blocks (#155, the defragmentation plan) ---------------------------------------------------------------
// Video memory comes back to the driver only when a block is empty. Moving sampled textures (what residency's demotion already does) can
// empty a block only if the block holds nothing else, so the census counts, per block of the video heap, the bytes of each kind. Filled
// by gfx::Device::block_stats from every live allocation that sits in a block (allocations of their own are left out).

enum class VideoAllocKind : uint8_t { Texture = 0, RenderTarget, Storage, Buffer };
constexpr int kVideoAllocKinds = 4;

struct VideoBlockEntry {
    uint64_t block = 0;        // the block's VkDeviceMemory, as a number
    uint64_t block_size = 0;
    uint64_t size = 0;
    VideoAllocKind kind = VideoAllocKind::Texture;
};

struct VideoBlockGroup {
    uint32_t blocks = 0;
    uint64_t block_bytes = 0;
    uint64_t used[kVideoAllocKinds] = {};
    uint64_t used_bytes() const {
        uint64_t u = 0;
        for (uint64_t v : used) u += v;
        return u;
    }
    uint64_t free_bytes() const { return block_bytes > used_bytes() ? block_bytes - used_bytes() : 0; }
    void add(uint64_t size, const uint64_t (&u)[kVideoAllocKinds]) {
        ++blocks;
        block_bytes += size;
        for (int k = 0; k < kVideoAllocKinds; ++k) used[k] += u[k];
    }
};

struct VideoBlockStats {
    bool valid = false;
    uint64_t allocs = 0;          // allocations seen in blocks
    VideoBlockGroup all;
    VideoBlockGroup tex_only;     // blocks holding sampled textures and nothing else: moving textures can empty them
    VideoBlockGroup mixed;        // blocks holding a render target, a storage image or a buffer as well
    VideoBlockGroup sparse;       // blocks under half used (either kind)
    VideoBlockGroup sparse_tex;   // of them, textures only
};

// Groups the entries by block. Sorts `e` in place.
inline void video_block_tally(std::vector<VideoBlockEntry>& e, VideoBlockStats* out) {
    *out = VideoBlockStats{};
    std::sort(e.begin(), e.end(), [](const VideoBlockEntry& a, const VideoBlockEntry& b) { return a.block < b.block; });
    for (size_t i = 0; i < e.size();) {
        const uint64_t blk = e[i].block;
        uint64_t size = 0, used = 0;
        uint64_t u[kVideoAllocKinds] = {};
        for (; i < e.size() && e[i].block == blk; ++i) {
            size = std::max(size, e[i].block_size);
            u[static_cast<int>(e[i].kind)] += e[i].size;
            used += e[i].size;
            ++out->allocs;
        }
        const bool tex = used == u[static_cast<int>(VideoAllocKind::Texture)];
        out->all.add(size, u);
        (tex ? out->tex_only : out->mixed).add(size, u);
        if (used * 2 < size) {
            out->sparse.add(size, u);
            if (tex) out->sparse_tex.add(size, u);
        }
    }
    out->valid = true;
}

// One log line. Returns the length written (0: no figures).
inline int video_block_text(const VideoBlockStats& s, char* out, size_t n) {
    if (!out || !n) return 0;
    out[0] = 0;
    if (!s.valid) return 0;
    const double mib = 1048576.0;
    auto u = [](const VideoBlockGroup& g, VideoAllocKind k) { return g.used[static_cast<int>(k)] / 1048576.0; };
    return std::snprintf(out, n,
                         "allocator blocks by content: %u blocks / %.0f MiB, %llu allocations, %.0f MiB free (in use: textures %.0f, render targets %.0f, "
                         "storage images %.0f, buffers %.0f MiB); textures only %u blocks / %.0f MiB, %.0f MiB free; with other allocations %u blocks / "
                         "%.0f MiB, %.0f MiB free (render targets %.0f, storage images %.0f, buffers %.0f, textures %.0f MiB in them); under half used %u "
                         "blocks / %.0f MiB, %.0f MiB free, of them textures only %u blocks / %.0f MiB free",
                         s.all.blocks, s.all.block_bytes / mib, static_cast<unsigned long long>(s.allocs), s.all.free_bytes() / mib,
                         u(s.all, VideoAllocKind::Texture), u(s.all, VideoAllocKind::RenderTarget), u(s.all, VideoAllocKind::Storage),
                         u(s.all, VideoAllocKind::Buffer), s.tex_only.blocks, s.tex_only.block_bytes / mib, s.tex_only.free_bytes() / mib, s.mixed.blocks,
                         s.mixed.block_bytes / mib, s.mixed.free_bytes() / mib, u(s.mixed, VideoAllocKind::RenderTarget),
                         u(s.mixed, VideoAllocKind::Storage), u(s.mixed, VideoAllocKind::Buffer), u(s.mixed, VideoAllocKind::Texture), s.sparse.blocks,
                         s.sparse.block_bytes / mib, s.sparse.free_bytes() / mib, s.sparse_tex.blocks, s.sparse_tex.free_bytes() / mib);
}

}  // namespace nxrt::gfx
