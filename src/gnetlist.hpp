// gnetlist.hpp - a reader for gate-level (structural) Verilog netlists, for
// the tools that work on a mapped design: msta (timing), and whoever else
// needs cells and nets without elaborating behaviour.
//
// It reads what synthesis and place-and-route tools write: modules, port and
// wire declarations with ranges, cell and module instances with named or
// positional connections, concatenations, bit and part selects, constants,
// escaped identifiers, continuous assigns of a net or constant, supply0/1,
// tri0/tri1. Attributes, parameters overrides (#(...)), specify blocks and
// `celldefine-style directives are skipped. Anything behavioural (always,
// initial, expressions in an assign) is an error: this is not a simulator
// front end.
//
// gnl::write() writes a parsed module back as Verilog (declarations, assigns
// and instances in their original order), for tools that edit a netlist.
//
// gnl::read() parses; gnl::flatten() expands the hierarchy below a top module
// into one flat Netlist of leaf instances and bit-level nets. A module that is
// instantiated but not defined is a leaf (a library cell).
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "parallel.hpp"

namespace gnl {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ------------------------------------------------------------------ parsed

// A net reference as written: a whole net, one bit, a part select, or a constant.
struct Ref {
    enum Kind { Net, Bit, Part, Const, Open } kind = Open;
    std::string name;
    int msb = 0, lsb = 0;       // Bit: msb == lsb
    std::vector<uint8_t> bits;  // Const: msb first, values 0, 1, 2 (x), 3 (z)
    // The index of `name` in its module's decls, remembered by flatten() (and
    // checked against the name each time, so edits cannot make it wrong).
    mutable int decl = -1;
};

struct Conn {
    std::string port;       // "" for a positional connection
    std::vector<Ref> refs;  // a concatenation, msb first; empty: unconnected
};

struct Inst {
    std::string cell, name;
    std::vector<Conn> conns;
    int line = 0;
};

enum class Dir { In, Out, InOut };

struct Decl {
    std::string name;
    bool isPort = false, ranged = false;
    Dir dir = Dir::In;
    int msb = 0, lsb = 0;
    int tie = -1;  // supply0/tri0: 0, supply1/tri1: 1
    int width() const { return ranged ? std::abs(msb - lsb) + 1 : 1; }
};

struct Module {
    std::string name;
    std::vector<std::string> portOrder;
    // The order of the body: ('a', index into assigns) or ('i', index into insts).
    std::vector<std::pair<char, int>> body;
    std::vector<Decl> decls;
    std::unordered_map<std::string, int> declIdx;
    std::vector<Inst> insts;
    std::vector<std::pair<std::vector<Ref>, std::vector<Ref>>> assigns;  // lhs, rhs
    int line = 0;

    Decl& decl(const std::string& n) {
        auto it = declIdx.find(n);
        if (it != declIdx.end()) return decls[it->second];
        declIdx[n] = (int)decls.size();
        decls.push_back({});
        decls.back().name = n;
        return decls.back();
    }
    const Decl* find(const std::string& n) const {
        auto it = declIdx.find(n);
        return it == declIdx.end() ? nullptr : &decls[it->second];
    }
};

struct Source {
    std::vector<Module> modules;
    std::unordered_map<std::string, int> byName;
    const Module* module(const std::string& n) const {
        auto it = byName.find(n);
        return it == byName.end() ? nullptr : &modules[it->second];
    }
};

namespace detail {

struct Lexer {
    const std::string& s;
    std::string file;
    size_t p = 0;
    int line = 1;
    // 'i' identifier, 'n' number (maybe sized), 's' string, punctuation as
    // itself, 0 at the end.
    char kind = 0;
    std::string tok;
    int tokLine = 1;

    size_t end;         // lexing stops here
    size_t tokPos = 0;  // where the current token starts

    Lexer(const std::string& src, std::string f) : s(src), file(std::move(f)), end(src.size()) { next(); }
    // A piece of the text: [from, to), starting on line `ln`.
    Lexer(const std::string& src, std::string f, size_t from, size_t to, int ln)
        : s(src), file(std::move(f)), p(from), line(ln), end(to) { next(); }
    [[noreturn]] void fail(const std::string& m) const {
        throw Error(file + ":" + std::to_string(tokLine) + ": " + m);
    }
    void skip() {
        for (;;) {
            while (p < end && isspace((unsigned char)s[p])) {
                if (s[p] == '\n') line++;
                p++;
            }
            if (p + 1 < end && s[p] == '/' && s[p + 1] == '/') {
                while (p < end && s[p] != '\n') p++;
                continue;
            }
            if (p + 1 < end && s[p] == '/' && s[p + 1] == '*') {
                p += 2;
                while (p + 1 < end && !(s[p] == '*' && s[p + 1] == '/')) {
                    if (s[p] == '\n') line++;
                    p++;
                }
                p += 2;
                continue;
            }
            if (p + 1 < end && s[p] == '(' && s[p + 1] == '*' && !(p + 2 < end && s[p + 2] == ')')) {
                // (* attribute *)
                size_t e = s.find("*)", p + 2);
                if (e == std::string::npos || e >= end) { p = end; break; }
                for (size_t k = p; k < e; k++) if (s[k] == '\n') line++;
                p = e + 2;
                continue;
            }
            if (p < end && s[p] == '`') {  // compiler directive: skip the line
                while (p < end && s[p] != '\n') p++;
                continue;
            }
            break;
        }
    }
    void next() {
        skip();
        tokLine = line;
        tokPos = p;
        tok.clear();
        if (p >= end) { kind = 0; return; }
        char c = s[p];
        if (c == '\\') {  // escaped identifier: up to white space
            p++;
            while (p < end && !isspace((unsigned char)s[p])) tok += s[p++];
            kind = 'i';
            return;
        }
        if (isalpha((unsigned char)c) || c == '_' || c == '$') {
            while (p < end && (isalnum((unsigned char)s[p]) || s[p] == '_' || s[p] == '$')) tok += s[p++];
            kind = 'i';
            return;
        }
        if (isdigit((unsigned char)c) || c == '\'') {
            while (p < end && (isalnum((unsigned char)s[p]) || s[p] == '\'' || s[p] == '_' || s[p] == '?')) tok += s[p++];
            kind = 'n';
            return;
        }
        if (c == '"') {
            p++;
            while (p < end && s[p] != '"') {
                if (s[p] == '\\') p++;
                if (p < end) tok += s[p++];
            }
            p++;
            kind = 's';
            return;
        }
        kind = c;
        tok = c;
        p++;
    }
    bool is(char k) const { return kind == k; }
    bool isWord(const char* w) const { return kind == 'i' && tok == w; }
    void expect(char k) {
        if (kind != k) fail(std::string("expected '") + k + "', found '" + tok + "'");
        next();
    }
    std::string ident() {
        if (kind != 'i') fail("expected a name, found '" + tok + "'");
        std::string r = tok;
        next();
        return r;
    }
};

// 4'b10x1, 'h3, 12 -> bits, msb first. An unsized number is 32 bits.
inline std::vector<uint8_t> constBits(Lexer& L, const std::string& t) {
    size_t q = t.find('\'');
    std::vector<uint8_t> bits;
    if (q == std::string::npos) {
        unsigned long v = std::strtoul(t.c_str(), nullptr, 10);
        for (int i = 31; i >= 0; i--) bits.push_back((v >> i) & 1);
        return bits;
    }
    int width = q == 0 ? 32 : std::atoi(t.substr(0, q).c_str());
    size_t k = q + 1;
    if (k < t.size() && (t[k] == 's' || t[k] == 'S')) k++;
    if (k >= t.size()) L.fail("bad constant '" + t + "'");
    char base = (char)tolower((unsigned char)t[k++]);
    std::vector<uint8_t> raw;  // msb first
    auto digit = [&](char c, int nb) {
        c = (char)tolower((unsigned char)c);
        if (c == '_') return;
        if (c == 'x' || c == 'z' || c == '?') {
            for (int i = 0; i < nb; i++) raw.push_back(c == 'x' ? 2 : 3);
            return;
        }
        int v = isdigit((unsigned char)c) ? c - '0' : c - 'a' + 10;
        if (v < 0 || v >= (1 << nb)) L.fail("bad digit in constant '" + t + "'");
        for (int i = nb - 1; i >= 0; i--) raw.push_back((v >> i) & 1);
    };
    if (base == 'b') for (; k < t.size(); k++) digit(t[k], 1);
    else if (base == 'o') for (; k < t.size(); k++) digit(t[k], 3);
    else if (base == 'h') for (; k < t.size(); k++) digit(t[k], 4);
    else if (base == 'd') {
        unsigned long long v = std::strtoull(t.c_str() + k, nullptr, 10);
        for (int i = 63; i >= 0; i--) raw.push_back((v >> i) & 1);
    } else L.fail("bad constant '" + t + "'");
    // Fit to the width: truncate at the left, or extend (x and z extend themselves).
    uint8_t ext = !raw.empty() && raw[0] >= 2 ? raw[0] : 0;
    while ((int)raw.size() < width) raw.insert(raw.begin(), ext);
    bits.assign(raw.end() - width, raw.end());
    return bits;
}

inline int constInt(Lexer& L) {
    bool neg = false;
    if (L.is('-')) { neg = true; L.next(); }
    if (L.kind != 'n') L.fail("expected a number, found '" + L.tok + "'");
    int v = std::atoi(L.tok.c_str());
    L.next();
    return neg ? -v : v;
}

inline Ref parseRef(Lexer& L) {
    Ref r;
    if (L.kind == 'n') {
        r.kind = Ref::Const;
        r.bits = constBits(L, L.tok);
        L.next();
        return r;
    }
    r.name = L.ident();
    r.kind = Ref::Net;
    if (L.is('[')) {
        L.next();
        r.msb = r.lsb = constInt(L);
        r.kind = Ref::Bit;
        if (L.is(':')) {
            L.next();
            r.lsb = constInt(L);
            r.kind = Ref::Part;
        }
        L.expect(']');
    }
    return r;
}

// name | name[i] | name[m:l] | constant | { a, b, ... } | {n{...}}
inline void parseExpr(Lexer& L, std::vector<Ref>& out) {
    if (L.is('{')) {
        L.next();
        if (L.kind == 'n') {
            // Either a constant element or a replication count.
            std::string t = L.tok;
            Lexer save = L;
            L.next();
            if (L.is('{')) {
                int n = std::atoi(t.c_str());
                std::vector<Ref> inner;
                parseExpr(L, inner);
                L.expect('}');
                for (int i = 0; i < n; i++) out.insert(out.end(), inner.begin(), inner.end());
                return;
            }
            L.p = save.p; L.line = save.line; L.kind = save.kind; L.tok = save.tok; L.tokLine = save.tokLine; L.tokPos = save.tokPos;
        }
        for (;;) {
            parseExpr(L, out);
            if (L.is(',')) { L.next(); continue; }
            break;
        }
        L.expect('}');
        return;
    }
    out.push_back(parseRef(L));
}

inline void skipParens(Lexer& L) {
    int depth = 0;
    do {
        if (L.kind == 0) L.fail("unterminated '('");
        if (L.is('(')) depth++;
        if (L.is(')')) depth--;
        L.next();
    } while (depth > 0);
}

// One declaration: kw is input / output / inout / wire / tri / supply0 ...
inline void declare(Lexer& L, Module& m, const std::string& kw, bool inHeader) {
    // kw: input/output/inout/wire/tri/supply0/...
    bool port = kw == "input" || kw == "output" || kw == "inout";
    Dir dir = kw == "output" ? Dir::Out : kw == "inout" ? Dir::InOut : Dir::In;
    int tie = kw == "supply0" || kw == "tri0" ? 0 : kw == "supply1" || kw == "tri1" ? 1 : -1;
    while (L.isWord("wire") || L.isWord("reg") || L.isWord("logic") || L.isWord("signed") || L.isWord("tri") ||
           L.isWord("wand") || L.isWord("wor"))
        L.next();
    bool ranged = false;
    int msb = 0, lsb = 0;
    if (L.is('[')) {
        L.next();
        msb = constInt(L);
        L.expect(':');
        lsb = constInt(L);
        L.expect(']');
        ranged = true;
    }
    for (;;) {
        std::string n = L.ident();
        Decl& d = m.decl(n);
        if (port) {
            d.isPort = true;
            d.dir = dir;
            if (inHeader) m.portOrder.push_back(n);
        }
        if (ranged) { d.ranged = true; d.msb = msb; d.lsb = lsb; }
        if (tie >= 0) d.tie = tie;
        if (L.is('=')) {  // wire a = b;
            L.next();
            std::vector<Ref> lhs, rhs;
            Ref r;
            r.kind = Ref::Net;
            r.name = n;
            lhs.push_back(r);
            parseExpr(L, rhs);
            m.body.push_back({'a', (int)m.assigns.size()});
            m.assigns.push_back({lhs, rhs});
        }
        if (!L.is(',')) break;
        L.next();
        if (inHeader && (L.isWord("input") || L.isWord("output") || L.isWord("inout"))) break;
    }
}

// The items of a module body, up to endmodule (or, for a piece of a body
// parsed on its own, to the end of the piece).
inline void parseItems(Lexer& L, Module& m, bool piece) {
    static const char* declKw[] = {"input", "output", "inout", "wire", "tri", "supply0", "supply1", "tri0", "tri1",
                                   "wand", "wor", "reg", "logic"};
    while (!L.isWord("endmodule")) {
        if (L.kind == 0) {
            if (piece) return;
            L.fail("missing endmodule for module " + m.name);
        }
        if (L.is(';')) { L.next(); continue; }
        if (L.kind != 'i') L.fail("unexpected '" + L.tok + "'");
        std::string kw = L.tok;
        bool isDecl = false;
        for (auto* k : declKw) if (kw == k) isDecl = true;
        if (isDecl) {
            L.next();
            declare(L, m, kw, false);
            L.expect(';');
            continue;
        }
        if (kw == "assign") {
            L.next();
            for (;;) {
                std::vector<Ref> lhs, rhs;
                parseExpr(L, lhs);
                L.expect('=');
                parseExpr(L, rhs);
                m.body.push_back({'a', (int)m.assigns.size()});
                m.assigns.push_back({lhs, rhs});
                if (!L.is(',')) break;
                L.next();
            }
            if (!L.is(';')) L.fail("only a net, a constant or a concatenation can be assigned in a gate netlist");
            L.next();
            continue;
        }
        if (kw == "specify") {
            while (!L.isWord("endspecify") && L.kind != 0) L.next();
            L.next();
            continue;
        }
        if (kw == "parameter" || kw == "localparam" || kw == "defparam" || kw == "specparam") {
            while (!L.is(';') && L.kind != 0) L.next();
            continue;
        }
        if (kw == "always" || kw == "initial" || kw == "always_ff" || kw == "always_comb" || kw == "always_latch" ||
            kw == "function" || kw == "task" || kw == "generate" || kw == "primitive")
            L.fail("'" + kw + "' in a gate netlist: only structural Verilog is read");
        // An instance: CELL [#(...)] name [range] ( connections ) [, name (...)] ;
        Inst proto;
        proto.cell = kw;
        L.next();
        if (L.is('#')) {
            L.next();
            if (L.is('(')) skipParens(L);
            else L.next();
        }
        for (;;) {
            Inst in = proto;
            in.line = L.tokLine;
            in.name = L.ident();
            if (L.is('[')) L.fail("arrays of instances are not supported in a gate netlist");
            L.expect('(');
            while (!L.is(')')) {
                Conn c;
                if (L.is('.')) {
                    L.next();
                    c.port = L.ident();
                    L.expect('(');
                    if (!L.is(')')) parseExpr(L, c.refs);
                    L.expect(')');
                } else if (L.is(',')) {
                    // an empty positional connection
                } else {
                    parseExpr(L, c.refs);
                }
                in.conns.push_back(std::move(c));
                if (L.is(',')) { L.next(); if (L.is(')')) in.conns.push_back({}); continue; }
                if (!L.is(')')) L.fail("expected ',' or ')' in the connections of " + in.name);
            }
            L.next();
            m.body.push_back({'i', (int)m.insts.size()});
            m.insts.push_back(std::move(in));
            if (!L.is(',')) break;
            L.next();
        }
        L.expect(';');
    }
}

// Splits a large module body at statement ends and parses the pieces on
// several threads. Returns false, having done nothing, when the body is small
// or holds something that cannot be split (a specify block, a function);
// otherwise the items are in m and L is at endmodule.
inline bool parseItemsParallel(Lexer& L, Module& m, int threads) {
    const std::string& s = L.s;
    size_t start = L.tokPos;
    if (threads < 2 || L.end - start < (1u << 20)) return false;
    // One pass for the statement ends and the endmodule, skipping comments,
    // strings and escaped names.
    struct Cut { size_t pos; int line; };
    std::vector<Cut> cuts;
    size_t p = start, endPos = std::string::npos;
    int line = L.tokLine, endLine = 0;
    size_t want = (L.end - start) / (size_t)(threads * 4) + 1, last = start;
    while (p < L.end) {
        char c = s[p];
        if (c == '\n') { line++; p++; continue; }
        if (c == '/' && p + 1 < L.end && s[p + 1] == '/') {
            while (p < L.end && s[p] != '\n') p++;
            continue;
        }
        if (c == '/' && p + 1 < L.end && s[p + 1] == '*') {
            p += 2;
            while (p + 1 < L.end && !(s[p] == '*' && s[p + 1] == '/')) { if (s[p] == '\n') line++; p++; }
            p += 2;
            continue;
        }
        if (c == '"') {
            p++;
            while (p < L.end && s[p] != '"') { if (s[p] == '\\') p++; p++; }
            p++;
            continue;
        }
        if (c == '\\') {
            while (p < L.end && !isspace((unsigned char)s[p])) p++;
            continue;
        }
        if (c == ';') {
            p++;
            if (p - last >= want) { cuts.push_back({p, line}); last = p; }
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t b = p;
            while (p < L.end && (isalnum((unsigned char)s[p]) || s[p] == '_' || s[p] == '$')) p++;
            size_t n = p - b;
            if (n == 9 && s.compare(b, n, "endmodule") == 0) { endPos = b; endLine = line; break; }
            if ((n == 7 && s.compare(b, n, "specify") == 0) || (n == 8 && s.compare(b, n, "function") == 0) ||
                (n == 4 && s.compare(b, n, "task") == 0) || (n == 8 && s.compare(b, n, "generate") == 0))
                return false;
            continue;
        }
        p++;
    }
    if (endPos == std::string::npos || cuts.size() < 2) return false;
    // Pieces: [start, cut0), [cut0, cut1), ..., [cutN, endPos).
    struct Piece { size_t from, to; int line; Module m; std::string err; };
    std::vector<Piece> pieces;
    size_t from = start;
    int fromLine = L.tokLine;
    for (auto& c : cuts) {
        pieces.push_back({from, c.pos, fromLine, {}, {}});
        from = c.pos;
        fromLine = c.line;
    }
    pieces.push_back({from, endPos, fromLine, {}, {}});
    par::parallelFor(pieces.size(), (unsigned)threads, [&](size_t i) {
        Piece& pc = pieces[i];
        try {
            Lexer sub(s, L.file, pc.from, pc.to, pc.line);
            pc.m.name = m.name;
            parseItems(sub, pc.m, true);
        } catch (Error& e) {
            pc.err = e.what();
        }
    });
    for (auto& pc : pieces)
        if (!pc.err.empty()) throw Error(pc.err);
    size_t ni = m.insts.size(), na = m.assigns.size();
    for (auto& pc : pieces) { ni += pc.m.insts.size(); na += pc.m.assigns.size(); }
    m.insts.reserve(ni);
    m.assigns.reserve(na);
    m.body.reserve(ni + na);
    for (auto& pc : pieces) {
        for (auto& d : pc.m.decls) {
            Decl& t = m.decl(d.name);
            if (d.isPort) { t.isPort = true; t.dir = d.dir; }
            if (d.ranged) { t.ranged = true; t.msb = d.msb; t.lsb = d.lsb; }
            if (d.tie >= 0) t.tie = d.tie;
        }
        int ia = (int)m.assigns.size(), ii = (int)m.insts.size();
        for (auto& [k, i] : pc.m.body) m.body.push_back({k, i + (k == 'a' ? ia : ii)});
        for (auto& a : pc.m.assigns) m.assigns.push_back(std::move(a));
        for (auto& i : pc.m.insts) m.insts.push_back(std::move(i));
    }
    L.p = endPos;
    L.line = endLine;
    L.next();
    return true;
}

inline void parseModule(Lexer& L, Source& src, int threads) {
    Module m;
    m.line = L.tokLine;
    m.name = L.ident();
    if (L.is('#')) {  // parameter port list
        L.next();
        skipParens(L);
    }
    if (L.is('(')) {
        L.next();
        while (!L.is(')')) {
            if (L.isWord("input") || L.isWord("output") || L.isWord("inout")) {
                std::string kw = L.tok;
                L.next();
                declare(L, m, kw, true);
            } else {
                m.portOrder.push_back(L.ident());
                if (L.is(',')) L.next();
            }
        }
        L.next();
    }
    L.expect(';');
    if (!parseItemsParallel(L, m, threads)) parseItems(L, m, false);
    L.next();
    if (src.byName.count(m.name)) L.fail("module " + m.name + " is defined twice");
    src.byName[m.name] = (int)src.modules.size();
    src.modules.push_back(std::move(m));
}

}  // namespace detail

// Parses one file's text into src (several files can be read into one Source).
// With threads > 1 the body of a large module is parsed in pieces on that many
// threads; the result is the same.
inline void parse(const std::string& text, const std::string& file, Source& src, int threads = 1) {
    detail::Lexer L(text, file);
    while (L.kind != 0) {
        if (L.isWord("module") || L.isWord("macromodule")) {
            L.next();
            detail::parseModule(L, src, threads);
        } else if (L.isWord("primitive")) {
            while (!L.isWord("endprimitive") && L.kind != 0) L.next();
            L.next();
        } else if (L.is(';')) {
            L.next();
        } else {
            L.fail("expected 'module', found '" + L.tok + "'");
        }
    }
}

inline void read(const std::string& path, Source& src, int threads = 1) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("cannot open " + path);
    std::string text;
    in.seekg(0, std::ios::end);
    std::streamoff n = in.tellg();
    in.seekg(0);
    text.resize(n > 0 ? (size_t)n : 0);
    in.read(text.data(), (std::streamsize)text.size());
    parse(text, path, src, threads);
}

// ------------------------------------------------------------------ writer

// A name as Verilog: escaped (\name followed by a space) unless it is a
// simple identifier.
inline std::string vname(const std::string& n) {
    bool simple = !n.empty() && (isalpha((unsigned char)n[0]) || n[0] == '_');
    for (char c : n)
        if (!(isalnum((unsigned char)c) || c == '_' || c == '$')) simple = false;
    return simple ? n : "\\" + n + " ";
}

inline std::string vref(const Ref& r) {
    switch (r.kind) {
        case Ref::Net: return vname(r.name);
        case Ref::Bit: return vname(r.name) + "[" + std::to_string(r.msb) + "]";
        case Ref::Part: return vname(r.name) + "[" + std::to_string(r.msb) + ":" + std::to_string(r.lsb) + "]";
        case Ref::Const: {
            std::string s = std::to_string(r.bits.size()) + "'b";
            for (auto b : r.bits) s += "01xz"[b & 3];
            return s;
        }
        case Ref::Open: break;
    }
    return "";
}

inline std::string vexpr(const std::vector<Ref>& refs) {
    if (refs.empty()) return "";
    if (refs.size() == 1) return vref(refs[0]);
    std::string s = "{";
    for (size_t i = 0; i < refs.size(); i++) s += (i ? ", " : "") + vref(refs[i]);
    return s + "}";
}

// Writes one module: ports, declarations in their original order, then
// assigns and instances in their original order (Module::body); anything a
// tool appended to insts or assigns without a body entry follows.
inline void write(const Module& m, std::ostream& os) {
    os << "module " << vname(m.name) << " (";
    for (size_t i = 0; i < m.portOrder.size(); i++) os << (i ? ", " : "") << vname(m.portOrder[i]);
    os << ");\n";
    auto range = [](const Decl& d) {
        return d.ranged ? "[" + std::to_string(d.msb) + ":" + std::to_string(d.lsb) + "] " : std::string();
    };
    for (auto& d : m.decls)
        if (d.isPort)
            os << "  " << (d.dir == Dir::In ? "input " : d.dir == Dir::Out ? "output " : "inout ") << range(d)
               << vname(d.name) << ";\n";
    for (auto& d : m.decls)
        if (!d.isPort)
            os << "  " << (d.tie == 0 ? "supply0 " : d.tie == 1 ? "supply1 " : "wire ") << range(d) << vname(d.name)
               << ";\n";
    std::vector<bool> doneA(m.assigns.size()), doneI(m.insts.size());
    auto assign = [&](size_t i) {
        os << "  assign " << vexpr(m.assigns[i].first) << " = " << vexpr(m.assigns[i].second) << ";\n";
        doneA[i] = true;
    };
    auto inst = [&](size_t i) {
        const Inst& in = m.insts[i];
        os << "  " << vname(in.cell) << " " << vname(in.name) << " (";
        for (size_t k = 0; k < in.conns.size(); k++) {
            auto& c = in.conns[k];
            os << (k ? ", " : "");
            if (c.port.empty()) os << vexpr(c.refs);
            else os << "." << vname(c.port) << "(" << vexpr(c.refs) << ")";
        }
        os << ");\n";
        doneI[i] = true;
    };
    for (auto& [k, i] : m.body) {
        if (k == 'a' && i < (int)m.assigns.size() && !doneA[i]) assign(i);
        if (k == 'i' && i < (int)m.insts.size() && !doneI[i]) inst(i);
    }
    for (size_t i = 0; i < m.assigns.size(); i++) if (!doneA[i]) assign(i);
    for (size_t i = 0; i < m.insts.size(); i++) if (!doneI[i]) inst(i);
    os << "endmodule\n";
}

inline void write(const Source& src, std::ostream& os) {
    for (size_t i = 0; i < src.modules.size(); i++) {
        if (i) os << "\n";
        write(src.modules[i], os);
    }
}

// -------------------------------------------------------------------- flat

struct FlatConn {
    std::string pin;  // cell pin; a bus pin's bits are "D[3]"
    int net = -1;     // -1: unconnected
};

struct FlatInst {
    std::string name;  // hierarchical, '/' between levels
    std::string cell;
    std::vector<FlatConn> conns;
};

struct FlatPort {
    std::string name;  // "a" or "a[3]"
    Dir dir = Dir::In;
    int net = -1;
    std::string bus;   // "a" for a bit of a bus, else ""
    int bit = -1;
};

struct Netlist {
    std::string top;
    std::vector<std::string> nets;  // name of each net (one of its aliases)
    std::vector<FlatPort> ports;
    std::vector<FlatInst> insts;
    int const0 = -1, const1 = -1;   // nets tied to 0 and 1, -1 if unused
    std::vector<std::string> warnings;
};

// The bit order of a leaf cell's bus pin: called with (cell, pin, width) and
// returns the bit pin names msb first, or empty when the cell is unknown (then
// a multi-bit connection gets pin[width-1] .. pin[0]).
using BusPins = std::function<std::vector<std::string>(const std::string&, const std::string&, int)>;
// The pin names of a leaf cell in declaration order, for positional connections.
using PinOrder = std::function<std::vector<std::string>(const std::string&)>;

namespace detail {

struct Flattener {
    const Source& src;
    Netlist& out;
    BusPins busPins;
    PinOrder pinOrder;
    std::vector<int> parent;  // union-find over nets
    std::vector<std::string> names;
    int c0 = -1, c1 = -1, cx = -1;

    int find(int a) {
        while (parent[a] != a) a = parent[a] = parent[parent[a]];
        return a;
    }
    // Merges b into a, keeping the constants and the shorter (higher level) name.
    void unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        bool bConst = b == find0() || b == find1();
        if (bConst) std::swap(a, b);
        parent[b] = a;
    }
    int find0() { return c0 < 0 ? -2 : find(c0); }
    int find1() { return c1 < 0 ? -2 : find(c1); }
    int newNet(const std::string& n) {
        parent.push_back((int)parent.size());
        names.push_back(n);
        return (int)parent.size() - 1;
    }
    int constNet(int v) {
        if (v == 0) { if (c0 < 0) c0 = newNet("1'b0"); return c0; }
        if (v == 1) { if (c1 < 0) c1 = newNet("1'b1"); return c1; }
        return -1;  // x and z: unconnected
    }

    struct Scope {
        const Module* m;
        std::string prefix;
        std::vector<int> base;                          // the first net of each decl; its bits follow, msb first
        std::unordered_map<std::string, int> implicit;  // undeclared scalar nets
    };
    struct Bits {
        int first, n;
    };

    Bits implicitNet(Scope& sc, const std::string& n) {
        auto it = sc.implicit.find(n);
        if (it != sc.implicit.end()) return {it->second, 1};
        int id = newNet(sc.prefix + n);
        sc.implicit[n] = id;
        return {id, 1};
    }
    Bits bitsOf(Scope& sc, const std::string& n) {
        auto it = sc.m->declIdx.find(n);
        if (it == sc.m->declIdx.end()) return implicitNet(sc, n);
        return {sc.base[it->second], sc.m->decls[it->second].width()};
    }
    Bits bitsOf(Scope& sc, const Ref& r, const Decl*& d) {
        const auto& decls = sc.m->decls;
        if (r.decl < 0 || r.decl >= (int)decls.size() || decls[r.decl].name != r.name) {
            auto it = sc.m->declIdx.find(r.name);
            r.decl = it == sc.m->declIdx.end() ? -1 : it->second;
        }
        if (r.decl < 0) {
            d = nullptr;
            return implicitNet(sc, r.name);
        }
        d = &decls[r.decl];
        return {sc.base[r.decl], d->width()};
    }
    void resolve(Scope& sc, const std::vector<Ref>& refs, std::vector<int>& bits) {
        for (auto& r : refs) {
            if (r.kind == Ref::Const) {
                for (auto b : r.bits) bits.push_back(constNet(b));
                continue;
            }
            const Decl* d = nullptr;
            Bits v = bitsOf(sc, r, d);
            if (r.kind == Ref::Net) {
                for (int k = 0; k < v.n; k++) bits.push_back(v.first + k);
                continue;
            }
            int msb = d && d->ranged ? d->msb : v.n - 1;
            int lsb = d && d->ranged ? d->lsb : 0;
            auto at = [&](int i) {
                int k = msb >= lsb ? msb - i : i - msb;
                if (k < 0 || k >= v.n)
                    throw Error("module " + sc.m->name + ": index " + std::to_string(i) + " is outside " + r.name);
                return v.first + k;
            };
            int step = r.msb >= r.lsb ? -1 : 1;
            for (int i = r.msb;; i += step) {
                bits.push_back(at(i));
                if (i == r.lsb) break;
            }
        }
    }

    void expand(const Module& m, const std::string& prefix, const std::vector<std::vector<int>>* portBits, int depth) {
        if (depth > 200) throw Error("module " + m.name + " instantiates itself");
        Scope sc{&m, prefix, {}, {}};
        sc.base.reserve(m.decls.size());
        for (auto& d : m.decls) {
            sc.base.push_back((int)parent.size());
            int w = d.width();
            for (int k = 0; k < w; k++) {
                std::string n = prefix + d.name;
                if (d.ranged) n += "[" + std::to_string(d.msb >= d.lsb ? d.msb - k : d.msb + k) + "]";
                int id = newNet(n);
                if (d.tie >= 0) unite(constNet(d.tie), id);
            }
        }
        // Tie this module's port nets to the nets the parent connected.
        if (portBits) {
            for (size_t i = 0; i < m.portOrder.size(); i++) {
                Bits mine = bitsOf(sc, m.portOrder[i]);
                auto& theirs = (*portBits)[i];
                if (theirs.empty()) continue;
                if ((int)theirs.size() != mine.n)
                    out.warnings.push_back("instance " + prefix.substr(0, prefix.size() - 1) + ": port " +
                                           m.portOrder[i] + " is " + std::to_string(mine.n) +
                                           " bits, connected to " + std::to_string(theirs.size()));
                // Align at the lsb.
                size_t n = std::min((size_t)mine.n, theirs.size());
                for (size_t k = 0; k < n; k++) {
                    int t = theirs[theirs.size() - 1 - k];
                    // The parent's name wins: unite(parent, child).
                    if (t >= 0) unite(t, mine.first + mine.n - 1 - (int)k);
                }
            }
        } else {
            out.top = m.name;
            for (auto& pn : m.portOrder) {
                const Decl* d = m.find(pn);
                Bits v = bitsOf(sc, pn);
                for (int k = 0; k < v.n; k++) {
                    FlatPort fp;
                    fp.dir = d ? d->dir : Dir::In;
                    fp.net = v.first + k;
                    if (d && d->ranged) {
                        fp.bus = pn;
                        fp.bit = d->msb >= d->lsb ? d->msb - k : d->msb + k;
                        fp.name = pn + "[" + std::to_string(fp.bit) + "]";
                    } else {
                        fp.name = pn;
                    }
                    out.ports.push_back(std::move(fp));
                }
            }
        }
        for (auto& [lhs, rhs] : m.assigns) {
            std::vector<int> a, b;
            resolve(sc, lhs, a);
            resolve(sc, rhs, b);
            size_t n = std::min(a.size(), b.size());
            for (size_t k = 0; k < n; k++) {
                int x = a[a.size() - 1 - k], y = b[b.size() - 1 - k];
                if (x >= 0 && y >= 0) unite(y, x);
            }
        }
        for (auto& in : m.insts) {
            const Module* sub = src.module(in.cell);
            bool positional = !in.conns.empty() && in.conns[0].port.empty();
            if (sub) {
                std::vector<std::vector<int>> pb(sub->portOrder.size());
                for (size_t i = 0; i < in.conns.size(); i++) {
                    auto& c = in.conns[i];
                    size_t idx = i;
                    if (!positional) {
                        idx = sub->portOrder.size();
                        for (size_t k = 0; k < sub->portOrder.size(); k++)
                            if (sub->portOrder[k] == c.port) idx = k;
                        if (idx == sub->portOrder.size())
                            throw Error("instance " + prefix + in.name + ": module " + in.cell + " has no port " + c.port);
                    } else if (idx >= pb.size()) {
                        throw Error("instance " + prefix + in.name + ": too many connections for " + in.cell);
                    }
                    resolve(sc, c.refs, pb[idx]);
                }
                expand(*sub, prefix + in.name + "/", &pb, depth + 1);
                continue;
            }
            FlatInst fi;
            fi.name = prefix + in.name;
            fi.cell = in.cell;
            std::vector<std::string> order;
            if (positional) {
                if (pinOrder) order = pinOrder(in.cell);
                if (order.size() < in.conns.size())
                    throw Error("instance " + fi.name + ": positional connections need the pin order of cell " +
                                in.cell + ", which is not in a library");
            }
            for (size_t i = 0; i < in.conns.size(); i++) {
                auto& c = in.conns[i];
                std::string pin = positional ? order[i] : c.port;
                std::vector<int> bits;
                resolve(sc, c.refs, bits);
                if (bits.empty()) continue;
                std::vector<std::string> pins;
                if (busPins && bits.size() > 1) pins = busPins(in.cell, pin, (int)bits.size());
                if (pins.empty()) {
                    if (bits.size() == 1) pins.push_back(pin);
                    else for (int k = (int)bits.size() - 1; k >= 0; k--) pins.push_back(pin + "[" + std::to_string(k) + "]");
                }
                size_t n = std::min(pins.size(), bits.size());
                for (size_t k = 0; k < n; k++)  // align at the lsb
                    fi.conns.push_back({pins[pins.size() - 1 - k], bits[bits.size() - 1 - k]});
            }
            out.insts.push_back(std::move(fi));
        }
    }

    void finish() {
        // Number the surviving nets. A net keeps the name of its root; make
        // the root the member with the fewest hierarchy levels.
        std::vector<int> best(parent.size(), -1);
        auto depthOf = [&](int i) { return std::count(names[i].begin(), names[i].end(), '/'); };
        for (int i = 0; i < (int)parent.size(); i++) {
            int r = find(i);
            if (best[r] < 0 || depthOf(i) < depthOf(best[r])) best[r] = i;
        }
        std::vector<int> id(parent.size(), -1);
        auto map = [&](int n) {
            if (n < 0) return -1;
            int r = find(n);
            if (id[r] < 0) {
                id[r] = (int)out.nets.size();
                bool isConst = (c0 >= 0 && r == find(c0)) || (c1 >= 0 && r == find(c1));
                out.nets.push_back(isConst ? names[r == find0() ? c0 : c1] : names[best[r]]);
            }
            return id[r];
        };
        if (c0 >= 0 && c1 >= 0 && find(c0) == find(c1)) out.warnings.push_back("a net is tied to both 0 and 1");
        for (auto& p : out.ports) p.net = map(p.net);
        for (auto& i : out.insts)
            for (auto& c : i.conns) c.net = map(c.net);
        if (c0 >= 0) out.const0 = id[find(c0)];
        if (c1 >= 0) out.const1 = id[find(c1)];
    }
};

}  // namespace detail

// The modules no other module instantiates.
inline std::vector<std::string> tops(const Source& src) {
    std::unordered_map<std::string, bool> used;
    for (auto& m : src.modules)
        for (auto& i : m.insts) used[i.cell] = true;
    std::vector<std::string> r;
    for (auto& m : src.modules)
        if (!used.count(m.name)) r.push_back(m.name);
    return r;
}

inline Netlist flatten(const Source& src, const std::string& top, BusPins busPins = nullptr,
                       PinOrder pinOrder = nullptr) {
    const Module* m = src.module(top);
    if (!m) throw Error("no module named " + top);
    Netlist out;
    detail::Flattener f{src, out, std::move(busPins), std::move(pinOrder), {}, {}};
    f.expand(*m, "", nullptr, 0);
    f.finish();
    return out;
}

}  // namespace gnl
