/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

// Read-only shader storage buffers for the WebGL2 shim (see WebGLCompat.h). WebGL2 has no
// SSBOs. BAR's GL4 shaders read per-unit data (engine transforms at binding 0, model
// uniforms at binding 1) and Lua-filled buffers from them. Emulation:
//  - shaders: a `[readonly] buffer` block becomes an unsigned-integer texture
//    (rcl_ssboN, 16 bytes per texel, 1024 texels per row) plus loader functions that rebuild
//    each member from its std140/std430 offsets; `member[i]` becomes `rcl_ssboN_member(i)`.
//    Blocks a shader writes to, or whose layout can't be computed, are left alone (the
//    shader then fails to compile, as before).
//  - GL: the GL_SHADER_STORAGE_BUFFER target is redirected to GL_COPY_WRITE_BUFFER around
//    each call, glBindBufferBase/Range record the indexed bindings, and before a draw with
//    a program that reads binding N the bound range is copied (GPU side, through a pixel
//    unpack buffer) into binding N's texture when the buffer changed since.

#include "WebGLInternal.h"
#include "WebGLShaderTranslate.h"

#include <glad/glad.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "System/Log/ILog.h"

// the real WebGL2 entry points (glad's pointers are the shim's by now)
extern "C" {
	void emscripten_glGetIntegerv(GLenum pname, GLint* data);
	void emscripten_glActiveTexture(GLenum texture);
	void emscripten_glBindTexture(GLenum target, GLuint texture);
	void emscripten_glGenTextures(GLsizei n, GLuint* textures);
	void emscripten_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels);
	void emscripten_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels);
	void emscripten_glTexParameteri(GLenum target, GLenum pname, GLint param);
	void emscripten_glBindBuffer(GLenum target, GLuint buffer);
	void emscripten_glPixelStorei(GLenum pname, GLint param);
	void emscripten_glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params);
	void emscripten_glUniform1ui(GLint location, GLuint v0);
}

namespace {
	// ================================================================ shader side
	bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

	size_t FindIdent(const std::string& s, std::string_view id, size_t from = 0) {
		for (size_t pos = s.find(id, from); pos != std::string::npos; pos = s.find(id, pos + 1)) {
			const bool startOk = (pos == 0) || !IsIdentChar(s[pos - 1]);
			const bool endOk = (pos + id.size() >= s.size()) || !IsIdentChar(s[pos + id.size()]);
			if (startOk && endOk)
				return pos;
		}
		return std::string::npos;
	}

	size_t SkipSpace(const std::string& s, size_t pos) {
		while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
			pos++;
		return pos;
	}

	std::string ReadIdent(const std::string& s, size_t pos) {
		size_t end = pos;
		while (end < s.size() && IsIdentChar(s[end]))
			end++;
		return s.substr(pos, end - pos);
	}

	// position of the bracket closing the one at `open` (s[open] is '[', '(' or '{')
	size_t MatchBracket(const std::string& s, size_t open) {
		const char o = s[open];
		const char c = (o == '[')? ']': (o == '(')? ')': '}';
		int depth = 0;
		for (size_t i = open; i < s.size(); i++) {
			if (s[i] == o) depth++;
			else if (s[i] == c && --depth == 0) return i;
		}
		return std::string::npos;
	}

	std::string StripComments(const std::string& s) {
		std::string out;
		for (size_t i = 0; i < s.size(); ) {
			if (s.compare(i, 2, "//") == 0) { while (i < s.size() && s[i] != '\n') i++; out += ' '; }
			else if (s.compare(i, 2, "/*") == 0) { const size_t e = s.find("*/", i + 2); i = (e == std::string::npos)? s.size(): e + 2; out += ' '; }
			else out += s[i++];
		}
		return out;
	}

	struct Member {
		std::string type;
		std::string name;
		int arrayLen = -1; // -1: not an array, 0: unsized (last member of a block)
	};
	using StructMap = std::unordered_map<std::string, std::vector<Member>>;

	// "[qualifiers] T name", "T name[N]", "T name[]", "T[N] name", several declarators
	bool ParseMembers(const std::string& bodyIn, std::vector<Member>& out) {
		const std::string body = StripComments(bodyIn);
		size_t start = 0;
		while (start < body.size()) {
			size_t end = body.find(';', start);
			if (end == std::string::npos)
				end = body.size();
			std::string decl = body.substr(start, end - start);
			start = end + 1;

			// tokens: identifiers, numbers and brackets
			std::vector<std::string> tok;
			for (size_t i = 0; i < decl.size(); ) {
				if (std::isspace(static_cast<unsigned char>(decl[i]))) { i++; continue; }
				if (IsIdentChar(decl[i])) { size_t j = i; while (j < decl.size() && IsIdentChar(decl[j])) j++; tok.push_back(decl.substr(i, j - i)); i = j; continue; }
				if (decl[i] == '[' || decl[i] == ']' || decl[i] == ',') { tok.emplace_back(1, decl[i]); i++; continue; }
				if (decl[i] == '#') return false; // preprocessor lines inside: not handled
				return false;
			}
			static const std::unordered_set<std::string> quals = {"highp", "mediump", "lowp", "readonly", "restrict", "coherent", "volatile", "flat", "smooth", "precise"};
			tok.erase(std::remove_if(tok.begin(), tok.end(), [](const std::string& t) { return quals.count(t) != 0; }), tok.end());
			if (tok.empty())
				continue;

			size_t k = 0;
			const std::string type = tok[k++];
			const auto readArray = [&](int& len) -> bool {
				if (k >= tok.size() || tok[k] != "[")
					return true;
				k++;
				if (k < tok.size() && tok[k] == "]") { len = 0; k++; return true; }
				if (k + 1 >= tok.size() || tok[k + 1] != "]" || !std::isdigit(static_cast<unsigned char>(tok[k][0])))
					return false;
				len = std::atoi(tok[k].c_str());
				k += 2;
				return len > 0;
			};
			int typeArray = -1;
			if (!readArray(typeArray))
				return false;
			while (k < tok.size()) {
				Member m;
				m.type = type;
				m.name = tok[k++];
				m.arrayLen = typeArray;
				int nameArray = -1;
				if (!readArray(nameArray))
					return false;
				if (nameArray >= 0) {
					if (typeArray >= 0) return false; // arrays of arrays
					m.arrayLen = nameArray;
				}
				if (m.name.empty() || !IsIdentChar(m.name[0]))
					return false;
				out.push_back(m);
				if (k < tok.size()) {
					if (tok[k] != ",") return false;
					k++;
				}
			}
		}
		return true;
	}

	// struct definitions of the (preprocessed) source
	StructMap ParseStructs(const std::string& s) {
		StructMap structs;
		for (size_t pos = FindIdent(s, "struct"); pos != std::string::npos; pos = FindIdent(s, "struct", pos + 6)) {
			const size_t namePos = SkipSpace(s, pos + 6);
			const std::string name = ReadIdent(s, namePos);
			const size_t brace = SkipSpace(s, namePos + name.size());
			if (name.empty() || brace >= s.size() || s[brace] != '{')
				continue;
			const size_t close = MatchBracket(s, brace);
			if (close == std::string::npos)
				break;
			std::vector<Member> members;
			if (ParseMembers(s.substr(brace + 1, close - brace - 1), members))
				structs[name] = std::move(members);
		}
		return structs;
	}

	uint32_t RoundUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

	// scalar kind ('f', 'i', 'u', 'b'), vector size and matrix columns (0: not a matrix)
	bool BasicType(const std::string& t, char& kind, int& comps, int& cols) {
		cols = 0;
		if (t == "float") { kind = 'f'; comps = 1; return true; }
		if (t == "int") { kind = 'i'; comps = 1; return true; }
		if (t == "uint") { kind = 'u'; comps = 1; return true; }
		if (t == "bool") { kind = 'b'; comps = 1; return true; }
		if (t.size() == 4 && t.compare(0, 3, "vec") == 0) { kind = 'f'; comps = t[3] - '0'; return comps >= 2 && comps <= 4; }
		if (t.size() == 5 && t.compare(1, 3, "vec") == 0 && (t[0] == 'i' || t[0] == 'u' || t[0] == 'b')) { kind = t[0]; comps = t[4] - '0'; return comps >= 2 && comps <= 4; }
		if (t.size() == 4 && t.compare(0, 3, "mat") == 0) { kind = 'f'; cols = comps = t[3] - '0'; return comps >= 2 && comps <= 4; }
		if (t.size() == 6 && t.compare(0, 3, "mat") == 0 && t[4] == 'x') { kind = 'f'; cols = t[3] - '0'; comps = t[5] - '0'; return cols >= 2 && cols <= 4 && comps >= 2 && comps <= 4; }
		return false;
	}

	struct Layout { uint32_t align = 0; uint32_t size = 0; };

	class StorageBlockWriter {
	public:
		StorageBlockWriter(const StructMap& s, bool std430, const std::string& prefix): structs(s), std430(std430), p(prefix) {}

		bool TypeLayout(const std::string& t, Layout& l) const {
			char kind; int comps, cols;
			if (BasicType(t, kind, comps, cols)) {
				const Layout vec = {uint32_t((comps == 1)? 4: (comps == 2)? 8: 16), uint32_t(4 * comps)};
				if (cols == 0) { l = vec; return true; }
				const uint32_t stride = ColumnStride(comps);
				l = {std430? vec.align: 16u, stride * cols};
				return true;
			}
			const auto it = structs.find(t);
			if (it == structs.end())
				return false;
			uint32_t offset = 0, align = 4;
			for (const Member& m: it->second) {
				Layout ml;
				if (!MemberLayout(m, ml))
					return false;
				offset = RoundUp(offset, ml.align) + ml.size;
				align = std::max(align, ml.align);
			}
			if (!std430) align = RoundUp(align, 16);
			l = {align, RoundUp(offset, align)};
			return true;
		}
		uint32_t ColumnStride(int comps) const { return std430? ((comps == 2)? 8u: 16u): 16u; }
		bool ArrayStride(const std::string& t, uint32_t& stride, uint32_t& align) const {
			Layout el;
			if (!TypeLayout(t, el))
				return false;
			align = std430? el.align: RoundUp(el.align, 16);
			stride = RoundUp(el.size, align);
			return true;
		}
		bool MemberLayout(const Member& m, Layout& l) const {
			if (m.arrayLen < 0)
				return TypeLayout(m.type, l);
			uint32_t stride, align;
			if (m.arrayLen == 0 || !ArrayStride(m.type, stride, align))
				return false;
			l = {align, stride * uint32_t(m.arrayLen)};
			return true;
		}

		// expression loading a value of type t at byte offset expression o
		bool Load(const std::string& t, const std::string& o, std::string& expr) {
			char kind; int comps, cols;
			if (BasicType(t, kind, comps, cols)) {
				if (cols > 0) {
					const std::string col = std::string("vec") + char('0' + comps);
					expr = t + "(";
					for (int c = 0; c < cols; c++) {
						std::string ce;
						Load(col, o + " + " + std::to_string(c * ColumnStride(comps)) + "u", ce);
						expr += (c? ", ": "") + ce;
					}
					expr += ")";
					return true;
				}
				const auto conv = [&](const std::string& u, int n) {
					const std::string sz = (n == 1)? "": std::to_string(n);
					switch (kind) {
						case 'f': return "uintBitsToFloat(" + u + ")";
						case 'i': return ((n == 1)? std::string("int"): "ivec" + sz) + "(" + u + ")";
						case 'b': return (n == 1)? "(" + u + " != 0u)": "notEqual(" + u + ", uvec" + sz + "(0u))";
						default: return u;
					}
				};
				if (comps >= 3) {
					// 16-byte aligned in both layouts: one texel
					expr = conv(p + "_t(" + o + ")" + ((comps == 3)? ".xyz": ""), comps);
				} else if (comps == 2) {
					expr = conv("uvec2(" + p + "_w(" + o + "), " + p + "_w(" + o + " + 4u))", 2);
				} else {
					expr = conv(p + "_w(" + o + ")", 1);
				}
				return true;
			}
			const auto it = structs.find(t);
			if (it == structs.end())
				return false;
			// one loader function per struct type
			const std::string fn = p + "_ld_" + t;
			if (!emitted.count(fn)) {
				std::string body = t + "(";
				uint32_t offset = 0;
				bool first = true;
				for (const Member& m: it->second) {
					Layout ml;
					if (!MemberLayout(m, ml))
						return false;
					offset = RoundUp(offset, ml.align);
					std::string me;
					if (!LoadMember(m, "o + " + std::to_string(offset) + "u", me))
						return false;
					body += (first? "": ", ") + me;
					first = false;
					offset += ml.size;
				}
				body += ")";
				emitted.insert(fn);
				code += t + " " + fn + "(uint o) { return " + body + "; } ";
			}
			expr = fn + "(" + o + ")";
			return true;
		}
		bool LoadMember(const Member& m, const std::string& o, std::string& expr) {
			if (m.arrayLen < 0)
				return Load(m.type, o, expr);
			uint32_t stride, align;
			if (m.arrayLen == 0 || !ArrayStride(m.type, stride, align))
				return false;
			expr = m.type + "[" + std::to_string(m.arrayLen) + "](";
			for (int i = 0; i < m.arrayLen; i++) {
				std::string e;
				if (!Load(m.type, o + " + " + std::to_string(i * stride) + "u", e))
					return false;
				expr += (i? ", ": "") + e;
			}
			expr += ")";
			return true;
		}

		const StructMap& structs;
		const bool std430;
		const std::string p;
		std::unordered_set<std::string> emitted;
		std::string code;
	};

	// true when the text after an access (at pos) assigns to it
	bool IsWrite(const std::string& s, size_t pos) {
		// skip .member / .xyz chains and further [] indexing
		for (;;) {
			pos = SkipSpace(s, pos);
			if (pos < s.size() && s[pos] == '.') { pos = SkipSpace(s, pos + 1); pos += ReadIdent(s, pos).size(); continue; }
			if (pos < s.size() && s[pos] == '[') { const size_t c = MatchBracket(s, pos); if (c == std::string::npos) return true; pos = c + 1; continue; }
			break;
		}
		if (pos + 1 < s.size() && ((s[pos] == '+' && s[pos + 1] == '+') || (s[pos] == '-' && s[pos + 1] == '-')))
			return true;
		if (pos < s.size() && s[pos] == '=' && (pos + 1 >= s.size() || s[pos + 1] != '='))
			return true;
		if (pos + 1 < s.size() && s[pos + 1] == '=' && std::string_view("+-*/%&|^").find(s[pos]) != std::string_view::npos)
			return true;
		if (pos + 2 < s.size() && s[pos + 2] == '=' && (s.compare(pos, 2, "<<") == 0 || s.compare(pos, 2, ">>") == 0))
			return true;
		return false;
	}

	// rewrites one block; false leaves the source unchanged
	bool RewriteBlock(std::string& s, size_t declBegin, size_t declEnd, const std::string& layout, const std::string& quals,
		const std::string& body, const std::string& instName, std::vector<int>& bindings, const char*& why)
	{
		if (quals.find("writeonly") != std::string::npos)
			return (why = "writeonly", false);
		// binding = N (the preprocessor has resolved macros on the cross-compilation path);
		// without one the block uses binding 0, as in GL (Lua can't change a block's binding)
		int binding = 0;
		if (const size_t b = FindIdent(layout, "binding"); b != std::string::npos) {
			size_t q = SkipSpace(layout, b + 7);
			if (q >= layout.size() || layout[q] != '=')
				return (why = "unparsable binding", false);
			q = SkipSpace(layout, q + 1);
			if (q >= layout.size() || !std::isdigit(static_cast<unsigned char>(layout[q])))
				return (why = "unparsable binding", false);
			binding = std::atoi(layout.c_str() + q);
		}
		if (binding < 0 || binding >= WebGLCompat::NUM_STORAGE_BINDINGS)
			return (why = (binding < 0)? "negative binding": "binding 16 or higher", false);
		const bool std430 = (FindIdent(layout, "std430") != std::string::npos);

		std::vector<Member> members;
		if (!ParseMembers(body, members) || members.empty())
			return (why = "unparsable members", false);
		for (size_t i = 0; i + 1 < members.size(); i++) {
			if (members[i].arrayLen == 0)
				return (why = "unsized member before the last", false);
		}

		const StructMap structs = ParseStructs(s.substr(0, declBegin));
		const std::string p = "rcl_ssbo" + std::to_string(binding);
		StorageBlockWriter w(structs, std430, p);

		// member accessors
		std::string accessors;
		struct Access { std::string name; bool array; uint32_t offset; uint32_t stride; };
		std::vector<Access> access;
		uint32_t offset = 0;
		for (const Member& m: members) {
			Layout ml;
			uint32_t stride = 0, align = 0;
			if (m.arrayLen >= 0) {
				if (!w.ArrayStride(m.type, stride, align))
					return (why = "member type layout", false);
				ml = {align, stride * uint32_t(std::max(m.arrayLen, 0))};
			} else if (!w.TypeLayout(m.type, ml)) {
				return (why = "member type layout", false);
			}
			offset = RoundUp(offset, ml.align);
			std::string e;
			if (!w.Load(m.type, (m.arrayLen >= 0)? (std::to_string(offset) + "u + i * " + std::to_string(stride) + "u"): (std::to_string(offset) + "u"), e))
				return (why = "member type load", false);
			accessors += m.type + " " + p + "_" + m.name + ((m.arrayLen >= 0)? "(uint i)": "()") + " { return " + e + "; } ";
			access.push_back({m.name, m.arrayLen >= 0, offset, stride});
			offset += ml.size;
		}

		// rewrite the uses (after the declaration) into a copy, then commit
		std::string rest = s.substr(declEnd);
		for (const Access& a: access) {
			const std::string fn = p + "_" + a.name;
			for (size_t pos = FindIdent(rest, instName.empty()? a.name: instName); pos != std::string::npos; ) {
				size_t start = pos;
				size_t after = pos + (instName.empty()? a.name.size(): instName.size());
				// a struct field of the same name (x.name) is something else
				size_t prev = start;
				while (prev > 0 && std::isspace(static_cast<unsigned char>(rest[prev - 1]))) prev--;
				if (prev > 0 && rest[prev - 1] == '.') { pos = FindIdent(rest, instName.empty()? a.name: instName, after); continue; }
				if (!instName.empty()) {
					size_t d = SkipSpace(rest, after);
					if (d >= rest.size() || rest[d] != '.') { pos = FindIdent(rest, instName, after); continue; }
					d = SkipSpace(rest, d + 1);
					if (ReadIdent(rest, d) != a.name) { pos = FindIdent(rest, instName, after); continue; }
					after = d + a.name.size();
				}
				size_t open = SkipSpace(rest, after);
				std::string repl;
				size_t replEnd;
				if (a.array && open < rest.size() && rest[open] == '[') {
					const size_t close = MatchBracket(rest, open);
					if (close == std::string::npos || IsWrite(rest, close + 1))
						return (why = "written to", false);
					repl = fn + "(uint(" + rest.substr(open + 1, close - open - 1) + "))";
					replEnd = close + 1;
				} else if (a.array && open < rest.size() && rest[open] == '.') {
					const size_t m = SkipSpace(rest, open + 1);
					const size_t paren = SkipSpace(rest, m + 6);
					if (ReadIdent(rest, m) != "length" || paren >= rest.size() || rest[paren] != '(')
						return (why = "unsupported member access", false);
					const size_t close = MatchBracket(rest, paren);
					if (close == std::string::npos || a.stride == 0)
						return (why = "unsupported length()", false);
					repl = "int((" + p + "_bytes - " + std::to_string(a.offset) + "u) / " + std::to_string(a.stride) + "u)";
					replEnd = close + 1;
				} else if (!a.array) {
					if (IsWrite(rest, after))
						return (why = "written to", false);
					repl = fn + "()";
					replEnd = after;
				} else {
					return (why = "whole array used as a value", false);
				}
				rest.replace(start, replEnd - start, repl);
				pos = FindIdent(rest, instName.empty()? a.name: instName, start + repl.size());
			}
		}

		// declarations on one line, followed by the block's newlines (line numbers stay)
		std::string decl;
		decl += "uniform highp usampler2D " + p + "; uniform uint " + p + "_bytes; ";
		decl += "uvec4 " + p + "_t(uint o) { uint i = o >> 4u; return texelFetch(" + p + ", ivec2(int(i & 1023u), int(i >> 10u)), 0); } ";
		decl += "uint " + p + "_w(uint o) { return " + p + "_t(o)[int((o >> 2u) & 3u)]; } ";
		decl += w.code + accessors;
		for (size_t i = declBegin; i < declEnd; i++) {
			if (s[i] == '\n')
				decl += '\n';
		}
		s = s.substr(0, declBegin) + decl + rest;
		if (std::find(bindings.begin(), bindings.end(), binding) == bindings.end())
			bindings.push_back(binding);
		return true;
	}
}

int WebGLCompat::RewriteStorageBlocks(std::string& s, std::vector<int>& bindings, bool report)
{
	int failed = 0;
	for (size_t pos = FindIdent(s, "buffer"); pos != std::string::npos; ) {
		// [layout(...)] [qualifiers] buffer Name { ... } [inst];
		const size_t namePos = SkipSpace(s, pos + 6);
		const std::string name = ReadIdent(s, namePos);
		const size_t brace = SkipSpace(s, namePos + name.size());
		if (name.empty() || brace >= s.size() || s[brace] != '{') {
			pos = FindIdent(s, "buffer", pos + 6);
			continue;
		}
		const size_t close = MatchBracket(s, brace);
		if (close == std::string::npos)
			break;
		size_t endPos = SkipSpace(s, close + 1);
		const std::string inst = ReadIdent(s, endPos);
		endPos = SkipSpace(s, endPos + inst.size());
		if (endPos >= s.size() || s[endPos] != ';') {
			pos = FindIdent(s, "buffer", close);
			continue;
		}
		// the qualifiers and layout before `buffer`, back to the previous statement
		size_t declBegin = s.find_last_of(";}{\n", pos);
		declBegin = (declBegin == std::string::npos)? 0: declBegin + 1;
		// a layout(...) on the previous line belongs to this declaration too
		std::string head = s.substr(declBegin, pos - declBegin);
		while (head.find("layout") == std::string::npos && declBegin > 0 && s[declBegin - 1] == '\n') {
			const size_t prevBegin = s.find_last_of(";}{\n", declBegin - 2);
			const size_t pb = (prevBegin == std::string::npos)? 0: prevBegin + 1;
			const std::string prevLine = s.substr(pb, declBegin - pb);
			if (prevLine.find("layout") == std::string::npos)
				break;
			declBegin = pb;
			head = s.substr(declBegin, pos - declBegin);
		}
		std::string layout;
		if (const size_t l = head.find("layout"); l != std::string::npos) {
			const size_t lp = head.find('(', l);
			const size_t lc = (lp == std::string::npos)? std::string::npos: head.find(')', lp);
			if (lc != std::string::npos)
				layout = head.substr(lp + 1, lc - lp - 1);
		}
		const char* why = "?";
		if (RewriteBlock(s, declBegin, endPos + 1, layout, head, s.substr(brace + 1, close - brace - 1), inst, bindings, why)) {
			pos = FindIdent(s, "buffer", declBegin);
		} else {
			if (report)
				LOG_L(L_WARNING, "[WebGLCompat] storage block %s left as is (%s): shaders using it won't compile", name.c_str(), why);
			failed++;
			pos = FindIdent(s, "buffer", close);
		}
	}
	return failed;
}

namespace {
	// ================================================================ GL side
	#define PREV(name) decltype(glad_##name) prev_##name = nullptr
	#define WRAP(name) do { prev_##name = glad_##name; glad_##name = compat_##name; } while (0)

	PREV(glBindBuffer); PREV(glBindBufferBase); PREV(glBindBufferRange);
	PREV(glBufferData); PREV(glBufferSubData); PREV(glGetBufferSubData); PREV(glGetBufferParameteriv);
	PREV(glMapBuffer); PREV(glMapBufferRange); PREV(glUnmapBuffer); PREV(glFlushMappedBufferRange);
	PREV(glCopyBufferSubData); PREV(glDeleteBuffers); PREV(glGetIntegerv);

	constexpr int NUM_BINDINGS = WebGLCompat::NUM_STORAGE_BINDINGS;
	constexpr int NUM_UNITS = WebGLCompat::NUM_STORAGE_UNITS;
	constexpr uint32_t ROW_TEXELS = 1024;
	constexpr uint32_t ROW_BYTES = ROW_TEXELS * 16;

	GLuint genericBuffer = 0; // GL_SHADER_STORAGE_BUFFER (non-indexed) binding
	GLuint copyWriteBuffer = 0; // our idea of GL_COPY_WRITE_BUFFER (see AsTarget)
	GLuint copyReadBuffer = 0;

	struct Binding { GLuint buffer = 0; GLintptr offset = 0; GLsizeiptr size = -1; };
	Binding bindings[NUM_BINDINGS];

	std::unordered_map<GLuint, uint32_t> versions; // bumped by every write through the SSBO target

	struct Mirror {
		GLuint tex = 0;
		GLsizei rows = 0;
		GLuint buffer = 0;
		GLintptr offset = -1;
		GLsizeiptr size = -1;
		uint32_t version = 0;
		uint32_t bytes = 0;
	};
	Mirror mirrors[NUM_BINDINGS];
	GLint unitBase = 0;

	GLint RealInt(GLenum pname) { GLint v = 0; emscripten_glGetIntegerv(pname, &v); return v; }

	// run f with the SSBO generic binding visible as GL_COPY_WRITE_BUFFER (or COPY_READ)
	template<typename F> auto AsTarget(GLenum as, F&& f) {
		const GLenum binding = (as == GL_COPY_WRITE_BUFFER)? GL_COPY_WRITE_BUFFER_BINDING: GL_COPY_READ_BUFFER_BINDING;
		const GLint prev = RealInt(binding);
		prev_glBindBuffer(as, genericBuffer);
		struct Restore { GLenum as; GLint prev; ~Restore() { prev_glBindBuffer(as, GLuint(prev)); } } restore{as, prev};
		return f(as);
	}

	void Bump(GLuint buffer) { if (buffer != 0) versions[buffer]++; }

	void APIENTRY compat_glBindBuffer(GLenum target, GLuint buffer) {
		if (target == GL_SHADER_STORAGE_BUFFER) { genericBuffer = buffer; return; }
		prev_glBindBuffer(target, buffer);
	}
	void APIENTRY compat_glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
		if (target != GL_SHADER_STORAGE_BUFFER) { prev_glBindBufferBase(target, index, buffer); return; }
		genericBuffer = buffer;
		if (index < GLuint(NUM_BINDINGS))
			bindings[index] = {buffer, 0, -1};
	}
	void APIENTRY compat_glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
		if (target != GL_SHADER_STORAGE_BUFFER) { prev_glBindBufferRange(target, index, buffer, offset, size); return; }
		genericBuffer = buffer;
		if (index < GLuint(NUM_BINDINGS))
			bindings[index] = {buffer, offset, size};
	}
	void APIENTRY compat_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
		if (target != GL_SHADER_STORAGE_BUFFER) { prev_glBufferData(target, size, data, usage); return; }
		Bump(genericBuffer);
		AsTarget(GL_COPY_WRITE_BUFFER, [&](GLenum t) { prev_glBufferData(t, size, data, usage); return 0; });
	}
	void APIENTRY compat_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
		if (target != GL_SHADER_STORAGE_BUFFER) { prev_glBufferSubData(target, offset, size, data); return; }
		Bump(genericBuffer);
		AsTarget(GL_COPY_WRITE_BUFFER, [&](GLenum t) { prev_glBufferSubData(t, offset, size, data); return 0; });
	}
	void APIENTRY compat_glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data) {
		if (target != GL_SHADER_STORAGE_BUFFER) { prev_glGetBufferSubData(target, offset, size, data); return; }
		AsTarget(GL_COPY_WRITE_BUFFER, [&](GLenum t) { prev_glGetBufferSubData(t, offset, size, data); return 0; });
	}
	void APIENTRY compat_glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params) {
		if (target != GL_SHADER_STORAGE_BUFFER) { prev_glGetBufferParameteriv(target, pname, params); return; }
		AsTarget(GL_COPY_WRITE_BUFFER, [&](GLenum t) { prev_glGetBufferParameteriv(t, pname, params); return 0; });
	}
	void* APIENTRY compat_glMapBuffer(GLenum target, GLenum access) {
		if (target != GL_SHADER_STORAGE_BUFFER) return prev_glMapBuffer(target, access);
		Bump(genericBuffer);
		return AsTarget(GL_COPY_WRITE_BUFFER, [&](GLenum t) { return prev_glMapBuffer(t, access); });
	}
	void* APIENTRY compat_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
		if (target != GL_SHADER_STORAGE_BUFFER) return prev_glMapBufferRange(target, offset, length, access);
		return AsTarget(GL_COPY_WRITE_BUFFER, [&](GLenum t) { return prev_glMapBufferRange(t, offset, length, access); });
	}
	GLboolean APIENTRY compat_glUnmapBuffer(GLenum target) {
		if (target != GL_SHADER_STORAGE_BUFFER) return prev_glUnmapBuffer(target);
		Bump(genericBuffer);
		return AsTarget(GL_COPY_WRITE_BUFFER, [&](GLenum t) { return prev_glUnmapBuffer(t); });
	}
	void APIENTRY compat_glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
		if (target != GL_SHADER_STORAGE_BUFFER) { prev_glFlushMappedBufferRange(target, offset, length); return; }
		Bump(genericBuffer);
		AsTarget(GL_COPY_WRITE_BUFFER, [&](GLenum t) { prev_glFlushMappedBufferRange(t, offset, length); return 0; });
	}
	void APIENTRY compat_glCopyBufferSubData(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size) {
		if (readTarget != GL_SHADER_STORAGE_BUFFER && writeTarget != GL_SHADER_STORAGE_BUFFER) {
			prev_glCopyBufferSubData(readTarget, writeTarget, readOffset, writeOffset, size);
			return;
		}
		if (writeTarget == GL_SHADER_STORAGE_BUFFER) {
			Bump(genericBuffer);
			// copying within the generic buffer: both ends are the same buffer
			const GLenum rt = (readTarget == GL_SHADER_STORAGE_BUFFER)? GL_COPY_WRITE_BUFFER: readTarget;
			AsTarget(GL_COPY_WRITE_BUFFER, [&](GLenum t) { prev_glCopyBufferSubData(rt, t, readOffset, writeOffset, size); return 0; });
		} else {
			// SSBO -> other target; COPY_READ carries the SSBO unless the destination is COPY_READ
			const GLenum as = (writeTarget == GL_COPY_READ_BUFFER)? GL_COPY_WRITE_BUFFER: GL_COPY_READ_BUFFER;
			AsTarget(as, [&](GLenum t) { prev_glCopyBufferSubData(t, writeTarget, readOffset, writeOffset, size); return 0; });
		}
	}
	void APIENTRY compat_glDeleteBuffers(GLsizei n, const GLuint* buffers) {
		for (GLsizei i = 0; i < n; i++) {
			versions.erase(buffers[i]);
			if (genericBuffer == buffers[i]) genericBuffer = 0;
			for (Binding& b: bindings) { if (b.buffer == buffers[i]) b = {}; }
			for (Mirror& m: mirrors) { if (m.buffer == buffers[i]) m.buffer = 0; }
		}
		prev_glDeleteBuffers(n, buffers);
	}
	void APIENTRY compat_glGetIntegerv(GLenum pname, GLint* data) {
		switch (pname) {
			case GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS: { *data = NUM_BINDINGS; } return;
			case GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS:
			case GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS: { *data = 4; } return;
			case GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS: { *data = NUM_UNITS; } return;
			// texture rows * row bytes
			case GL_MAX_SHADER_STORAGE_BLOCK_SIZE: { *data = RealInt(GL_MAX_TEXTURE_SIZE) * GLint(ROW_BYTES); } return;
			case GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT: { *data = 16; } return;
			case GL_SHADER_STORAGE_BUFFER_BINDING: { *data = GLint(genericBuffer); } return;
			default: break;
		}
		prev_glGetIntegerv(pname, data);
	}

	// copy the bound range of binding b into its texture if it changed since
	uint32_t UpdateMirror(int b) {
		const Binding& bd = bindings[b];
		Mirror& m = mirrors[b];
		if (bd.buffer == 0)
			return 0;

		GLsizeiptr size = bd.size;
		const uint32_t version = versions[bd.buffer];
		if (m.buffer == bd.buffer && m.offset == bd.offset && m.size == bd.size && m.version == version)
			return m.bytes;

		const GLint prevUnpack = RealInt(GL_PIXEL_UNPACK_BUFFER_BINDING);
		emscripten_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, bd.buffer);
		if (size < 0) {
			GLint total = 0;
			emscripten_glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &total);
			size = std::max<GLsizeiptr>(0, total - bd.offset);
		}
		const uint32_t texels = uint32_t(size / 16);
		const GLsizei rows = GLsizei((texels + ROW_TEXELS - 1) / ROW_TEXELS);

		const GLint prevActive = RealInt(GL_ACTIVE_TEXTURE);
		// any of the reserved units will do: BindStorageForDraw binds every mirror afterwards
		emscripten_glActiveTexture(GL_TEXTURE0 + unitBase);
		if (m.tex == 0) {
			emscripten_glGenTextures(1, &m.tex);
			emscripten_glBindTexture(GL_TEXTURE_2D, m.tex);
			emscripten_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			emscripten_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		} else {
			emscripten_glBindTexture(GL_TEXTURE_2D, m.tex);
		}
		if (rows > m.rows) {
			// grow in powers of two; no data, so no unpack buffer
			GLsizei newRows = std::max<GLsizei>(1, m.rows);
			while (newRows < rows) newRows *= 2;
			emscripten_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
			emscripten_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, ROW_TEXELS, newRows, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, nullptr);
			emscripten_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, bd.buffer);
			m.rows = newRows;
		}

		const GLint rowLength = RealInt(GL_UNPACK_ROW_LENGTH), skipRows = RealInt(GL_UNPACK_SKIP_ROWS), skipPixels = RealInt(GL_UNPACK_SKIP_PIXELS), align = RealInt(GL_UNPACK_ALIGNMENT);
		emscripten_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		emscripten_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
		emscripten_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
		emscripten_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		const uint32_t fullRows = texels / ROW_TEXELS;
		const uint32_t lastTexels = texels % ROW_TEXELS;
		if (fullRows > 0)
			emscripten_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ROW_TEXELS, GLsizei(fullRows), GL_RGBA_INTEGER, GL_UNSIGNED_INT, reinterpret_cast<const void*>(bd.offset));
		if (lastTexels > 0)
			emscripten_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, GLint(fullRows), GLsizei(lastTexels), 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, reinterpret_cast<const void*>(bd.offset + GLintptr(fullRows) * ROW_BYTES));
		emscripten_glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
		emscripten_glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
		emscripten_glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
		emscripten_glPixelStorei(GL_UNPACK_ALIGNMENT, align);

		emscripten_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, GLuint(prevUnpack));
		emscripten_glActiveTexture(GLenum(prevActive));

		m.buffer = bd.buffer;
		m.offset = bd.offset;
		m.size = bd.size;
		m.version = version;
		m.bytes = texels * 16;
		return m.bytes;
	}
}

void WebGLCompat::BindStorageForDraw(const std::vector<StorageUse>& uses)
{
	if (uses.empty())
		return;
	// refresh every mirror first (an upload binds its texture on a reserved unit), then bind
	// them all: the engine may also have used the units for something else
	for (const StorageUse& u: uses) {
		const uint32_t bytes = UpdateMirror(u.binding);
		if (u.bytesLoc >= 0)
			emscripten_glUniform1ui(u.bytesLoc, bytes);
	}
	const GLint prevActive = RealInt(GL_ACTIVE_TEXTURE);
	for (const StorageUse& u: uses) {
		emscripten_glActiveTexture(GL_TEXTURE0 + u.unit);
		emscripten_glBindTexture(GL_TEXTURE_2D, mirrors[u.binding].tex);
	}
	emscripten_glActiveTexture(GLenum(prevActive));
}

GLint WebGLCompat::StorageTextureUnit(int slot) { return (slot >= 0 && slot < NUM_UNITS)? unitBase + slot: -1; }

void WebGLCompat::InstallStorageEmulation()
{
	// the last texture units, out of the engine's usual range
	unitBase = std::max(0, RealInt(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS) - NUM_UNITS);
	WRAP(glBindBuffer); WRAP(glBindBufferBase); WRAP(glBindBufferRange);
	WRAP(glBufferData); WRAP(glBufferSubData); WRAP(glGetBufferSubData); WRAP(glGetBufferParameteriv);
	WRAP(glMapBuffer); WRAP(glMapBufferRange); WRAP(glUnmapBuffer); WRAP(glFlushMappedBufferRange);
	WRAP(glCopyBufferSubData); WRAP(glDeleteBuffers); WRAP(glGetIntegerv);
	LOG("[WebGLCompat] read-only shader storage buffers: %d bindings as textures on units %d-%d (up to %d per program)", NUM_BINDINGS, unitBase, unitBase + NUM_UNITS - 1, NUM_UNITS);
}
