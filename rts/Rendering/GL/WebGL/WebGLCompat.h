/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// Desktop-GL-on-WebGL2 shim for the browser build (RECOIL_WEB_RENDER).
//
// The engine is written against a desktop OpenGL 3.0+ compatibility context reached
// through glad's function pointers. A browser offers WebGL2 (GLES 3.0). This layer
// sits between the two: it loads glad from Emscripten's GL, reports a desktop-style
// version/extension set, fills every entry point WebGL2 lacks with a counting no-op
// stub, and replaces selected pointers with emulations.
namespace WebGLCompat {
	// SDL's Emscripten video backend creates GL contexts through EGL, which Emscripten
	// runs on the page's main thread; that cannot work for a canvas transferred to the
	// engine's worker (OffscreenCanvas). These replace the SDL_GL_* context calls and
	// use the html5 WebGL API from the calling thread instead.
	void* CreateContext();               // WebGL2 context on "#canvas", made current; nullptr on failure
	void DeleteContext(void* ctx);
	int MakeCurrent(void* ctx);          // nullptr releases the current context
	void SwapWindow();                   // presenting is implicit when the frame callback returns

	// replaces gladLoadGL(); needs a current WebGL2 context
	bool Load();

	// log the unsupported GL entry points called since the last report (name: count)
	void LogStubStats(bool final);
}
