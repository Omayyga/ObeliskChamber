// SPDX-License-Identifier: GPL-3.0-or-later
// [sched] cpu_limit: the process held to N physical cores for performance tests (nxrt/cpu_limit.h).
#include "nxrt/cpu_limit.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt {
namespace {

constexpr const char* kTag = "sched";

struct Core {
    uint64_t mask = 0;     // its hardware threads in processor group 0
    int eff = 0;           // EfficiencyClass: higher is faster (P-cores above E-cores on a hybrid CPU; 0 everywhere on others)
};

std::atomic<bool> g_active{false};
std::atomic<unsigned> g_threads{0};
std::atomic<int> g_cores{0};

std::vector<Core> cores_of_group0(int* groups) {
    std::vector<Core> out;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (!len) return out;
    std::string buf(len, '\0');
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len))
        return out;
    int other_groups = 0;
    for (DWORD off = 0; off < len;) {
        auto* e = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
        if (e->Relationship == RelationProcessorCore) {
            const GROUP_AFFINITY& g = e->Processor.GroupMask[0];
            if (g.Group == 0) out.push_back({static_cast<uint64_t>(g.Mask), static_cast<int>(e->Processor.EfficiencyClass)});
            else ++other_groups;
        }
        off += e->Size;
    }
    if (groups) *groups = other_groups;
    return out;
}

int popcount64(uint64_t v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

}  // namespace

void cpu_limit_apply() {
    const int64_t want = config_get_int("sched", "cpu_limit", 0);
    if (want <= 0) return;
    const bool smt = config_get_bool("sched", "cpu_limit_smt", true);
    std::string kind = config_get_str("sched", "cpu_limit_cores", "fast");
    for (char& c : kind) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool slow = kind == "slow";
    if (kind != "fast" && kind != "slow") NXLOG_W(kTag, "[sched] cpu_limit_cores = %s is not fast or slow: fast", kind.c_str());

    int other_groups = 0;
    std::vector<Core> cores = cores_of_group0(&other_groups);
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    for (Core& c : cores) c.mask &= static_cast<uint64_t>(proc);            // only processors the process may already use
    cores.erase(std::remove_if(cores.begin(), cores.end(), [](const Core& c) { return c.mask == 0; }), cores.end());
    if (cores.empty()) {
        NXLOG_W(kTag, "[sched] cpu_limit = %lld: the processor list is not available, no limit", static_cast<long long>(want));
        return;
    }
    // fastest (or slowest) first; within a class, the lower processor numbers first as Windows numbers them
    std::stable_sort(cores.begin(), cores.end(), [&](const Core& a, const Core& b) { return slow ? a.eff < b.eff : a.eff > b.eff; });
    const size_t n = std::min(cores.size(), static_cast<size_t>(want));
    uint64_t mask = 0;
    int kept_eff_min = cores[0].eff, kept_eff_max = cores[0].eff;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t m = smt ? cores[i].mask : cores[i].mask & (~cores[i].mask + 1);   // one hardware thread: the core's first
        mask |= m;
        kept_eff_min = std::min(kept_eff_min, cores[i].eff);
        kept_eff_max = std::max(kept_eff_max, cores[i].eff);
    }
    if (!SetProcessAffinityMask(GetCurrentProcess(), static_cast<DWORD_PTR>(mask))) {
        NXLOG_W(kTag, "[sched] cpu_limit = %lld: SetProcessAffinityMask(%llx) failed (%lu), no limit", static_cast<long long>(want),
                static_cast<unsigned long long>(mask), GetLastError());
        return;
    }
    g_threads.store(static_cast<unsigned>(popcount64(mask)));
    g_cores.store(static_cast<int>(n));
    g_active.store(true);
    NXLOG_I(kTag, "[sched] cpu_limit = %lld (potato mode): %zu of %zu physical cores (%s, efficiency class %d-%d), %d hardware threads, "
                  "processors %llx of %llx%s",
            static_cast<long long>(want), n, cores.size(), slow ? "slowest first" : "fastest first", kept_eff_min, kept_eff_max,
            popcount64(mask), static_cast<unsigned long long>(mask), static_cast<unsigned long long>(proc),
            other_groups ? " (processor group 0 only)" : "");
}

bool cpu_limit_active() { return g_active.load(std::memory_order_relaxed); }

unsigned host_threads() {
    if (g_active.load(std::memory_order_relaxed)) return std::max(1u, g_threads.load(std::memory_order_relaxed));
    return std::max(1u, std::thread::hardware_concurrency());
}

int host_cores() {
    if (g_active.load(std::memory_order_relaxed)) return g_cores.load(std::memory_order_relaxed);
    int groups = 0;
    const std::vector<Core> c = cores_of_group0(&groups);
    return static_cast<int>(c.size()) + groups;
}

}  // namespace nxrt
