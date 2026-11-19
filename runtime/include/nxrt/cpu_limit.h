// SPDX-License-Identifier: GPL-3.0-or-later
// "Potato mode" for performance tests (M5, Road to 0.4.0): [sched] cpu_limit = N holds the whole process to N physical cores, so a fast PC
// runs the game the way a small one would. The runtime's own thread pools are sized from what is left, not from the machine.
//   [sched] cpu_limit        0 (default, off) or the number of physical cores to keep
//   [sched] cpu_limit_smt    true (default): both hardware threads of each kept core; false: one per core (a CPU without SMT)
//   [sched] cpu_limit_cores  fast (default): the fastest cores first (P-cores); slow: the slowest first (E-cores on a hybrid CPU)
// Applied once at start (nxrt::init), before any thread pool exists; the log names the processors kept.
#pragma once

namespace nxrt {

void cpu_limit_apply();      // nxrt::init, right after the log is up
bool cpu_limit_active();     // a limit is in force
// Hardware threads and physical cores for sizing thread pools: the limit's when one is in force, else the machine's
// (std::thread::hardware_concurrency(), and the physical cores the system reports).
unsigned host_threads();
int host_cores();

}  // namespace nxrt
