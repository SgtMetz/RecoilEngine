/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/* WASM port: CPU topology for Emscripten.
 *
 * The browser exposes only a logical core count (navigator.hardwareConcurrency,
 * surfaced as emscripten_num_logical_cores()); there is no cache or SMT topology
 * and no way to pin threads. Report every core as a performance core sharing one
 * cache group, and never pin.
 */

#include "System/Platform/CpuTopology.h"

#include <emscripten/threading.h>
#include <algorithm>

namespace cpu_topology {

static uint32_t LogicalCoreMask()
{
	int n = emscripten_num_logical_cores();
	if (n < 1) n = 1;
	if (n > 32) n = 32;
	return (n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
}

ThreadPinPolicy GetThreadPinPolicy() { return THREAD_PIN_POLICY_NONE; }

ProcessorMasks GetProcessorMasks()
{
	ProcessorMasks masks;
	masks.performanceCoreMask = LogicalCoreMask();
	return masks;
}

ProcessorCaches GetProcessorCache()
{
	ProcessorCaches caches;
	ProcessorGroupCaches group;
	group.groupMask = LogicalCoreMask();
	caches.groupCaches.push_back(group);
	return caches;
}

} // namespace cpu_topology


// Emscripten's musl declares sched_{get,set}affinity in <sched.h> but does not implement
// them. Threading.cpp and ThreadAffinityGuard.cpp use them, so provide them here: every
// logical core is "available", and setting affinity is accepted but has no effect.
#include <sched.h>
#include <cstring> // memset, used by CPU_ZERO_S

extern "C" int sched_getaffinity(pid_t, size_t setSize, cpu_set_t* set)
{
	if (set == nullptr)
		return -1;

	CPU_ZERO_S(setSize, set);

	const int n = std::max(1, emscripten_num_logical_cores());
	for (int i = 0; i < n && static_cast<size_t>(i) < setSize * 8; ++i)
		CPU_SET_S(i, setSize, set);

	return 0;
}

extern "C" int sched_setaffinity(pid_t, size_t, const cpu_set_t*)
{
	return 0;
}
