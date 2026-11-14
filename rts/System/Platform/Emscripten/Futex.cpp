/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/* WASM port: spring_futex / linux_signal for Emscripten.
 *
 * Same algorithm as Platform/Linux/Futex.cpp, but the wait/wake primitives are
 * emscripten_futex_wait/emscripten_futex_wake (Atomics.wait/notify on shared memory)
 * instead of the futex(2) syscall. On the browser main thread, where Atomics.wait
 * is not allowed, Emscripten falls back to busy-waiting.
 */

#include "System/Platform/Linux/Futex.h"

#include <emscripten/threading.h>
#include <algorithm>
#include <climits>
#include <cmath>

static inline void futex_wait(std::uint32_t* addr, std::uint32_t expected, double maxWaitMs)
{
	emscripten_futex_wait(addr, expected, maxWaitMs);
}

static inline void futex_wake(std::uint32_t* addr, int count)
{
	emscripten_futex_wake(addr, count);
}


spring_futex::spring_futex() noexcept { mtx = 0; }
spring_futex::~spring_futex() { mtx = 0; }

void spring_futex::lock()
{
	native_type c;
	if ((c = __sync_val_compare_and_swap(&mtx, 0, 1)) == 0)
		return;

	do {
		if ((c == 2) || __sync_val_compare_and_swap(&mtx, 1, 2) != 0)
			futex_wait(&mtx, 2, INFINITY);
	} while ((c = __sync_val_compare_and_swap(&mtx, 0, 2)) != 0);
}

bool spring_futex::try_lock() noexcept
{
	return __sync_bool_compare_and_swap(&mtx, 0, 1);
}

void spring_futex::unlock()
{
	if (__sync_fetch_and_sub(&mtx, 1) != 1) {
		mtx = 0;
		futex_wake(&mtx, 4);
	}
}


linux_signal::linux_signal() noexcept
: sleepers(false)
, gen(0)
, mtx(0)
{
}

linux_signal::~linux_signal()
{
	gen = 0;
	mtx = 0;
}

void linux_signal::wait()
{
	int m; // cur gen
	const int g = gen.load(); // our gen
	sleepers++;
	while ((g - (m = mtx)) >= 0) {
		futex_wait(&mtx, m, INFINITY);
	}
	sleepers--;
}

void linux_signal::wait_for(spring_time t)
{
	int m; // cur gen
	const int g = gen.load(); // our gen
	sleepers++;

	const spring_time endTimer = spring_now() + t;
	const double waitMs = t.toNanoSecsi() / 1e6;

	while (((g - (m = mtx)) >= 0) && (spring_now() < endTimer)) {
		futex_wait(&mtx, m, waitMs);
	}
	sleepers--;
}

void linux_signal::notify_all(const int min_sleepers)
{
	if (sleepers.load() < std::max(1, min_sleepers))
		return;

	mtx = gen++;
	futex_wake(&mtx, INT_MAX);
}
