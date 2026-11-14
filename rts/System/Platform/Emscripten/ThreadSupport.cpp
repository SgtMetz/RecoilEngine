/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/* WASM port: thread controls for Emscripten.
 *
 * Same thread start-up handshake as Platform/Linux/ThreadSupport.cpp, but without
 * the SIGUSR1 suspend/resume machinery: Web Workers cannot be suspended or have
 * their context captured, so Suspend/Resume always report THREADERR_MISC.
 */

#include <cassert>
#include <functional>
#include <pthread.h>

#include "System/Log/ILog.h"
#include "System/Platform/Threading.h"

namespace Threading {

void SetupCurrentThreadControls(std::shared_ptr<ThreadControls>& threadCtls)
{
	assert(!Threading::IsWatchDogThread());

	if (threadCtls.get() != nullptr)
		LOG_L(L_WARNING, "[%s] thread already has ThreadControls installed", __func__);

	threadCtls.reset(new Threading::ThreadControls());
	threadCtls->handle = GetCurrentThread();
	threadCtls->thread_id = static_cast<pid_t>(reinterpret_cast<uintptr_t>(pthread_self()));
	threadCtls->running.store(true);
}

void ThreadStart(
	std::function<void()> taskFunc,
	std::shared_ptr<ThreadControls>* threadCtls,
	ThreadControls* tempCtls
) {
	SetupCurrentThreadControls(localThreadControls);

	if (threadCtls != nullptr)
		*threadCtls = localThreadControls;

	{
		// same handshake as Linux: lets the parent in CreateNewThread() continue
		tempCtls->mutSuspend.lock();
		tempCtls->condInitialized.notify_all();
		tempCtls->mutSuspend.unlock();
	}

	taskFunc();

	localThreadControls->mutSuspend.lock();
	localThreadControls->running = false;
	localThreadControls->mutSuspend.unlock();
}

SuspendResult ThreadControls::Suspend() { return Threading::THREADERR_MISC; }
SuspendResult ThreadControls::Resume() { return Threading::THREADERR_MISC; }

} // namespace Threading
