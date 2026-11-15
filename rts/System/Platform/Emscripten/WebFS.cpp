/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

/* WASM port, browser build only (engine-headless-web): file-system setup.
 *
 * Game content lives in the origin private file system (OPFS), which the page fills
 * before it starts the engine. This mounts the OPFS root at /recoil, the fixed
 * executable directory of the WASM build (see Platform::GetProcessExecutablePath), so
 * the layout under /recoil is the usual data-dir layout: base/, maps/, packages/,
 * pool/, plus the write dir.
 *
 * WASMFS does not persist modification times in OPFS: every file reports the time it
 * was first looked up. The archive scanner validates its cache by mtime (archives and
 * every pool file), so a pre-built ArchiveCache would always be discarded and the
 * whole game re-hashed. The page therefore also writes /recoil/mtimes.tsv
 * ("<unix seconds>\t<path relative to /recoil>" per line), and the original times are
 * restored here; WASMFS keeps them in its directory cache for the rest of the session.
 *
 * Runs at the very start of main() (Main.cpp), on the engine's main pthread
 * (PROXY_TO_PTHREAD): OPFS access blocks, which the browser main thread must not do.
 */

#include <emscripten/emscripten.h>
#include <emscripten/wasmfs.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" void recoil_wasm_platform_init()
{
	const double t0 = emscripten_get_now();

	backend_t opfs = wasmfs_create_opfs_backend();

	if (wasmfs_create_directory("/recoil", 0777, opfs) != 0) {
		fprintf(stderr, "[WebFS] could not mount OPFS at /recoil\n");
		return;
	}

	FILE* f = fopen("/recoil/mtimes.tsv", "r");

	if (f == nullptr) {
		printf("[WebFS] OPFS mounted at /recoil (no mtimes.tsv, keeping WASMFS times)\n");
		return;
	}

	char line[4096];
	std::string path;
	size_t numSet = 0;
	size_t numFailed = 0;

	while (fgets(line, sizeof(line), f) != nullptr) {
		char* tab = strchr(line, '\t');

		if (tab == nullptr)
			continue;

		*tab = 0;
		char* rel = tab + 1;
		rel[strcspn(rel, "\r\n")] = 0;

		const time_t mtime = static_cast<time_t>(strtoll(line, nullptr, 10));
		const struct timespec times[2] = {{mtime, 0}, {mtime, 0}};

		path.assign("/recoil/");
		path.append(rel);

		if (utimensat(AT_FDCWD, path.c_str(), times, 0) == 0) {
			numSet += 1;
		} else {
			if (numFailed < 10)
				fprintf(stderr, "[WebFS] utimensat failed for %s\n", path.c_str());

			numFailed += 1;
		}
	}

	fclose(f);
	printf("[WebFS] OPFS mounted at /recoil, restored %zu mtimes (%zu failed) in %.1f s\n", numSet, numFailed, (emscripten_get_now() - t0) * 0.001);
}
