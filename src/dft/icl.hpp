// icl.hpp - IEEE 1687 (IJTAG) Instrument Connectivity Language for mdft.
//
// Reads ICL modules, elaborates the top module into a flat scan network
// (scan registers, scan multiplexers, the data signals that steer them), and
// writes that network as synthesizable SystemVerilog.
//
// Supported ICL: Module, ScanInPort, ScanOutPort, DataInPort, DataOutPort,
// the control ports (SelectPort, ShiftEnPort, CaptureEnPort, UpdateEnPort,
// TCKPort, ResetPort and the To* forms), ScanRegister (ScanInSource,
// CaptureSource, ResetValue, RefEnum), ScanMux, DataMux, LogicSignal,
// Instance (InputPort, Parameter), Alias, Enum, Parameter, LocalParameter.
// ScanInterface, AccessLink, Attribute and NameSpace statements are read and
// ignored. DataRegister (addressable registers) is not supported.
//
// Bit order: a register R[7:0] shifts from R[7] to R[0]; R[0] is the scan
// output. Internally a register's bit `pos` is its distance from the right
// index, so pos 0 is the bit that leaves first.
#pragma once
#include <cstring>
#include <functional>
#include <memory>
#include <set>

#include "jtag.hpp"

namespace dft {

namespace icl {

struct Tok {
    enum K { Id, Num, Str, Punct, End } k = End;
    std::string s;
    int line = 0;
};

inline std::vector<Tok> tokens(const std::string& src, const std::string& file) {
    std::vector<Tok> t;
    int line = 1;
    auto idc = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; };
    for (size_t i = 0; i < src.size();) {
        char c = src[i];
        if (c == '\n') { line++, i++; continue; }
        if (std::isspace((unsigned char)c)) { i++; continue; }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            size_t j = src.find("*/", i + 2);
            if (j == std::string::npos) throw Error(file + ":" + std::to_string(line) + ": unterminated comment");
            for (size_t k = i; k < j; k++)
                if (src[k] == '\n') line++;
            i = j + 2;
            continue;
        }
        if (c == '"') {
            size_t j = src.find('"', i + 1);
            if (j == std::string::npos) throw Error(file + ":" + std::to_string(line) + ": unterminated string");
            t.push_back({Tok::Str, src.substr(i + 1, j - i - 1), line});
            i = j + 1;
            continue;
        }
        if (std::isdigit((unsigned char)c) || c == '\'') {
            size_t j = i;
            while (j < src.size() && (std::isdigit((unsigned char)src[j]) || src[j] == '_')) j++;
            size_t k = j;
            while (k < src.size() && (src[k] == ' ' || src[k] == '\t')) k++;
            if (k < src.size() && src[k] == '\'' && k + 1 < src.size() && std::strchr("bBhHdD", src[k + 1])) {
                j = k + 2;
                while (j < src.size() && (std::isalnum((unsigned char)src[j]) || src[j] == '_' || src[j] == ' ')) {
                    if (src[j] == ' ' && !(j + 1 < src.size() && std::isalnum((unsigned char)src[j + 1]))) break;
                    j++;
                }
            } else if (c == '\'') throw Error(file + ":" + std::to_string(line) + ": bad number");
            std::string n;
            for (size_t q = i; q < j; q++)
                if (src[q] != '_' && src[q] != ' ' && src[q] != '\t') n += src[q];
            t.push_back({Tok::Num, n, line});
            i = j;
            continue;
        }
        if (idc(c)) {
            size_t j = i;
            while (j < src.size() && idc(src[j])) j++;
            t.push_back({Tok::Id, src.substr(i, j - i), line});
            i = j;
            continue;
        }
        if ((c == '=' || c == '!') && i + 1 < src.size() && src[i + 1] == '=') {
            t.push_back({Tok::Punct, std::string(1, c) + "=", line});
            i += 2;
            continue;
        }
        t.push_back({Tok::Punct, std::string(1, c), line});
        i++;
    }
    t.push_back({Tok::End, "", line});
    return t;
}

// A number: bits by significance (index 0 = least significant), '0' '1' 'x'.
// Unsized numbers are 32 bits wide and marked so.
struct Number {
    std::string bits;
    bool sized = false;
};
inline Number parseNumber(const std::string& s) {
    Number n;
    size_t q = s.find('\'');
    if (q == std::string::npos) {
        uint64_t v = std::strtoull(s.c_str(), nullptr, 10);
        n.bits = bitsOf(v, 32);
        return n;
    }
    int width = q ? std::atoi(s.substr(0, q).c_str()) : -1;
    char base = (char)std::tolower((unsigned char)s[q + 1]);
    std::string dig = s.substr(q + 2), bits;
    if (base == 'd') bits = bitsOf(std::strtoull(dig.c_str(), nullptr, 10), 64);
    else {
        for (auto it = dig.rbegin(); it != dig.rend(); ++it) {
            char c = (char)std::tolower((unsigned char)*it);
            if (base == 'b') bits += (c == '0' || c == '1') ? c : 'x';
            else {
                if (c == 'x') bits += "xxxx";
                else {
                    int v = std::isdigit((unsigned char)c) ? c - '0' : c - 'a' + 10;
                    bits += bitsOf((uint64_t)v, 4);
                }
            }
        }
    }
    if (width < 0) {
        n.bits = bits;
        n.bits.resize(std::max<size_t>(bits.size(), 1), '0');
        if (base == 'd') {
            size_t top = n.bits.find_last_of('1');
            n.bits.resize(top == std::string::npos ? 1 : top + 1);
        }
        n.sized = base != 'd';
        return n;
    }
    char pad = (!bits.empty() && bits.back() == 'x' && base != 'd') ? 'x' : '0';
    bits.resize(width, pad);
    n.bits = bits, n.sized = true;
    return n;
}

struct Expr {
    enum K { Ref, Const, Concat, Not, And, Or, Xor, Eq, Ne } k = Const;
    std::string name;  // Ref: a.b.c
    bool hasIdx = false;
    int hi = 0, lo = 0;
    Number num;
    std::vector<Expr> kids;
    int line = 0;
};

struct PortDecl {
    std::string kind, name;
    bool vec = false;
    int msb = 0, lsb = 0;
    bool hasSource = false;
    Expr source;
    int width() const { return vec ? std::abs(msb - lsb) + 1 : 1; }
};
struct RegDecl {
    std::string name;
    bool vec = false;
    int msb = 0, lsb = 0;
    Expr scanIn;
    bool hasScanIn = false, hasCapture = false, hasReset = false;
    Expr capture;
    Number reset;
    std::string refEnum;
    int line = 0;
    int width() const { return vec ? std::abs(msb - lsb) + 1 : 1; }
};
struct MuxDecl {
    std::string name;
    bool data = false, vec = false;
    int msb = 0, lsb = 0;
    Expr sel;
    std::vector<std::pair<Expr, Expr>> rows;  // select value (constant or enum name) : source
    int line = 0;
};
struct InstDecl {
    std::string name, module;
    std::map<std::string, Expr> conns;
    std::vector<std::pair<std::string, std::vector<Tok>>> params;
    int line = 0;
};
struct AliasDecl {
    std::string name;
    bool vec = false;
    int msb = 0, lsb = 0;
    Expr target;
    std::string refEnum;
};
struct ModuleBody {
    std::string name;
    std::vector<PortDecl> ports;
    std::vector<RegDecl> regs;
    std::vector<MuxDecl> muxes;
    std::vector<InstDecl> insts;
    std::map<std::string, Expr> logic;
    std::map<std::string, AliasDecl> aliases;
    std::map<std::string, std::map<std::string, Number>> enums;
    const PortDecl* port(const std::string& n) const {
        for (const PortDecl& p : ports)
            if (p.name == n) return &p;
        return nullptr;
    }
    const RegDecl* reg(const std::string& n) const {
        for (const RegDecl& r : regs)
            if (r.name == n) return &r;
        return nullptr;
    }
    const MuxDecl* mux(const std::string& n) const {
        for (const MuxDecl& m : muxes)
            if (m.name == n) return &m;
        return nullptr;
    }
    const InstDecl* inst(const std::string& n) const {
        for (const InstDecl& i : insts)
            if (i.name == n) return &i;
        return nullptr;
    }
};

// The text of one module, parsed again for each set of parameter values.
struct ModuleSrc {
    std::string name, file;
    std::vector<Tok> body;  // between the braces
};

struct Library {
    std::map<std::string, ModuleSrc> modules;
    std::vector<std::string> order;

    void read(const std::string& src, const std::string& file) {
        std::vector<Tok> t = tokens(src, file);
        size_t i = 0;
        auto err = [&](const std::string& m) { return Error(file + ":" + std::to_string(t[i].line) + ": " + m); };
        while (t[i].k != Tok::End) {
            if (t[i].k == Tok::Id && t[i].s == "Module") {
                ModuleSrc m;
                m.file = file;
                i++;
                if (t[i].k != Tok::Id) throw err("expected a module name");
                m.name = t[i++].s;
                if (t[i].s != "{") throw err("expected '{' after Module " + m.name);
                i++;
                int depth = 1;
                while (t[i].k != Tok::End) {
                    if (t[i].k == Tok::Punct && t[i].s == "{") depth++;
                    if (t[i].k == Tok::Punct && t[i].s == "}" && --depth == 0) break;
                    m.body.push_back(t[i++]);
                }
                if (t[i].k == Tok::End) throw err("Module " + m.name + " is not closed");
                i++;
                m.body.push_back({Tok::End, "", t[i - 1].line});
                if (modules.count(m.name)) throw err("Module " + m.name + " is defined twice");
                order.push_back(m.name);
                modules[m.name] = std::move(m);
            } else if (t[i].k == Tok::Id && (t[i].s == "NameSpace" || t[i].s == "UseNameSpace")) {
                while (t[i].k != Tok::End && t[i].s != ";") i++;
                i++;
            } else throw err("expected 'Module', found '" + t[i].s + "'");
        }
    }
};

// Parses one module body with its parameters bound.
struct BodyParser {
    const ModuleSrc& src;
    std::map<std::string, std::vector<Tok>> params;  // name -> value tokens
    const std::vector<Tok>* t;
    size_t i = 0;
    std::vector<Tok> expanded;

    Error err(const std::string& m) const { return Error(src.file + ":" + std::to_string((*t)[i].line) + ": " + m); }
    bool is(const char* p) const { return (*t)[i].k == Tok::Punct && (*t)[i].s == p; }
    bool kw(const char* w) const { return (*t)[i].k == Tok::Id && (*t)[i].s == w; }
    void expect(const char* p) {
        if (!is(p)) throw err(std::string("expected '") + p + "' before '" + (*t)[i].s + "'");
        i++;
    }
    std::string ident() {
        if ((*t)[i].k != Tok::Id) throw err("expected a name, found '" + (*t)[i].s + "'");
        return (*t)[i++].s;
    }
    // Replace $name by its value everywhere; parameters are collected first.
    void expand() {
        const std::vector<Tok>& in = src.body;
        // Pass 1: Parameter / LocalParameter defaults (overrides win).
        for (size_t k = 0; k + 1 < in.size(); k++) {
            if (in[k].k == Tok::Id && (in[k].s == "Parameter" || in[k].s == "LocalParameter") && in[k + 1].k == Tok::Id &&
                (k == 0 || in[k - 1].s == ";" || in[k - 1].s == "}" || in[k - 1].s == "{")) {
                // Only module-level ones: depth 0.
                int depth = 0;
                for (size_t q = 0; q < k; q++) {
                    if (in[q].k == Tok::Punct && in[q].s == "{") depth++;
                    if (in[q].k == Tok::Punct && in[q].s == "}") depth--;
                }
                if (depth != 0) continue;
                std::string name = in[k + 1].s;
                std::vector<Tok> val;
                size_t q = k + 3;
                while (q < in.size() && in[q].s != ";") val.push_back(in[q++]);
                if (in[k].s == "LocalParameter" || !params.count(name)) params[name] = val;
            }
        }
        std::function<void(const std::vector<Tok>&, int)> sub = [&](const std::vector<Tok>& v, int depth) {
            if (depth > 16) throw Error(src.file + ": parameters of " + src.name + " refer to each other");
            for (const Tok& tk : v) {
                if (tk.k == Tok::Id && tk.s[0] == '$') {
                    auto it = params.find(tk.s.substr(1));
                    if (it == params.end())
                        throw Error(src.file + ":" + std::to_string(tk.line) + ": unknown parameter " + tk.s);
                    size_t before = expanded.size();
                    sub(it->second, depth + 1);
                    for (size_t q = before; q < expanded.size(); q++) expanded[q].line = tk.line;
                } else if (tk.k != Tok::End) expanded.push_back(tk);
            }
        };
        sub(in, 0);
        expanded.push_back({Tok::End, "", in.empty() ? 0 : in.back().line});
        t = &expanded;
    }

    long intPrimary() {
        if (is("(")) {
            i++;
            long v = intExpr();
            expect(")");
            return v;
        }
        if (is("-")) {
            i++;
            return -intPrimary();
        }
        if ((*t)[i].k != Tok::Num) throw err("expected a number, found '" + (*t)[i].s + "'");
        Number n = parseNumber((*t)[i++].s);
        long v = 0;
        for (size_t b = 0; b < n.bits.size() && b < 62; b++)
            if (n.bits[b] == '1') v |= 1L << b;
        return v;
    }
    long intTerm() {
        long v = intPrimary();
        while (is("*") || is("/") || is("%")) {
            char op = (*t)[i++].s[0];
            long r = intPrimary();
            if (op != '*' && r == 0) throw err("division by zero");
            v = op == '*' ? v * r : op == '/' ? v / r : v % r;
        }
        return v;
    }
    long intExpr() {
        long v = intTerm();
        while (is("+") || is("-")) {
            char op = (*t)[i++].s[0];
            long r = intTerm();
            v = op == '+' ? v + r : v - r;
        }
        return v;
    }
    // [msb:lsb] or [i] after a declared name.
    void range(bool& vec, int& msb, int& lsb) {
        if (!is("[")) return;
        i++;
        vec = true;
        msb = lsb = (int)intExpr();
        if (is(":")) {
            i++;
            lsb = (int)intExpr();
        }
        expect("]");
    }
    Expr unary() {
        Expr e;
        e.line = (*t)[i].line;
        if (is("~") || is("!")) {
            i++;
            e.k = Expr::Not;
            e.kids.push_back(unary());
            return e;
        }
        if (is("(")) {
            i++;
            e = concat();
            expect(")");
            return e;
        }
        if ((*t)[i].k == Tok::Num) {
            // A number, or a constant expression such as 4-1 (after parameter expansion).
            if ((*t)[i].s.find('\'') != std::string::npos) {
                e.k = Expr::Const;
                e.num = parseNumber((*t)[i++].s);
                return e;
            }
            long v = intExpr();
            e.k = Expr::Const;
            e.num.bits = bitsOf((uint64_t)v, 32);
            return e;
        }
        e.k = Expr::Ref;
        e.name = ident();
        while (is(".")) {
            i++;
            e.name += "." + ident();
        }
        if (is("[")) {
            i++;
            e.hasIdx = true;
            e.hi = e.lo = (int)intExpr();
            if (is(":")) {
                i++;
                e.lo = (int)intExpr();
            }
            expect("]");
        }
        return e;
    }
    Expr binary() {
        Expr l = unary();
        while (is("&") || is("|") || is("^") || is("==") || is("!=")) {
            std::string op = (*t)[i++].s;
            Expr e;
            e.line = l.line;
            e.k = op == "&" ? Expr::And : op == "|" ? Expr::Or : op == "^" ? Expr::Xor : op == "==" ? Expr::Eq : Expr::Ne;
            e.kids.push_back(std::move(l));
            e.kids.push_back(unary());
            l = std::move(e);
        }
        return l;
    }
    Expr concat() {
        Expr first = binary();
        if (!is(",")) return first;
        Expr e;
        e.k = Expr::Concat;
        e.line = first.line;
        e.kids.push_back(std::move(first));
        while (is(",")) {
            i++;
            e.kids.push_back(binary());
        }
        return e;
    }
    void skipStatement() {
        int depth = 0;
        while ((*t)[i].k != Tok::End) {
            if (is("{")) depth++;
            else if (is("}")) {
                if (--depth == 0) { i++; return; }
            } else if (is(";") && depth == 0) { i++; return; }
            i++;
        }
    }

    ModuleBody parse() {
        expand();
        ModuleBody m;
        m.name = src.name;
        static const std::set<std::string> portKinds{
            "ScanInPort", "ScanOutPort", "DataInPort", "DataOutPort", "SelectPort", "ShiftEnPort", "CaptureEnPort",
            "UpdateEnPort", "TCKPort", "ResetPort", "ToSelectPort", "ToShiftEnPort", "ToCaptureEnPort", "ToUpdateEnPort",
            "ToTCKPort", "ToResetPort", "ClockPort", "ToClockPort", "TMSPort", "ToTMSPort", "TRSTPort", "ToTRSTPort",
            "AddressPort", "WriteEnPort", "ReadEnPort", "ToIRSelectPort"};
        while ((*t)[i].k != Tok::End) {
            int line = (*t)[i].line;
            std::string k = ident();
            if (portKinds.count(k)) {
                PortDecl p;
                p.kind = k;
                p.name = ident();
                range(p.vec, p.msb, p.lsb);
                if (is("{")) {
                    i++;
                    while (!is("}")) {
                        if ((*t)[i].k == Tok::End) throw err("port " + p.name + " is not closed");
                        if (kw("Source")) {
                            i++;
                            p.source = concat();
                            p.hasSource = true;
                            expect(";");
                        } else skipStatement();
                    }
                    i++;
                } else expect(";");
                m.ports.push_back(std::move(p));
            } else if (k == "ScanRegister") {
                RegDecl r;
                r.line = line;
                r.name = ident();
                range(r.vec, r.msb, r.lsb);
                expect("{");
                while (!is("}")) {
                    if ((*t)[i].k == Tok::End) throw err("ScanRegister " + r.name + " is not closed");
                    std::string a = ident();
                    if (a == "ScanInSource") r.scanIn = unary(), r.hasScanIn = true, expect(";");
                    else if (a == "CaptureSource") r.capture = concat(), r.hasCapture = true, expect(";");
                    else if (a == "ResetValue") {
                        if ((*t)[i].k != Tok::Num) throw err("ResetValue of " + r.name + " must be a number");
                        r.reset = parseNumber((*t)[i++].s), r.hasReset = true;
                        expect(";");
                    } else if (a == "RefEnum") r.refEnum = ident(), expect(";");
                    else { i--; skipStatement(); }
                }
                i++;
                if (!r.hasScanIn) throw Error(src.file + ":" + std::to_string(line) + ": ScanRegister " + r.name + " has no ScanInSource");
                m.regs.push_back(std::move(r));
            } else if (k == "ScanMux" || k == "DataMux") {
                MuxDecl x;
                x.line = line;
                x.data = k == "DataMux";
                x.name = ident();
                range(x.vec, x.msb, x.lsb);
                if (!kw("SelectedBy")) throw err("expected SelectedBy");
                i++;
                x.sel = concat();
                expect("{");
                while (!is("}")) {
                    if ((*t)[i].k == Tok::End) throw err(k + " " + x.name + " is not closed");
                    std::vector<Expr> vals;
                    vals.push_back(unary());
                    while (is("|")) {  // several select values for one source
                        i++;
                        vals.push_back(unary());
                    }
                    expect(":");
                    Expr s = concat();
                    expect(";");
                    for (Expr& v : vals) x.rows.push_back({std::move(v), s});
                }
                i++;
                m.muxes.push_back(std::move(x));
            } else if (k == "LogicSignal") {
                std::string name = ident();
                expect("{");
                m.logic[name] = concat();
                expect(";");
                expect("}");
            } else if (k == "Instance") {
                InstDecl in;
                in.line = line;
                in.name = ident();
                if (!kw("Of")) throw err("expected Of");
                i++;
                in.module = ident();
                while (is(":")) {  // NameSpace::Module
                    i++;
                    if (is(":")) i++;
                    in.module = ident();
                }
                if (is("{")) {
                    i++;
                    while (!is("}")) {
                        if ((*t)[i].k == Tok::End) throw err("Instance " + in.name + " is not closed");
                        std::string a = ident();
                        if (a == "InputPort") {
                            std::string pn = ident();
                            expect("=");
                            in.conns[pn] = concat();
                            expect(";");
                        } else if (a == "Parameter") {
                            std::string pn = ident();
                            expect("=");
                            std::vector<Tok> val;
                            while (!is(";") && (*t)[i].k != Tok::End) val.push_back((*t)[i++]);
                            expect(";");
                            in.params.push_back({pn, val});
                        } else { i--; skipStatement(); }
                    }
                    i++;
                } else expect(";");
                m.insts.push_back(std::move(in));
            } else if (k == "Alias") {
                AliasDecl a;
                a.name = ident();
                range(a.vec, a.msb, a.lsb);
                expect("=");
                a.target = concat();
                if (is("{")) {
                    i++;
                    while (!is("}")) {
                        if ((*t)[i].k == Tok::End) throw err("Alias " + a.name + " is not closed");
                        if (kw("RefEnum")) { i++; a.refEnum = ident(); expect(";"); }
                        else skipStatement();
                    }
                    i++;
                } else expect(";");
                m.aliases[a.name] = std::move(a);
            } else if (k == "Enum") {
                std::string name = ident();
                expect("{");
                while (!is("}")) {
                    if ((*t)[i].k == Tok::End) throw err("Enum " + name + " is not closed");
                    std::string sym = ident();
                    expect("=");
                    if ((*t)[i].k != Tok::Num) throw err("Enum value must be a number");
                    m.enums[name][sym] = parseNumber((*t)[i++].s);
                    expect(";");
                }
                i++;
            } else if (k == "DataRegister") {
                throw Error(src.file + ":" + std::to_string(line) + ": DataRegister (addressable registers) is not supported");
            } else {
                // Parameter, LocalParameter, Attribute, ScanInterface, AccessLink, ...
                i--;
                skipStatement();
            }
        }
        return m;
    }
};

// ---------------------------------------------------------------------------
// The elaborated network.

struct DBit {
    enum K : uint8_t { C0, C1, CX, Reg, Pin, Ex } k = CX;
    int id = 0, bit = 0;
};
using DVec = std::vector<DBit>;  // index 0 = least significant

struct ScanRef {
    enum K : uint8_t { None, Reg, Mux, TopSI } k = None;
    int id = 0;
};

struct FReg {
    std::string name;   // hierarchical, dots
    int width = 1, msb = 0, lsb = 0;
    bool vec = false;
    ScanRef si;
    bool hasCapture = false;
    DVec capture;
    std::string reset;  // by pos; 'x' when there is no ResetValue
    std::string value;  // the update stage, by pos
    std::string ctxPath, enumName;
    const ModuleBody* mod = nullptr;
    int posOf(int index) const { return std::abs(index - lsb); }
};
struct FMux {
    std::string name;
    DVec sel;
    std::vector<std::pair<std::string, ScanRef>> rows;  // pattern by significance (may hold x)
};
struct FExpr {
    Expr::K op = Expr::Not;
    std::vector<DVec> args;
    std::vector<std::string> pats;  // data mux: row patterns; args[0] = select, args[1..] = rows
    bool dmux = false;
    int width = 1;
};
struct FPin {
    std::string name;  // hierarchical
    int width = 1;
    bool vec = false;
    int msb = 0, lsb = 0;
    bool instrument = false;  // an instrument's DataOutPort (true) or a top-level DataInPort
};
struct FObs {  // a value the network presents: a top DataOutPort or an instrument's DataInPort
    std::string name;
    DVec bits;
};

struct Ctx {
    std::string path;
    std::shared_ptr<ModuleBody> mod;
    Ctx* parent = nullptr;
    const InstDecl* inst = nullptr;
    std::map<std::string, std::unique_ptr<Ctx>> kids;
};

struct Network {
    Library lib;
    std::string top;
    std::unique_ptr<Ctx> root;
    std::vector<FReg> regs;
    std::vector<FMux> muxes;
    std::vector<FExpr> exprs;
    std::vector<FPin> pins;
    std::vector<FObs> obs;
    ScanRef so;
    std::map<std::string, int> regIdx, pinIdx;
    std::map<std::pair<const Ctx*, std::string>, int> muxIdx;
    std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> pinCapture;  // pin bit -> reg bits that capture it
    std::map<std::string, std::shared_ptr<ModuleBody>> parsed;
    std::set<std::pair<const Ctx*, std::string>> busy;

    static std::string join(const std::string& a, const std::string& b) { return a.empty() ? b : a + "." + b; }
    Error err(const Ctx& c, int line, const std::string& m) const {
        return Error(lib.modules.at(c.mod->name).file + ":" + std::to_string(line) + ": " + m);
    }

    std::shared_ptr<ModuleBody> body(const std::string& module, const InstDecl* inst, const Ctx* parent) {
        auto it = lib.modules.find(module);
        if (it == lib.modules.end())
            throw Error("ICL: no Module " + module + (inst ? " (instance " + join(parent->path, inst->name) + ")" : ""));
        std::string key = module;
        BodyParser bp{it->second};
        if (inst)
            for (auto& [n, v] : inst->params) {
                bp.params[n] = v;
                key += "|" + n + "=";
                for (const Tok& tk : v) key += tk.s + " ";
            }
        auto hit = parsed.find(key);
        if (hit != parsed.end()) return hit->second;
        auto b = std::make_shared<ModuleBody>(bp.parse());
        parsed[key] = b;
        return b;
    }

    void build(Ctx& c, int depth) {
        if (depth > 64) throw Error("ICL: instances nest more than 64 deep (a module instantiates itself?)");
        for (const InstDecl& in : c.mod->insts) {
            auto k = std::make_unique<Ctx>();
            k->path = join(c.path, in.name);
            k->mod = body(in.module, &in, &c);
            k->parent = &c;
            k->inst = &in;
            build(*k, depth + 1);
            c.kids[in.name] = std::move(k);
        }
        for (const RegDecl& r : c.mod->regs) {
            FReg f;
            f.name = join(c.path, r.name);
            f.width = r.width(), f.msb = r.msb, f.lsb = r.lsb, f.vec = r.vec;
            f.reset.assign(f.width, 'x');
            if (r.hasReset)
                for (int b = 0; b < f.width; b++) f.reset[b] = b < (int)r.reset.bits.size() ? r.reset.bits[b] : '0';
            f.value = f.reset;
            f.ctxPath = c.path, f.enumName = r.refEnum, f.mod = c.mod.get();
            regIdx[f.name] = (int)regs.size();
            regs.push_back(std::move(f));
        }
    }

    // Split "a.b.c" at the first dot when `a` is an instance here.
    Ctx* child(Ctx& c, const std::string& name, std::string& rest) {
        size_t dot = name.find('.');
        if (dot == std::string::npos) return nullptr;
        auto it = c.kids.find(name.substr(0, dot));
        if (it == c.kids.end()) return nullptr;
        rest = name.substr(dot + 1);
        return it->second.get();
    }

    ScanRef scan(Ctx& c, const Expr& e) {
        if (e.k != Expr::Ref) throw err(c, e.line, "a scan source must be a signal name");
        std::string rest;
        if (Ctx* k = child(c, e.name, rest)) {
            const PortDecl* p = k->mod->port(rest);
            if (p && p->kind == "ScanOutPort") {
                if (!p->hasSource) throw err(c, e.line, "ScanOutPort " + e.name + " has no Source");
                return scan(*k, p->source);
            }
            Expr sub = e;
            sub.name = rest;
            return scan(*k, sub);
        }
        if (c.mod->reg(e.name)) return {ScanRef::Reg, regIdx.at(join(c.path, e.name))};
        if (const MuxDecl* m = c.mod->mux(e.name); m && !m->data) {
            auto key = std::make_pair((const Ctx*)&c, e.name);
            auto it = muxIdx.find(key);
            if (it != muxIdx.end()) return {ScanRef::Mux, it->second};
            int id = (int)muxes.size();
            muxIdx[key] = id;
            muxes.emplace_back();
            FMux f;
            f.name = join(c.path, e.name);
            f.sel = data(c, m->sel);
            for (auto& [v, s] : m->rows) f.rows.push_back({pattern(c, v, (int)f.sel.size(), m->sel), scan(c, s)});
            muxes[id] = std::move(f);
            return {ScanRef::Mux, id};
        }
        if (const PortDecl* p = c.mod->port(e.name)) {
            if (p->kind == "ScanInPort") {
                if (!c.parent) return {ScanRef::TopSI, 0};
                auto it = c.inst->conns.find(e.name);
                if (it == c.inst->conns.end())
                    throw err(*c.parent, c.inst->line, "instance " + c.path + ": ScanInPort " + e.name + " is not connected");
                return scan(*c.parent, it->second);
            }
            if (p->kind == "ScanOutPort" && p->hasSource) return scan(c, p->source);
        }
        throw err(c, e.line, "'" + e.name + "' is not a scan signal in " + c.mod->name);
    }

    // A mux row value: a number, or an enum symbol of the select register.
    std::string pattern(Ctx& c, const Expr& v, int width, const Expr& sel) {
        std::string bits;
        if (v.k == Expr::Const) bits = v.num.bits;
        else if (v.k == Expr::Ref) {
            const Number* found = nullptr;
            std::string en;
            if (sel.k == Expr::Ref) {
                if (const RegDecl* r = c.mod->reg(sel.name)) en = r->refEnum;
                if (auto a = c.mod->aliases.find(sel.name); a != c.mod->aliases.end()) en = a->second.refEnum;
            }
            for (auto& [name, syms] : c.mod->enums) {
                if (!en.empty() && name != en) continue;
                auto it = syms.find(v.name);
                if (it != syms.end()) found = &it->second;
            }
            if (!found) throw err(c, v.line, "'" + v.name + "' is not a number or an enum value");
            bits = found->bits;
        } else throw err(c, v.line, "a select value must be a constant");
        bits.resize(width, '0');
        return bits;
    }

    DVec constant(const Number& n) {
        DVec v;
        for (char b : n.bits) v.push_back({b == '0' ? DBit::C0 : b == '1' ? DBit::C1 : DBit::CX});
        return v;
    }
    int addPin(const std::string& name, const PortDecl& p, bool instrument) {
        auto it = pinIdx.find(name);
        if (it != pinIdx.end()) return it->second;
        FPin f;
        f.name = name, f.width = p.width(), f.vec = p.vec, f.msb = p.msb, f.lsb = p.lsb, f.instrument = instrument;
        pinIdx[name] = (int)pins.size();
        pins.push_back(f);
        return (int)pins.size() - 1;
    }
    DVec slice(Ctx& c, const Expr& e, DVec full, bool vec, int msb, int lsb) {
        if (!e.hasIdx) return full;
        int lo = vec ? std::abs(e.lo - lsb) : e.lo, hi = vec ? std::abs(e.hi - lsb) : e.hi;
        if (lo > hi) std::swap(lo, hi);
        bool inRange = vec ? (e.hi >= std::min(msb, lsb) && e.hi <= std::max(msb, lsb) && e.lo >= std::min(msb, lsb) &&
                              e.lo <= std::max(msb, lsb))
                           : hi < (int)full.size();
        if (!inRange || hi >= (int)full.size()) throw err(c, e.line, "the index of '" + e.name + "' is out of range");
        return DVec(full.begin() + lo, full.begin() + hi + 1);
    }

    DVec data(Ctx& c, const Expr& e) {
        switch (e.k) {
        case Expr::Const: return constant(e.num);
        case Expr::Concat: {
            DVec v;
            for (auto it = e.kids.rbegin(); it != e.kids.rend(); ++it) {
                DVec k = data(c, *it);
                if (it->k == Expr::Const && !it->num.sized) throw err(c, e.line, "an unsized number in a concatenation");
                v.insert(v.end(), k.begin(), k.end());
            }
            return v;
        }
        case Expr::Not:
        case Expr::And:
        case Expr::Or:
        case Expr::Xor:
        case Expr::Eq:
        case Expr::Ne: {
            FExpr f;
            f.op = e.k;
            for (const Expr& k : e.kids) f.args.push_back(data(c, k));
            size_t w = f.args[0].size();
            if (f.args.size() == 2) {
                // An unsized constant takes the other side's width.
                for (int s = 0; s < 2; s++)
                    if (e.kids[s].k == Expr::Const && !e.kids[s].num.sized) f.args[s].resize(f.args[1 - s].size(), DBit{DBit::C0});
                w = std::max(f.args[0].size(), f.args[1].size());
                f.args[0].resize(w, DBit{DBit::C0});
                f.args[1].resize(w, DBit{DBit::C0});
            }
            f.width = (e.k == Expr::Eq || e.k == Expr::Ne) ? 1 : (int)w;
            int id = (int)exprs.size();
            exprs.push_back(std::move(f));
            DVec v;
            for (int b = 0; b < exprs[id].width; b++) v.push_back({DBit::Ex, id, b});
            return v;
        }
        case Expr::Ref: break;
        }
        std::string rest;
        if (Ctx* k = child(c, e.name, rest)) {
            Expr sub = e;
            sub.name = rest;
            return data(*k, sub);
        }
        auto key = std::make_pair((const Ctx*)&c, e.name);
        if (!busy.insert(key).second) throw err(c, e.line, "'" + e.name + "' depends on itself");
        struct Done {
            std::set<std::pair<const Ctx*, std::string>>& s;
            std::pair<const Ctx*, std::string> k;
            ~Done() { s.erase(k); }
        } done{busy, key};
        if (const RegDecl* r = c.mod->reg(e.name)) {
            int id = regIdx.at(join(c.path, e.name));
            DVec full;
            for (int b = 0; b < r->width(); b++) full.push_back({DBit::Reg, id, b});
            return slice(c, e, full, r->vec, r->msb, r->lsb);
        }
        if (auto a = c.mod->aliases.find(e.name); a != c.mod->aliases.end())
            return slice(c, e, data(c, a->second.target), a->second.vec, a->second.msb, a->second.lsb);
        if (auto l = c.mod->logic.find(e.name); l != c.mod->logic.end()) return slice(c, e, data(c, l->second), false, 0, 0);
        if (const MuxDecl* m = c.mod->mux(e.name); m && m->data) {
            FExpr f;
            f.dmux = true;
            f.args.push_back(data(c, m->sel));
            int w = m->vec ? std::abs(m->msb - m->lsb) + 1 : 1;
            for (auto& [v, s] : m->rows) {
                f.pats.push_back(pattern(c, v, (int)f.args[0].size(), m->sel));
                DVec d = data(c, s);
                d.resize(w, DBit{DBit::C0});
                f.args.push_back(d);
            }
            f.width = w;
            int id = (int)exprs.size();
            exprs.push_back(std::move(f));
            DVec full;
            for (int b = 0; b < w; b++) full.push_back({DBit::Ex, id, b});
            return slice(c, e, full, m->vec, m->msb, m->lsb);
        }
        if (const PortDecl* p = c.mod->port(e.name)) {
            DVec full;
            if (p->kind == "DataInPort") {
                if (!c.parent) {
                    int id = addPin(e.name, *p, false);
                    for (int b = 0; b < p->width(); b++) full.push_back({DBit::Pin, id, b});
                } else {
                    auto it = c.inst->conns.find(e.name);
                    if (it == c.inst->conns.end()) full.assign(p->width(), DBit{DBit::CX});
                    else {
                        full = data(*c.parent, it->second);
                        full.resize(p->width(), DBit{DBit::C0});
                    }
                }
            } else if (p->kind == "DataOutPort") {
                if (p->hasSource) {
                    full = data(c, p->source);
                    full.resize(p->width(), DBit{DBit::C0});
                } else {
                    int id = addPin(join(c.path, e.name), *p, true);
                    for (int b = 0; b < p->width(); b++) full.push_back({DBit::Pin, id, b});
                }
            } else throw err(c, e.line, "'" + e.name + "' (" + p->kind + ") is not a data signal");
            return slice(c, e, full, p->vec, p->msb, p->lsb);
        }
        throw err(c, e.line, "'" + e.name + "' is not a data signal in " + c.mod->name);
    }

    void walk(Ctx& c, const std::function<void(Ctx&)>& f) {
        f(c);
        for (auto& [n, k] : c.kids) walk(*k, f);
    }

    void elaborate(const std::string& topName, const std::string& scanOut) {
        top = topName;
        if (top.empty()) {
            // The module nobody instantiates; the last one if several.
            std::set<std::string> used;
            for (auto& [n, m] : lib.modules)
                for (size_t k = 0; k + 3 < m.body.size(); k++)
                    if (m.body[k].k == Tok::Id && m.body[k].s == "Instance" && m.body[k + 2].s == "Of") used.insert(m.body[k + 3].s);
            for (const std::string& n : lib.order)
                if (!used.count(n)) top = n;
            if (top.empty()) throw Error("ICL: no top module (every module is instantiated)");
        }
        root = std::make_unique<Ctx>();
        root->mod = body(top, nullptr, nullptr);
        build(*root, 0);
        const PortDecl* soPort = nullptr;
        for (const PortDecl& p : root->mod->ports)
            if (p.kind == "ScanOutPort" && (scanOut.empty() ? !soPort : p.name == scanOut)) soPort = &p;
        if (!soPort) throw Error("ICL: " + top + " has no ScanOutPort" + (scanOut.empty() ? "" : " named " + scanOut));
        if (!soPort->hasSource) throw Error("ICL: ScanOutPort " + soPort->name + " of " + top + " has no Source");
        // Every register's scan input, so that the whole network is known even
        // where the reset state leaves it off the path.
        so = scan(*root, soPort->source);
        walk(*root, [&](Ctx& c) {
            for (const RegDecl& r : c.mod->regs) {
                int id = regIdx.at(join(c.path, r.name));
                regs[id].si = scan(c, r.scanIn);
                if (r.hasCapture) {
                    DVec cap = data(c, r.capture);
                    cap.resize(r.width(), DBit{DBit::C0});
                    regs[id].capture = cap, regs[id].hasCapture = true;
                    for (int b = 0; b < r.width(); b++)
                        if (cap[b].k == DBit::Pin) pinCapture[{cap[b].id, cap[b].bit}].push_back({id, b});
                }
            }
        });
        // What the network shows: top-level DataOutPorts, and the DataInPorts
        // of instruments (modules with no scan logic of their own).
        for (const PortDecl& p : root->mod->ports)
            if (p.kind == "DataOutPort" && p.hasSource) {
                Expr e;
                e.k = Expr::Ref, e.name = p.name;
                obs.push_back({p.name, data(*root, e)});
            }
        walk(*root, [&](Ctx& c) {
            if (!c.parent || !c.mod->regs.empty() || !c.mod->insts.empty() || !c.mod->muxes.empty()) return;
            for (const PortDecl& p : c.mod->ports)
                if (p.kind == "DataInPort" && c.inst->conns.count(p.name)) {
                    Expr e;
                    e.k = Expr::Ref, e.name = p.name;
                    obs.push_back({join(c.path, p.name), data(c, e)});
                }
        });
    }

    // --- state ---------------------------------------------------------------
    std::vector<std::string> pinValue;  // what the testbench drives, by pin

    void resetState() {
        for (FReg& r : regs) r.value = r.reset;
    }
    char evalBit(const DBit& b) const {
        switch (b.k) {
        case DBit::C0: return '0';
        case DBit::C1: return '1';
        case DBit::CX: return 'x';
        case DBit::Reg: return regs[b.id].value[b.bit];
        case DBit::Pin: return b.id < (int)pinValue.size() && b.bit < (int)pinValue[b.id].size() ? pinValue[b.id][b.bit] : 'x';
        case DBit::Ex: return evalExpr(b.id)[b.bit];
        }
        return 'x';
    }
    std::string eval(const DVec& v) const {
        std::string s(v.size(), 'x');
        for (size_t i = 0; i < v.size(); i++) s[i] = evalBit(v[i]);
        return s;
    }
    static bool matches(const std::string& val, const std::string& pat) {
        for (size_t i = 0; i < pat.size(); i++) {
            if (pat[i] == 'x') continue;
            if (i >= val.size() || val[i] != pat[i]) return false;
        }
        return true;
    }
    std::string evalExpr(int id) const {
        const FExpr& f = exprs[id];
        std::string out(f.width, 'x');
        if (f.dmux) {
            std::string sel = eval(f.args[0]);
            if (sel.find('x') != std::string::npos) return out;
            for (size_t r = 0; r < f.pats.size(); r++)
                if (matches(sel, f.pats[r])) return eval(f.args[r + 1]);
            return out;
        }
        std::string a = eval(f.args[0]), b = f.args.size() > 1 ? eval(f.args[1]) : "";
        auto known = [](char c) { return c != 'x'; };
        switch (f.op) {
        case Expr::Not:
            for (size_t i = 0; i < a.size(); i++) out[i] = known(a[i]) ? (a[i] == '0' ? '1' : '0') : 'x';
            break;
        case Expr::And:
            for (size_t i = 0; i < a.size(); i++)
                out[i] = (a[i] == '0' || b[i] == '0') ? '0' : (a[i] == '1' && b[i] == '1') ? '1' : 'x';
            break;
        case Expr::Or:
            for (size_t i = 0; i < a.size(); i++)
                out[i] = (a[i] == '1' || b[i] == '1') ? '1' : (a[i] == '0' && b[i] == '0') ? '0' : 'x';
            break;
        case Expr::Xor:
            for (size_t i = 0; i < a.size(); i++) out[i] = (known(a[i]) && known(b[i])) ? (a[i] != b[i] ? '1' : '0') : 'x';
            break;
        case Expr::Eq:
        case Expr::Ne: {
            bool unknown = false, diff = false;
            for (size_t i = 0; i < a.size(); i++) {
                if (!known(a[i]) || !known(b[i])) unknown = true;
                else if (a[i] != b[i]) diff = true;
            }
            out[0] = diff ? (f.op == Expr::Eq ? '0' : '1') : unknown ? 'x' : (f.op == Expr::Eq ? '1' : '0');
            break;
        }
        default: break;
        }
        return out;
    }
    // The branch a mux selects now, or -1 (select unknown or no row matches).
    int selected(const FMux& m) const {
        std::string sel = eval(m.sel);
        for (size_t r = 0; r < m.rows.size(); r++) {
            bool ok = true;
            for (size_t i = 0; i < sel.size() && ok; i++)
                if (m.rows[r].first[i] != 'x' && sel[i] != m.rows[r].first[i]) ok = false;
            if (ok) return (int)r;
        }
        return -1;
    }
    // The registers between scan-in and scan-out now, scan-out end first.
    std::vector<int> activePath() const {
        std::vector<int> path;
        ScanRef at = so;
        size_t guard = 0, limit = regs.size() + muxes.size() + 2;
        while (at.k != ScanRef::TopSI) {
            if (++guard > limit) throw Error("IJTAG: the active scan path loops");
            if (at.k == ScanRef::Reg) {
                path.push_back(at.id);
                at = regs[at.id].si;
            } else if (at.k == ScanRef::Mux) {
                int r = selected(muxes[at.id]);
                if (r < 0)
                    throw Error("IJTAG: the select of ScanMux " + muxes[at.id].name + " is " + toMsbFirst(eval(muxes[at.id].sel)) +
                                ", which picks no input (missing ResetValue?)");
                at = muxes[at.id].rows[r].second;
            } else throw Error("IJTAG: a scan source is unconnected");
        }
        return path;
    }

    // --- RTL -----------------------------------------------------------------
    static std::string ident(std::string s) {
        for (char& c : s)
            if (!std::isalnum((unsigned char)c) && c != '_') c = '_';
        return s;
    }
    std::string vbit(const DBit& b) const {
        switch (b.k) {
        case DBit::C0: return "1'b0";
        case DBit::C1: return "1'b1";
        case DBit::CX: return "1'b0";
        case DBit::Reg: return "r" + std::to_string(b.id) + "_upd[" + std::to_string(b.bit) + "]";
        case DBit::Pin: return ident(pins[b.id].name) + "[" + std::to_string(b.bit) + "]";
        case DBit::Ex: return "e" + std::to_string(b.id) + "[" + std::to_string(b.bit) + "]";
        }
        return "1'b0";
    }
    std::string vvec(const DVec& v) const {
        if (v.size() == 1) return vbit(v[0]);
        std::string s = "{";
        for (size_t i = v.size(); i-- > 0;) s += vbit(v[i]) + (i ? ", " : "");
        return s + "}";
    }
    std::string vscan(const ScanRef& r) const {
        if (r.k == ScanRef::Reg) return "r" + std::to_string(r.id) + "_sr[0]";
        if (r.k == ScanRef::Mux) return "m" + std::to_string(r.id) + "_so";
        return "si";
    }
    static std::string vmatch(const std::string& sel, const std::string& pat) {
        std::string mask = careOf(pat), val = pat;
        for (char& c : val)
            if (c == 'x') c = '0';
        if (mask.find('0') == std::string::npos) return "(" + sel + " == " + vlog(val) + ")";
        return "((" + sel + " & " + vlog(mask) + ") == " + vlog(val) + ")";
    }

    // Module <name>: tck, si, so, sel, ce, se, ue, rst, one input per pin and
    // one output per observed value.
    void writeRtl(std::ostream& os, const std::string& name) const {
        os << "// IEEE 1687 scan network of " << top << ", written by mdft: " << regs.size() << " scan registers, "
           << muxes.size() << " scan muxes\nmodule " << name << " (\n  input  logic tck, si, sel, ce, se, ue, rst,\n";
        for (const FPin& p : pins) os << "  input  logic [" << p.width - 1 << ":0] " << ident(p.name) << ",\n";
        for (const FObs& o : obs) os << "  output logic [" << o.bits.size() - 1 << ":0] " << ident(o.name) << ",\n";
        os << "  output logic so);\n";
        for (size_t i = 0; i < regs.size(); i++)
            os << "  logic [" << regs[i].width - 1 << ":0] r" << i << "_sr, r" << i << "_upd;  // " << regs[i].name
               << "\n  wire r" << i << "_on;\n";
        for (size_t i = 0; i < muxes.size(); i++) os << "  wire m" << i << "_on, m" << i << "_so;  // " << muxes[i].name << "\n";
        for (size_t i = 0; i < exprs.size(); i++) {
            const FExpr& f = exprs[i];
            os << "  wire [" << f.width - 1 << ":0] e" << i << " = ";
            if (f.dmux) {
                std::string sel = vvec(f.args[0]);
                for (size_t r = 0; r < f.pats.size(); r++) os << vmatch(sel, f.pats[r]) << " ? " << vvec(f.args[r + 1]) << " : ";
                os << f.width << "'b0;\n";
                continue;
            }
            std::string a = vvec(f.args[0]), b = f.args.size() > 1 ? vvec(f.args[1]) : "";
            switch (f.op) {
            case Expr::Not: os << "~" << a; break;
            case Expr::And: os << a << " & " << b; break;
            case Expr::Or: os << a << " | " << b; break;
            case Expr::Xor: os << a << " ^ " << b; break;
            case Expr::Eq: os << a << " == " << b; break;
            default: os << a << " != " << b; break;
            }
            os << ";\n";
        }
        // Who reads each scan node: that decides when it is on the active path.
        std::vector<std::string> regOn(regs.size()), muxOn(muxes.size());
        auto consume = [&](const ScanRef& src, const std::string& when) {
            if (src.k == ScanRef::Reg) regOn[src.id] += (regOn[src.id].empty() ? "" : " | ") + when;
            else if (src.k == ScanRef::Mux) muxOn[src.id] += (muxOn[src.id].empty() ? "" : " | ") + when;
        };
        consume(so, "sel");
        for (size_t i = 0; i < regs.size(); i++) consume(regs[i].si, "r" + std::to_string(i) + "_on");
        for (size_t i = 0; i < muxes.size(); i++) {
            const FMux& m = muxes[i];
            std::string sel = vvec(m.sel), prev;
            std::ostringstream so_;
            for (size_t r = 0; r < m.rows.size(); r++) {
                std::string b = "m" + std::to_string(i) + "_b" + std::to_string(r);
                os << "  wire " << b << " = " << prev << vmatch(sel, m.rows[r].first) << ";\n";
                prev += "~" + b + " & ";
                consume(m.rows[r].second, "(m" + std::to_string(i) + "_on & " + b + ")");
                so_ << b << " ? " << vscan(m.rows[r].second) << " : ";
            }
            os << "  assign m" << i << "_so = " << so_.str() << "1'b0;\n";
        }
        for (size_t i = 0; i < muxes.size(); i++) os << "  assign m" << i << "_on = " << (muxOn[i].empty() ? "1'b0" : muxOn[i]) << ";\n";
        for (size_t i = 0; i < regs.size(); i++) {
            const FReg& r = regs[i];
            std::string n = "r" + std::to_string(i);
            os << "  assign " << n << "_on = " << (regOn[i].empty() ? "1'b0" : regOn[i]) << ";\n";
            os << "  always_ff @(posedge tck) begin\n    if (" << n << "_on && ce) " << n << "_sr <= "
               << (r.hasCapture ? vvec(r.capture) : n + "_upd") << ";\n    else if (" << n << "_on && se) " << n << "_sr <= "
               << (r.width > 1 ? "{" + vscan(r.si) + ", " + n + "_sr[" + std::to_string(r.width - 1) + ":1]}" : vscan(r.si))
               << ";\n  end\n";
            std::string rst = r.reset;
            for (char& c : rst)
                if (c == 'x') c = '0';
            os << "  always_ff @(negedge tck or posedge rst) begin\n    if (rst) " << n << "_upd <= " << vlog(rst)
               << ";\n    else if (" << n << "_on && ue) " << n << "_upd <= " << n << "_sr;\n  end\n";
        }
        for (const FObs& o : obs) os << "  assign " << ident(o.name) << " = " << vvec(o.bits) << ";\n";
        os << "  assign so = " << vscan(so) << ";\nendmodule\n";
    }

    // The network behind a TAP: instruction IJTAG selects it.
    void writeTapTop(std::ostream& os, const std::string& name, const std::string& netName, uint32_t idcode) const {
        std::string id = toMsbFirst(bitsOf(idcode | 1u, 32));
        writeTapRtl(os, name + "_tap", 4, "0001", "1111", "0001", id);
        os << "\n// " << top << ": the IJTAG network behind a TAP (instruction IJTAG = 0010)\nmodule " << name << " (\n";
        for (const FPin& p : pins) os << "  input  logic [" << p.width - 1 << ":0] " << ident(p.name) << ",\n";
        for (const FObs& o : obs) os << "  output logic [" << o.bits.size() - 1 << ":0] " << ident(o.name) << ",\n";
        os << "  input  logic tck, tms, tdi, trst_n,\n  output wire  tdo);\n"
           << "  logic [3:0] ir;\n  logic capture_dr, shift_dr, update_dr, tlr, tdo_r, tdo_en, net_so;\n"
           << "  wire net_sel = (ir == 4'b0010);\n"
           << "  " << name << "_tap u_tap (.tck(tck), .tms(tms), .tdi(tdi), .trst_n(trst_n), .tdo(tdo_r), .tdo_en(tdo_en),\n"
           << "    .ir(ir), .capture_dr(capture_dr), .shift_dr(shift_dr), .update_dr(update_dr), .tlr(tlr),\n"
           << "    .ext_sel(net_sel), .ext_so(net_so));\n"
           << "  assign tdo = tdo_en ? tdo_r : 1'bz;\n"
           << "  " << netName << " u_net (.tck(tck), .si(tdi), .sel(net_sel), .ce(capture_dr), .se(shift_dr), .ue(update_dr),\n"
           << "    .rst(tlr), .so(net_so)";
        for (const FPin& p : pins) os << ",\n    ." << ident(p.name) << "(" << ident(p.name) << ")";
        for (const FObs& o : obs) os << ",\n    ." << ident(o.name) << "(" << ident(o.name) << ")";
        os << ");\nendmodule\n";
    }
};

}  // namespace icl
}  // namespace dft
