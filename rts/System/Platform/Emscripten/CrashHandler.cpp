/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/* WASM port: crash handler for Emscripten.
 *
 * WebAssembly has no signals, no ucontext, no libunwind and no addr2line. Traps and
 * uncaught exceptions surface in the host (Node/browser) with a JS stack trace that
 * Emscripten maps to function names, so this only logs what the engine asks for.
 */

#include "System/Platform/CrashHandler.h"
#include "System/Log/ILog.h"

#include <emscripten.h>

#define LOG_SECTION_CRASHHANDLER "CrashHandler"
LOG_REGISTER_SECTION_GLOBAL(LOG_SECTION_CRASHHANDLER)

#ifdef LOG_SECTION_CURRENT
	#undef LOG_SECTION_CURRENT
#endif
#define LOG_SECTION_CURRENT LOG_SECTION_CRASHHANDLER

namespace CrashHandler {
	void Install() {}
	void Remove() {}

	void Stacktrace(Threading::NativeThreadHandle, const std::string& threadName, const int logLevel)
	{
		char buf[8192];
		emscripten_get_callstack(EM_LOG_C_STACK | EM_LOG_NO_PATHS, buf, sizeof(buf));
		LOG_I(logLevel, "Stacktrace (%s):\n%s", threadName.c_str(), buf);
	}

	void PrepareStacktrace(const int) {}
	void CleanupStacktrace(const int) { LOG_CLEANUP(); }

	void SuspendedStacktrace(Threading::ThreadControls*, const char* threadName)
	{
		// other threads cannot be suspended or inspected from WASM
		LOG_L(L_WARNING, "[%s] stacktrace of thread \"%s\" is not available on WebAssembly", __func__, threadName);
	}

	void OutputStacktrace()
	{
		Stacktrace(Threading::GetCurrentThread(), "current", LOG_LEVEL_ERROR);
	}
}
