/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

// Header-only so it can be tested natively on SPIR-V modules (see WebGLSpirv.cpp).

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

// ---- branch folding -----------------------------------------------------------------------
// glslang lowers every && / || (GLSL short-circuits them) to a selection construct joined by
// an OpPhi, and ?: and small ifs to a selection whose arms store into a function variable.
// SPIRV-Cross prints both back as nested if/else chains: BAR's radar preview sheet shader
// grows a 35-line function into 220 lines of branches. ANGLE's D3D11 backend inlines every
// function into HLSL, and on that shader the HLSL compiler gave up after about a second and
// the WebGL context was lost (and with it every later draw).
//
// This pass folds such a selection into straight-line code when each arm is short and has
// no side effects other than, at its end, one store to a function variable (no calls,
// loops, discards or nested branches left in it): the arms' instructions move into the
// header block, each phi of the merge block becomes an OpSelect, and stores become one store
// of an OpSelect (a one-armed store selects against the variable's current value). The
// header then runs straight into the merge block and absorbs it, so enclosing selections
// can fold next. Results are unchanged: evaluating both arms is invisible because nothing
// else in them writes. Scalars only: SPIR-V 1.0 OpSelect needs a condition with as many
// components as the result.
namespace SpvFold {
	enum : uint32_t {
		OpUndef = 1, OpExtInstImport = 11, OpExtInst = 12, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22,
		OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43, OpConstantNull = 46,
		OpFunction = 54, OpFunctionParameter = 55, OpFunctionEnd = 56, OpFunctionCall = 57,
		OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66,
		OpSelect = 169, OpPhi = 245, OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248,
		OpBranch = 249, OpBranchConditional = 250, OpSwitch = 251,
		StorageClassFunction = 7,
	};
	constexpr size_t MAX_ARM_INSTRUCTIONS = 48;

	using Inst = std::vector<uint32_t>;
	inline uint32_t Op(const Inst& i) { return i[0] & 0xffffu; }
	struct Block { uint32_t label; std::vector<Inst> body; };
	struct Function { std::vector<Inst> head; std::vector<Block> blocks; Inst end; };

	inline bool Speculatable(const Inst& i, uint32_t glslStd) {
		const uint32_t op = Op(i);
		if (op == OpExtInst)
			return i.size() >= 5 && i[3] == glslStd && i[4] != 35 && i[4] != 51; // not Modf/Frexp (pointer outputs)
		return op == 1 || op == 8 || op == 317            // OpUndef, OpLine, OpNoLine
			|| op == 61 || op == 65 || op == 66             // OpLoad, OpAccessChain, OpInBoundsAccessChain
			|| (op >= 77 && op <= 84) || op == 86           // vector/composite ops, OpTranspose, OpSampledImage
			|| op == 87 || op == 88 || op == 95 || op == 100 // samples, OpImageFetch, OpImage
			|| op == 103 || op == 104 || op == 106          // image size/level queries
			|| (op >= 109 && op <= 124)                     // conversions, OpBitcast
			|| (op >= 126 && op <= 152)                     // arithmetic, matrix/vector products, OpDot
			|| (op >= 154 && op <= 191)                     // OpAny/OpAll, tests, logical ops, OpSelect, comparisons
			|| (op >= 194 && op <= 205)                     // shifts and bit operations
			|| (op >= 207 && op <= 215);                    // derivatives
	}
	// instructions laid out as [header, result type, result id, ...]
	inline bool HasResultType(uint32_t op, const Inst& i, uint32_t glslStd) {
		if (op == OpLabel || op == 8 || op == 317 || i.size() < 3)
			return false;
		return op == OpUndef || (op >= OpConstantTrue && op <= OpConstantNull) || op == OpFunctionParameter
			|| op == OpFunctionCall || op == OpVariable || op == OpPhi || Speculatable(i, glslStd) || op == OpExtInst;
	}

	struct Module {
		uint32_t glslStd = 0;
		uint32_t bound = 0;
		std::unordered_map<uint32_t, uint32_t> typeOp;  // type id -> OpTypeBool/Int/Float
		std::unordered_map<uint32_t, uint32_t> idType;  // value id -> its type id
		bool IsScalar(uint32_t value) const {
			const auto t = idType.find(value);
			if (t == idType.end())
				return false;
			const auto o = typeOp.find(t->second);
			return o != typeOp.end() && (o->second == OpTypeBool || o->second == OpTypeInt || o->second == OpTypeFloat);
		}
	};

	// fold selections in one function; returns the number folded
	inline int FoldFunction(Function& f, Module& mod) {
		int folded = 0;
		for (bool changed = true; changed; ) {
			changed = false;
			// label references from branches and merge/continue declarations; phi parents;
			// function variables and the variable each access chain starts from
			std::unordered_map<uint32_t, int> refs, phiParentRefs;
			std::unordered_map<uint32_t, size_t> index;
			std::unordered_map<uint32_t, uint32_t> base; // pointer id -> function variable it points into
			for (size_t bi = 0; bi < f.blocks.size(); bi++) {
				index[f.blocks[bi].label] = bi;
				for (const Inst& i: f.blocks[bi].body) {
					switch (Op(i)) {
						case OpBranch: refs[i[1]]++; break;
						case OpBranchConditional: refs[i[2]]++; refs[i[3]]++; break;
						case OpSelectionMerge: refs[i[1]]++; break;
						case OpLoopMerge: refs[i[1]]++; refs[i[2]]++; break;
						case OpSwitch: refs[i[2]]++; for (size_t k = 4; k < i.size(); k += 2) refs[i[k]]++; break;
						case OpPhi: for (size_t k = 4; k < i.size(); k += 2) phiParentRefs[i[k]]++; break;
						case OpVariable: if (i.size() >= 4 && i[3] == StorageClassFunction) base[i[2]] = i[2]; break;
						case OpAccessChain: case OpInBoundsAccessChain:
							if (const auto b = base.find(i[3]); b != base.end()) base[i[2]] = b->second;
							break;
						default: break;
					}
				}
			}

			for (size_t ai = 0; ai < f.blocks.size() && !changed; ai++) {
				Block& a = f.blocks[ai];
				const size_t n = a.body.size();
				if (n < 2 || Op(a.body[n - 1]) != OpBranchConditional || Op(a.body[n - 2]) != OpSelectionMerge)
					continue;
				const uint32_t m = a.body[n - 2][1], cond = a.body[n - 1][1];
				const uint32_t x = a.body[n - 1][2], y = a.body[n - 1][3];
				if (x == y || (x == m && y == m) || index.count(m) == 0 || !mod.IsScalar(cond))
					continue;

				// both arms: the merge itself, or a block only A reaches, running straight into M
				// after speculatable code and at most one final store to a function variable
				struct Arm { bool ok = false; uint32_t ptr = 0, value = 0; size_t numCode = 0; };
				const auto arm = [&](uint32_t s) {
					Arm r;
					if (s == m) { r.ok = true; return r; }
					const auto it = index.find(s);
					if (it == index.end() || it->second == 0 || refs[s] != 1)
						return r;
					const std::vector<Inst>& body = f.blocks[it->second].body;
					if (body.empty() || body.size() > MAX_ARM_INSTRUCTIONS || Op(body.back()) != OpBranch || body.back()[1] != m)
						return r;
					r.numCode = body.size() - 1;
					if (r.numCode > 0 && Op(body[r.numCode - 1]) == OpStore) {
						const Inst& st = body[r.numCode - 1];
						if (st.size() != 3 || base.count(st[1]) == 0 || !mod.IsScalar(st[2]))
							return r;
						r.ptr = st[1];
						r.value = st[2];
						r.numCode--;
					}
					for (size_t k = 0; k < r.numCode; k++) {
						if (!Speculatable(body[k], mod.glslStd))
							return r;
					}
					r.ok = true;
					return r;
				};
				const Arm ax = arm(x), ay = arm(y);
				if (!ax.ok || !ay.ok)
					continue;
				// two stores must write the same pointer
				if (ax.ptr != 0 && ay.ptr != 0 && ax.ptr != ay.ptr)
					continue;
				// M: reached only through this construct, with scalar phis over exactly its two edges
				if (refs[m] != 3)
					continue;
				const uint32_t px = (x == m)? a.label: x, py = (y == m)? a.label: y;
				Block& mb = f.blocks[index[m]];
				size_t numPhis = 0;
				bool phisOk = true;
				for (; numPhis < mb.body.size() && Op(mb.body[numPhis]) == OpPhi; numPhis++) {
					const Inst& p = mb.body[numPhis];
					const auto t = mod.typeOp.find(p[1]);
					const bool scalar = (t != mod.typeOp.end() && (t->second == OpTypeBool || t->second == OpTypeInt || t->second == OpTypeFloat));
					if (p.size() != 7 || !scalar || !((p[4] == px && p[6] == py) || (p[4] == py && p[6] == px))) {
						phisOk = false;
						break;
					}
				}
				// the phis' parents stop being predecessors of M: no phi other than M's may name them
				if (!phisOk || phiParentRefs[px] != int(numPhis) || phiParentRefs[py] != int(numPhis))
					continue;
				// nothing to merge: leave it (an empty if, or one that only discards, ...)
				if (numPhis == 0 && ax.ptr == 0 && ay.ptr == 0)
					continue;

				// fold: A's own code, both arms' code, the merged store, one OpSelect per phi, M's code
				std::vector<Inst> body(a.body.begin(), a.body.end() - 2);
				for (const auto& [s, ar]: {std::pair<uint32_t, Arm>{x, ax}, std::pair<uint32_t, Arm>{y, ay}}) {
					if (s == m)
						continue;
					const std::vector<Inst>& armBody = f.blocks[index[s]].body;
					body.insert(body.end(), armBody.begin(), armBody.begin() + ar.numCode);
				}
				if (ax.ptr != 0 || ay.ptr != 0) {
					const uint32_t ptr = (ax.ptr != 0)? ax.ptr: ay.ptr;
					const uint32_t type = mod.idType[(ax.ptr != 0)? ax.value: ay.value];
					uint32_t vTrue = ax.value, vFalse = ay.value;
					if (ax.ptr == 0 || ay.ptr == 0) {
						const uint32_t old = mod.bound++;
						body.push_back({(4u << 16) | OpLoad, type, old, ptr});
						mod.idType[old] = type;
						(ax.ptr == 0? vTrue: vFalse) = old;
					}
					const uint32_t sel = mod.bound++;
					body.push_back({(6u << 16) | OpSelect, type, sel, cond, vTrue, vFalse});
					mod.idType[sel] = type;
					body.push_back({(3u << 16) | OpStore, ptr, sel});
				}
				for (size_t k = 0; k < numPhis; k++) {
					const Inst& p = mb.body[k];
					const uint32_t vTrue = (p[4] == px)? p[3]: p[5];
					const uint32_t vFalse = (p[4] == px)? p[5]: p[3];
					body.push_back({(6u << 16) | OpSelect, p[1], p[2], cond, vTrue, vFalse});
				}
				body.insert(body.end(), mb.body.begin() + numPhis, mb.body.end());
				a.body = std::move(body);

				// M's successors now see A as their predecessor
				const uint32_t aLabel = a.label;
				for (Block& b: f.blocks) {
					for (Inst& i: b.body) {
						if (Op(i) != OpPhi)
							continue;
						for (size_t k = 4; k < i.size(); k += 2) {
							if (i[k] == m)
								i[k] = aLabel;
						}
					}
				}
				std::vector<Block> kept;
				kept.reserve(f.blocks.size());
				for (Block& b: f.blocks) {
					if (b.label != m && !(b.label == x && x != m) && !(b.label == y && y != m))
						kept.push_back(std::move(b));
				}
				f.blocks = std::move(kept);
				folded++;
				changed = true;
			}
		}
		return folded;
	}

	// folds the selections of every function in the module; returns how many
	inline int Fold(std::vector<uint32_t>& words) {
		if (words.size() < 5)
			return 0;
		Module mod;
		mod.bound = words[3];
		std::vector<Inst> globals;
		std::vector<Function> functions;
		Function* cur = nullptr;
		for (size_t p = 5; p < words.size(); ) {
			const uint32_t count = words[p] >> 16;
			if (count == 0 || p + count > words.size())
				return 0; // malformed: leave the module alone
			Inst i(words.begin() + p, words.begin() + p + count);
			p += count;
			const uint32_t op = Op(i);
			if ((op == OpTypeBool || op == OpTypeInt || op == OpTypeFloat) && i.size() >= 2)
				mod.typeOp[i[1]] = op;
			if (op == OpExtInstImport && i.size() >= 3 && std::strcmp(reinterpret_cast<const char*>(&i[2]), "GLSL.std.450") == 0)
				mod.glslStd = i[1];
			if (HasResultType(op, i, mod.glslStd))
				mod.idType[i[2]] = i[1];
			if (cur == nullptr) {
				if (op == OpFunction) {
					functions.emplace_back();
					cur = &functions.back();
					cur->head.push_back(std::move(i));
					continue;
				}
				globals.push_back(std::move(i));
				continue;
			}
			if (op == OpFunctionEnd) {
				cur->end = std::move(i);
				cur = nullptr;
			} else if (op == OpLabel) {
				cur->blocks.push_back({i[1], {}});
			} else if (cur->blocks.empty()) {
				cur->head.push_back(std::move(i));
			} else {
				cur->blocks.back().body.push_back(std::move(i));
			}
		}
		if (cur != nullptr)
			return 0;

		int folded = 0;
		for (Function& f: functions)
			folded += FoldFunction(f, mod);
		if (folded == 0)
			return 0;

		std::vector<uint32_t> out(words.begin(), words.begin() + 5);
		out[3] = mod.bound;
		const auto emit = [&out](const Inst& i) { out.insert(out.end(), i.begin(), i.end()); };
		for (const Inst& i: globals)
			emit(i);
		for (const Function& f: functions) {
			for (const Inst& i: f.head)
				emit(i);
			for (const Block& b: f.blocks) {
				out.push_back((2u << 16) | OpLabel);
				out.push_back(b.label);
				for (const Inst& i: b.body)
					emit(i);
			}
			emit(f.end);
		}
		words = std::move(out);
		return folded;
	}
}
