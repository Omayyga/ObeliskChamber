// SPDX-License-Identifier: GPL-3.0-or-later
// [gfx] gpu_checkpoints (default off): NVIDIA's diagnostic checkpoints (VK_NV_device_diagnostic_checkpoints) for device losses.
//
// #121: an RTX 5060 Ti loses the device in one cutscene every time, with VK_EXT_device_fault reporting only "instruction pointer (invalid)":
// the GPU jumped to an address that is not shader code, and nothing said which shader. With this on, the replay puts a marker before every
// draw and dispatch (vkCmdSetCheckpointNV; through the deferred-recording queue, so the order is kept). After a loss the driver tells, per
// pipeline stage, the last marker the GPU reached; each marker is a number into a ring of records here that holds the draw's or dispatch's
// shader hashes. The report then names the work the GPU had started (top of pipe) but not finished (bottom of pipe), and the records between.
// A marker per draw costs some GPU and render-thread time, so this is for test builds and reports, not for play.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <volk.h>

#include "nxrt/config.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include "nvn_gpu_internal.h"
#include "nvn_checkpoints.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "nvn.ckpt";
constexpr uint64_t kRing = 1u << 16;   // records kept: far more than the draws of the frames in flight

struct Rec {
    std::atomic<uint64_t> seq{0};      // written last: a reader trusts the record only when this is the marker it looks for
    uint64_t present = 0;              // presents at the time it was recorded
    char kind = 0;                     // 'd' draw, 'c' dispatch
    char a[20] = {}, b[20] = {}, c[20] = {};   // vertex, fragment, geometry (draws) or compute (dispatches) shader hashes
};

Rec* g_ring = nullptr;
uint64_t g_seq = 0;                    // render thread
int g_state = -1;                      // -1 not decided, 0 off, 1 on

void copy_hash(char (&dst)[20], const char* src) {
    if (!src) { dst[0] = '-'; dst[1] = 0; return; }
    std::strncpy(dst, src, sizeof dst - 1);
    dst[sizeof dst - 1] = 0;
}

const char* stage_name(VkPipelineStageFlagBits s) {
    switch (s) {
        case VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT: return "top of pipe (started)";
        case VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT: return "bottom of pipe (finished)";
        default: return "stage";
    }
}

std::string describe(uint64_t seq) {
    char b[200];
    const Rec& r = g_ring[seq % kRing];
    if (!seq || r.seq.load(std::memory_order_acquire) != seq) {
        std::snprintf(b, sizeof b, "marker %llu (no longer recorded)", static_cast<unsigned long long>(seq));
        return b;
    }
    if (r.kind == 'c')
        std::snprintf(b, sizeof b, "marker %llu: dispatch, cs %s (present %llu)", static_cast<unsigned long long>(seq), r.a,
                      static_cast<unsigned long long>(r.present));
    else
        std::snprintf(b, sizeof b, "marker %llu: draw, vs %s fs %s%s%s (present %llu)", static_cast<unsigned long long>(seq), r.a, r.b,
                      r.c[0] && r.c[0] != '-' ? " gs " : "", r.c[0] && r.c[0] != '-' ? r.c : "", static_cast<unsigned long long>(r.present));
    return b;
}

// The device-lost report's section: called once, on the thread that saw the loss.
std::string report() {
    if (!g_ring || !vkGetQueueCheckpointDataNV) return "";
    const VkQueue q = device().queue();
    uint32_t n = 0;
    vkGetQueueCheckpointDataNV(q, &n, nullptr);
    std::string out = "  GPU checkpoints ([gfx] gpu_checkpoints):\n";
    if (!n) return out + "    the driver reports none\n";
    std::vector<VkCheckpointDataNV> d(n, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
    vkGetQueueCheckpointDataNV(q, &n, d.data());
    uint64_t started = 0, finished = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t seq = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d[i].pCheckpointMarker));
        out += "    ";
        out += stage_name(d[i].stage);
        out += ": ";
        out += describe(seq);
        out += "\n";
        if (d[i].stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) started = std::max(started, seq);
        if (d[i].stage == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) finished = std::max(finished, seq);
    }
    // the work between the last finished marker and the last started one was on the GPU when it failed
    if (started > finished) {
        const uint64_t first = std::max(finished + 1, started > 24 ? started - 24 : 1);
        out += "    in flight (started, not finished):\n";
        for (uint64_t s = first; s <= started; ++s) out += "      " + describe(s) + "\n";
    }
    return out;
}

bool on() {
    if (g_state < 0) {
        g_state = 0;
        if (nxrt::config_get_bool("gfx", "gpu_checkpoints", false)) {
            if (device().features().gpu_checkpoints && vkCmdSetCheckpointNV && vkGetQueueCheckpointDataNV) {
                g_ring = new Rec[kRing];
                g_state = 1;
                gfx::set_device_lost_extra(&report);
                NXLOG_I(kTag, "GPU checkpoints on: a marker before every draw and dispatch, named in the device-lost report ([gfx] gpu_checkpoints)");
            } else {
                NXLOG_W(kTag, "[gfx] gpu_checkpoints is on, but this driver does not offer VK_NV_device_diagnostic_checkpoints (NVIDIA only): off");
            }
        }
    }
    return g_state == 1;
}

void mark(VkCommandBuffer cb, char kind, const char* a, const char* b, const char* c) {
    const uint64_t seq = ++g_seq;
    Rec& r = g_ring[seq % kRing];
    r.seq.store(0, std::memory_order_relaxed);
    r.kind = kind;
    r.present = counters().presents.load(std::memory_order_relaxed);
    copy_hash(r.a, a);
    copy_hash(r.b, b);
    copy_hash(r.c, c);
    r.seq.store(seq, std::memory_order_release);
    vkCmdSetCheckpointNV(cb, reinterpret_cast<const void*>(static_cast<uintptr_t>(seq)));
}

}  // namespace

void ckpt_draw(VkCommandBuffer cb, const char* vs, const char* fs, const char* gs) {
    if (on()) mark(cb, 'd', vs, fs, gs);
}

void ckpt_dispatch(VkCommandBuffer cb, const char* cs) {
    if (on()) mark(cb, 'c', cs, nullptr, nullptr);
}

}  // namespace nxrt::nvn::be::gpu
