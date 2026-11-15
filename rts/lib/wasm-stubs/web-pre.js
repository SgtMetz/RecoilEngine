// Emscripten --pre-js for the browser build (engine-headless-web).
// Browsers have no process environment; the page passes the engine's environment
// variables (SPRING_DATADIR, SPRING_WRITEDIR, ...) as Module.recoilEnv, copied into
// ENV before main(). Command-line arguments come from Module.arguments as usual.
// Pthread workers load this file too; preRun only runs on the main thread.
Module['preRun'] = [].concat(Module['preRun'] || []);
Module['preRun'].push(function () {
	var env = Module['recoilEnv'] || {};
	for (var key in env) {
		ENV[key] = String(env[key]);
	}
});
