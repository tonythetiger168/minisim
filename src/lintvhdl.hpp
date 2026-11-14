// VHDL-only rules, on the VHDL source text (mlint).
//
// Everything that has an equivalent in SystemVerilog is checked on the
// translated design by the ordinary rules (src/vhdl/sv.hpp turns VHDL into
// SV, `line directives keep the VHDL file and line). What the translation
// erases is checked here, on the VHDL tokens: vendor packages, 'event clock
// edges, buffer ports, shared variables, positional port maps, unconstrained
// integers, initial values, port types.
//
// The rules follow the VHDL sections of the STARC RTL design style guide and
// the Reuse Methodology Manual; the checks are written here, no other tool's
// code is used. One task per file; findings are reported in a fixed order.
#pragma once

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "lintir.hpp"
#include "parallel.hpp"

namespace lintvhdl {
using namespace lintir;

struct Tok {
    char kind;          // i identifier or keyword (lower case), n number, s string, c character, p punctuation
    std::string s;
    size_t line, col;
};

// IEEE 1076-2019 clause 15: identifiers are case-insensitive; comments are
// "--" to the end of the line and (since 2008) /* ... */.
inline std::vector<Tok> lex(std::string_view t) {
    std::vector<Tok> out;
    size_t i = 0, line = 1, ls = 0;
    auto push = [&](char k, std::string s, size_t at) { out.push_back({k, std::move(s), line, at - ls + 1}); };
    while (i < t.size()) {
        char c = t[i];
        if (c == '\n') { line++; i++; ls = i; continue; }
        if (std::isspace((unsigned char)c)) { i++; continue; }
        if (c == '-' && i + 1 < t.size() && t[i + 1] == '-') {
            while (i < t.size() && t[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < t.size() && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < t.size() && !(t[i] == '*' && t[i + 1] == '/')) {
                if (t[i] == '\n') { line++; ls = i + 1; }
                i++;
            }
            i += 2;
            continue;
        }
        size_t b = i;
        if (std::isalpha((unsigned char)c)) {
            while (i < t.size() && (std::isalnum((unsigned char)t[i]) || t[i] == '_')) i++;
            std::string s(t.substr(b, i - b));
            for (char& x : s) x = (char)std::tolower((unsigned char)x);
            push('i', std::move(s), b);
            continue;
        }
        if (c == '\\') {   // extended identifier: case matters, keep it as written
            i++;
            while (i < t.size() && t[i] != '\\' && t[i] != '\n') i++;
            i++;
            push('i', std::string(t.substr(b, i - b)), b);
            continue;
        }
        if (std::isdigit((unsigned char)c)) {
            while (i < t.size() && (std::isalnum((unsigned char)t[i]) || t[i] == '_' || t[i] == '#' || t[i] == '.')) i++;
            push('n', std::string(t.substr(b, i - b)), b);
            continue;
        }
        if (c == '"') {
            i++;
            while (i < t.size() && t[i] != '\n') {
                if (t[i] == '"') {
                    if (i + 1 < t.size() && t[i + 1] == '"') { i += 2; continue; }
                    break;
                }
                i++;
            }
            i++;
            push('s', std::string(t.substr(b, std::min(i, t.size()) - b)), b);
            continue;
        }
        if (c == '\'') {
            // A character literal 'x', unless this is an attribute or a
            // qualified expression after a name: clk'event, t'(...).
            bool afterName = !out.empty() && (out.back().kind == 'i' || out.back().s == ")") &&
                             out.back().line == line && out.back().col + out.back().s.size() == b - ls + 1;
            if (i + 2 < t.size() && t[i + 2] == '\'' && !(afterName && (std::isalpha((unsigned char)t[i + 1]) || t[i + 1] == '('))) {
                push('c', std::string(t.substr(b, 3)), b);
                i += 3;
                continue;
            }
            push('p', "'", b);
            i++;
            continue;
        }
        static const char* two[] = {"=>", ":=", "<=", ">=", "/=", "**", "<>", "?="};
        bool done = false;
        for (const char* d : two)
            if (t.substr(i, 2) == d) {
                push('p', d, b);
                i += 2;
                done = true;
                break;
            }
        if (done) continue;
        push('p', std::string(1, c), b);
        i++;
    }
    return out;
}

struct Found {
    std::string rule;
    size_t line, col;
    std::string text;
};

inline bool isWord(const Tok& t, std::string_view w) { return t.kind == 'i' && t.s == w; }

// The declaration "names : [mode] type ..." from tokens [b, e): the names and
// where the type mark is.
struct Decl {
    std::vector<const Tok*> names;
    size_t type = 0;   // index of the type mark; 0 when there is none
};
inline Decl declAt(const std::vector<Tok>& k, size_t b, size_t e) {
    Decl d;
    size_t i = b;
    for (; i < e && k[i].s != ":"; i++)
        if (k[i].kind == 'i') d.names.push_back(&k[i]);
    if (i >= e) return d;
    i++;
    static const char* modes[] = {"in", "out", "inout", "buffer", "linkage", "signal", "constant", "variable"};
    while (i < e && k[i].kind == 'i') {
        bool mode = false;
        for (const char* m : modes) mode |= k[i].s == m;
        if (!mode) break;
        i++;
    }
    if (i < e && k[i].kind == 'i') d.type = i;
    return d;
}

inline void check(std::string_view text, const Rules& rules, std::vector<Found>& out) {
    std::vector<Tok> k = lex(text);
    auto on = [&](const char* r) { return rules.enabled(r); };
    auto add = [&](const char* r, const Tok& at, std::string msg) {
        if (on(r)) out.push_back({r, at.line, at.col, std::move(msg)});
    };
    // Is a range constraint present in [b, e)?
    auto ranged = [&](size_t b, size_t e) {
        for (size_t j = b; j < e; j++)
            if (isWord(k[j], "range") || k[j].s == "(") return true;
        return false;
    };
    auto integerNoRange = [&](const Decl& d, size_t e, const char* what) {
        if (!d.type) return;
        const std::string& ty = k[d.type].s;
        if (ty != "integer" && ty != "natural" && ty != "positive") return;
        if (ranged(d.type + 1, e)) return;
        for (const Tok* n : d.names)
            add("VHD008", *n,
                std::string(what) + " '" + n->s + "' is " + ty +
                    " with no range, so synthesis builds 32 bits; constrain it (" + ty + " range 0 to N)");
    };
    auto bitPort = [&](const Decl& d) {
        if (!d.type) return;
        const std::string& ty = k[d.type].s;
        if (ty != "bit" && ty != "bit_vector") return;
        for (const Tok* n : d.names)
            add("VHD009", *n,
                "port '" + n->s + "' is of type " + ty +
                    "; std_logic models X and Z and is what the rest of a design connects to");
    };
    int depth = 0;
    for (size_t i = 0; i < k.size(); i++) {
        const Tok& t = k[i];
        if (t.s == "(") depth++;
        if (t.s == ")") depth--;
        if (t.kind != 'i') continue;
        // VHD001: use ieee.std_logic_arith.all and friends.
        if ((t.s == "std_logic_arith" || t.s == "std_logic_unsigned" || t.s == "std_logic_signed") && i >= 2 &&
            k[i - 1].s == "." && isWord(k[i - 2], "ieee"))
            add("VHD001", t,
                "ieee." + t.s +
                    " is a vendor package, not an IEEE standard, and tools differ on it; use ieee.numeric_std");
        // VHD002: clk'event as a clock edge.
        if (t.s == "event" && i >= 2 && k[i - 1].s == "'" && k[i - 2].kind == 'i')
            add("VHD002", k[i - 2],
                "'" + k[i - 2].s +
                    "'event as a clock edge also fires on X to 1 and other non-edges; use rising_edge(" +
                    k[i - 2].s + ") or falling_edge(" + k[i - 2].s + ")");
        // VHD003: buffer ports (a reserved word, so only ever a mode).
        if (t.s == "buffer")
            add("VHD003", t,
                "a buffer port; read an internal signal and drive the port from it (VHDL-2008 also lets out "
                "ports be read)");
        // VHD005: shared variables.
        if (t.s == "shared" && i + 1 < k.size() && isWord(k[i + 1], "variable"))
            add("VHD005", t, "a shared variable: synthesis does not build it and processes race on it; use a signal");
        // VHD006: port map / generic map by position.
        if ((t.s == "port" || t.s == "generic") && i + 2 < k.size() && isWord(k[i + 1], "map") && k[i + 2].s == "(") {
            int d = 0;
            bool named = false, any = false, positional = false;
            for (size_t j = i + 2; j < k.size(); j++) {
                const std::string& x = k[j].s;
                if (x == "(" && ++d == 1) continue;
                if (x == ")" && d-- == 1) { positional |= any && !named; break; }
                if (d == 1 && x == ",") { positional |= any && !named; named = any = false; continue; }
                if (d == 1 && x == "=>") named = true;
                any = true;
            }
            if (positional)
                add("VHD006", k[i + 1],
                    t.s + " map connects by position: a " + t.s +
                        " added to the entity later shifts every connection after it; use name => actual");
        }
        // VHD004, VHD008: signal declarations.
        if (t.s == "signal" && depth == 0) {
            size_t e = i + 1;
            while (e < k.size() && k[e].s != ";") e++;
            Decl d = declAt(k, i + 1, e);
            for (size_t j = i + 1; j < e; j++)
                if (k[j].s == ":=") {
                    if (!d.names.empty())
                        add("VHD004", *d.names.front(),
                            "signal '" + d.names.front()->s +
                                "' has an initial value: FPGA tools load it, ASIC synthesis ignores it, so the two "
                                "start differently; reset it instead");
                    break;
                }
            integerNoRange(d, e, "signal");
            i = e - 1;
            continue;
        }
        // VHD008, VHD009: entity ports.
        if (t.s == "port" && i + 1 < k.size() && k[i + 1].s == "(" && !(i > 0 && isWord(k[i - 1], "end"))) {
            int d = 0;
            size_t b = i + 2, j = i + 1;
            for (; j < k.size(); j++) {
                if (k[j].s == "(") { d++; continue; }
                if (k[j].s == ")" && --d == 0) break;
                if (d == 1 && k[j].s == ";") {
                    Decl dc = declAt(k, b, j);
                    integerNoRange(dc, j, "port");
                    bitPort(dc);
                    b = j + 1;
                }
            }
            if (j <= k.size()) {
                size_t e = std::min(j, k.size());
                Decl dc = declAt(k, b, e);
                integerNoRange(dc, e, "port");
                bitPort(dc);
            }
        }
        // VHD007: a process with no label.
        if (t.s == "process" && !(i > 0 && (isWord(k[i - 1], "end") || k[i - 1].s == ":" ||
                                            (isWord(k[i - 1], "postponed") && i > 1 && k[i - 2].s == ":"))))
            add("VHD007", t, "a process with no label; a label names it in waveforms, coverage and messages");
    }
}

inline void run(const std::vector<std::string>& files, Report& rep, unsigned threads) {
    static const char* all[] = {"VHD001", "VHD002", "VHD003", "VHD004", "VHD005", "VHD006", "VHD007", "VHD008", "VHD009"};
    bool any = false;
    for (const char* r : all) any |= rep.rules.enabled(r);
    if (!any) return;
    std::vector<std::vector<Found>> found(files.size());
    par::parallelFor(files.size(), threads, [&](size_t i) {
        std::ifstream in(files[i], std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = ss.str();
        check(text, rep.rules, found[i]);
    });
    for (size_t i = 0; i < files.size(); i++)
        for (Found& f : found[i]) rep.addAt(f.rule, files[i], f.line, f.col, std::move(f.text));
}

}  // namespace lintvhdl
