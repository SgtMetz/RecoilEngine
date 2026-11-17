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

// WebGL state read-back cache (engine-web). The engine and its Lua widgets save and restore
// GL state around draws with glGet* (blend function/equation, color and depth masks, cull
// face, polygon offset, line width, pixel store): about 16,000 getParameter calls a second
// in a game. Each is a synchronous round trip to the browser's GPU process; Firefox spends
// most of the frame there (under 3 fps). The values below change only through the listed
// setters, so getParameter answers from a per-context copy that those setters invalidate.
// Every call goes through these prototype methods, Emscripten's own included. A lost context
// clears the copy.
(function () {
	if (typeof WebGL2RenderingContext === 'undefined') return;
	var P = WebGL2RenderingContext.prototype;
	if (P.__recoilStateCache) return;
	P.__recoilStateCache = true;
	var BLEND_FUNC = [0x80C8, 0x80C9, 0x80CA, 0x80CB, 0x0BE1, 0x0BE0];
	var BLEND_EQ = [0x8009, 0x883D];
	var invalidates = {
		blendFunc: BLEND_FUNC, blendFuncSeparate: BLEND_FUNC,
		blendEquation: BLEND_EQ, blendEquationSeparate: BLEND_EQ,
		colorMask: [0x0C23], depthMask: [0x0B72], depthFunc: [0x0B74],
		cullFace: [0x0B45], frontFace: [0x0B46], lineWidth: [0x0B21],
		polygonOffset: [0x2A00, 0x8038],
		blendColor: [0x8005], clearColor: [0x0C22], clearDepth: [0x0B73],
		stencilMask: [0x0B98, 0x8CA5], stencilFunc: [0x0B92, 0x0B93, 0x0B94, 0x8800, 0x8CA3, 0x8CA4],
		stencilOp: [0x0B94, 0x0B95, 0x0B96, 0x8801, 0x8802, 0x8803],
	};
	var cacheable = {};
	for (var k in invalidates) invalidates[k].forEach(function (p) { cacheable[p] = true; });
	// pixel store and enable/disable invalidate the parameter they name
	var PIXEL_STORE = [0x0CF2, 0x0CF3, 0x0CF4, 0x0CF5, 0x806E, 0x806D, 0x0D02, 0x0D03, 0x0D04, 0x0D05, 0x806C, 0x806B, 0x9240, 0x9241, 0x9243];
	var CAPS = [0x0BE2, 0x0B71, 0x0B44, 0x0B90, 0x0C11, 0x8037, 0x809E, 0x80A0, 0x0BD0, 0x8C89];
	PIXEL_STORE.concat(CAPS).forEach(function (p) { cacheable[p] = true; });

	var caches = new WeakMap();
	function cacheOf(gl) { var c = caches.get(gl); if (!c) { c = new Map(); caches.set(gl, c); } return c; }
	var getParameter = P.getParameter;
	P.getParameter = function (pname) {
		if (!cacheable[pname]) return getParameter.call(this, pname);
		var c = cacheOf(this);
		if (c.has(pname)) {
			var v = c.get(pname);
			return (v && v.slice) ? v.slice() : v; // arrays (color mask): a fresh copy, like WebGL
		}
		var r = getParameter.call(this, pname);
		if ((r !== null || pname === 0x8894 || pname === 0x8895) && !this.isContextLost()) c.set(pname, (r && r.slice) ? r.slice() : r);
		return r;
	};
	// The engine sets state and reads it back right after (save/restore around draws), so a
	// setter stores the value it sets (write-through) instead of only invalidating; the
	// remaining invalidate-only setters drop their entries.
	var f32 = Math.fround, bool = function (v) { return !!v; };
	var writes = {
		blendFunc: function (c, a) { c.set(0x80C9, a[0]); c.set(0x80CB, a[0]); c.set(0x80C8, a[1]); c.set(0x80CA, a[1]); c.delete(0x0BE1); c.delete(0x0BE0); },
		blendFuncSeparate: function (c, a) { c.set(0x80C9, a[0]); c.set(0x80C8, a[1]); c.set(0x80CB, a[2]); c.set(0x80CA, a[3]); c.delete(0x0BE1); c.delete(0x0BE0); },
		blendEquation: function (c, a) { c.set(0x8009, a[0]); c.set(0x883D, a[0]); },
		blendEquationSeparate: function (c, a) { c.set(0x8009, a[0]); c.set(0x883D, a[1]); },
		colorMask: function (c, a) { c.set(0x0C23, [!!a[0], !!a[1], !!a[2], !!a[3]]); },
		depthMask: function (c, a) { c.set(0x0B72, bool(a[0])); },
		depthFunc: function (c, a) { c.set(0x0B74, a[0]); },
		cullFace: function (c, a) { c.set(0x0B45, a[0]); },
		frontFace: function (c, a) { c.set(0x0B46, a[0]); },
		lineWidth: function (c, a) { c.set(0x0B21, f32(a[0])); },
		polygonOffset: function (c, a) { c.set(0x8038, f32(a[0])); c.set(0x2A00, f32(a[1])); },
	};
	Object.keys(invalidates).forEach(function (name) {
		var f = P[name], list = invalidates[name], w = writes[name];
		if (typeof f !== 'function') return;
		P[name] = function () {
			var r = f.apply(this, arguments);
			var c = caches.get(this);
			if (c) {
				// recorded without checking for a GL error (that check is the round trip this
				// avoids); an invalid call would leave the copy wrong until the next valid set
				if (w) w(c, arguments); else for (var i = 0; i < list.length; i++) c.delete(list[i]);
			}
			return r;
		};
	});
	var NUMERIC_PIXEL_STORE = {};
	[0x0CF2, 0x0CF3, 0x0CF4, 0x0CF5, 0x806E, 0x806D, 0x0D02, 0x0D03, 0x0D04, 0x0D05, 0x806C, 0x806B].forEach(function (p) { NUMERIC_PIXEL_STORE[p] = true; });
	var pixelStorei = P.pixelStorei;
	P.pixelStorei = function (p, v) {
		var r = pixelStorei.apply(this, arguments);
		var c = caches.get(this);
		if (c) { if (NUMERIC_PIXEL_STORE[p]) c.set(p, v | 0); else c.delete(p); }
		return r;
	};
	[['enable', true], ['disable', false]].forEach(function (e) {
		var f = P[e[0]], on = e[1];
		P[e[0]] = function (cap) {
			var r = f.apply(this, arguments);
			var c = caches.get(this);
			if (c && cacheable[cap]) c.set(cap, on);
			return r;
		};
	});
	// buffer bindings: ARRAY_BUFFER_BINDING follows bindBuffer; ELEMENT_ARRAY_BUFFER_BINDING
	// is part of the vertex array object, so it is dropped when the VAO changes; deleting a
	// buffer unbinds it, so that drops both
	cacheable[0x8894] = true; cacheable[0x8895] = true;
	var bindBuffer = P.bindBuffer;
	P.bindBuffer = function (target, buf) {
		var r = bindBuffer.apply(this, arguments);
		var c = caches.get(this);
		if (c) {
			if (target === 0x8892) c.set(0x8894, buf || null);
			else if (target === 0x8893) c.set(0x8895, buf || null);
		}
		return r;
	};
	['bindVertexArray', 'deleteVertexArray'].forEach(function (name) {
		var f = P[name];
		P[name] = function () { var c = caches.get(this); if (c) c.delete(0x8895); return f.apply(this, arguments); };
	});
	var deleteBuffer = P.deleteBuffer;
	P.deleteBuffer = function () { var c = caches.get(this); if (c) { c.delete(0x8894); c.delete(0x8895); } return deleteBuffer.apply(this, arguments); };

	// isEnabled reads the same caps
	var isEnabled = P.isEnabled;
	P.isEnabled = function (cap) {
		if (!cacheable[cap]) return isEnabled.call(this, cap);
		var c = cacheOf(this);
		if (c.has(cap)) return !!c.get(cap);
		var r = isEnabled.call(this, cap);
		if (!this.isContextLost()) c.set(cap, r);
		return r;
	};
	if (typeof addEventListener === 'function') {
		// context loss: drop everything (the canvas fires this in its own scope; be generous)
		addEventListener('webglcontextlost', function () { caches = new WeakMap(); }, true);
	}
})();
