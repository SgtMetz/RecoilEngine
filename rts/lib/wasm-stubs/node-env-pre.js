// Emscripten --pre-js for the Node builds (engine-headless, unit tests).
// Emscripten does not import the host environment by default; copy process.env
// into ENV before main() so SPRING_DATADIR, SPRING_WRITEDIR, HOME, ... work.
if (typeof process === 'object' && process.env) {
	Module['preRun'] = [].concat(Module['preRun'] || []);
	Module['preRun'].push(function () {
		for (var key in process.env) {
			ENV[key] = process.env[key];
		}
	});
}
