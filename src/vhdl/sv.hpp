// vhdl/sv.hpp - translates analyzed VHDL design units into SystemVerilog
// text for msim's one engine (slang front end, lowering, kernel). Nothing is
// simulated here: an entity becomes a module, a package a package, a process
// an always block, and so on; `line directives keep diagnostics pointing at
// the VHDL source.
//
// Semantics kept on purpose:
//  - a VHDL process runs once at time 0 and then waits on its sensitivity
//    list: `always begin <body> @(<list>); end`
//  - signal assignment is a nonblocking assignment (the next delta);
//    concurrent signal assignments are processes too, so a derived clock
//    lags one delta like in VHDL
//  - '=' on std_logic is the exact enumeration compare (===)
//  - several processes driving one resolved signal: each drives its own
//    variable, resolved by a wire (std_logic resolution of 0 1 X Z)
//  - subprograms with unconstrained array or signal parameters are
//    specialized per call site (the widths are then known)
//
// std_logic's nine values map onto SystemVerilog's four: U W X - become x,
// L becomes 0 and H becomes 1 (docs/VHDL.md lists what this changes).
#pragma once

#include <algorithm>
#include <functional>
#include <set>
#include <sstream>

#include "parse.hpp"
#include "types.hpp"

namespace vhdl {

// SystemVerilog reserved words (IEEE 1800-2023 Annex B) that VHDL allows as names.
inline bool svKeyword(const std::string& w) {
    static const std::set<std::string> kw = {
        "accept_on", "alias", "always", "always_comb", "always_ff", "always_latch", "and", "assert", "assign", "assume",
        "automatic", "before", "begin", "bind", "bins", "binsof", "bit", "break", "buf", "bufif0", "bufif1", "byte", "case",
        "casex", "casez", "cell", "chandle", "checker", "class", "clocking", "cmos", "config", "const", "constraint",
        "context", "continue", "cover", "covergroup", "coverpoint", "cross", "deassign", "default", "defparam", "design",
        "disable", "dist", "do", "edge", "else", "end", "endcase", "endchecker", "endclass", "endclocking", "endconfig",
        "endfunction", "endgenerate", "endgroup", "endinterface", "endmodule", "endpackage", "endprimitive",
        "endprogram", "endproperty", "endspecify", "endsequence", "endtable", "endtask", "enum", "event", "eventually",
        "expect", "export", "extends", "extern", "final", "first_match", "for", "force", "foreach", "forever", "fork",
        "forkjoin", "function", "generate", "genvar", "global", "highz0", "highz1", "if", "iff", "ifnone", "ignore_bins",
        "illegal_bins", "implements", "implies", "import", "incdir", "include", "initial", "inout", "input", "inside",
        "instance", "int", "integer", "interconnect", "interface", "intersect", "join", "join_any", "join_none", "large",
        "let", "liblist", "library", "local", "localparam", "logic", "longint", "macromodule", "matches", "medium",
        "modport", "module", "nand", "negedge", "nettype", "new", "nexttime", "nmos", "nor", "noshowcancelled", "not",
        "notif0", "notif1", "null", "or", "output", "package", "packed", "parameter", "pmos", "posedge", "primitive",
        "priority", "program", "property", "protected", "pull0", "pull1", "pulldown", "pullup", "pulsestyle_ondetect",
        "pulsestyle_onevent", "pure", "rand", "randc", "randcase", "randsequence", "rcmos", "real", "realtime", "ref",
        "reg", "reject_on", "release", "repeat", "restrict", "return", "rnmos", "rpmos", "rtran", "rtranif0",
        "rtranif1", "s_always", "s_eventually", "s_nexttime", "s_until", "s_until_with", "scalared", "sequence",
        "shortint", "shortreal", "showcancelled", "signed", "small", "soft", "solve", "specify", "specparam", "static",
        "string", "strong", "strong0", "strong1", "struct", "super", "supply0", "supply1", "sync_accept_on",
        "sync_reject_on", "table", "tagged", "task", "this", "throughout", "time", "timeprecision", "timeunit", "tran",
        "tranif0", "tranif1", "tri", "tri0", "tri1", "triand", "trior", "trireg", "type", "typedef", "union", "unique",
        "unique0", "unsigned", "until", "until_with", "untyped", "use", "uwire", "var", "vectored", "virtual", "void",
        "wait", "wait_order", "wand", "weak", "weak0", "weak1", "while", "wildcard", "wire", "with", "within", "wor",
        "xnor", "xor"};
    return kw.count(w) > 0;
}

inline std::string svId(const std::string& text) {
    std::string t = text;
    bool plain = !t.empty() && (std::isalpha((unsigned char)t[0]) || t[0] == '_');
    for (char c : t)
        if (!(std::isalnum((unsigned char)c) || c == '_')) plain = false;
    if (plain && !svKeyword(t)) return t;
    std::string r = "\\";
    for (char c : t) r.push_back(c == ' ' ? '_' : c);
    return r + " ";
}

struct Val {
    std::string s;
    TP t;
    bool isConst = false;
    std::optional<int64_t> iv;   // folded integer value
};

class Translator {
public:
    // What the SystemVerilog side of a mixed design declares, so VHDL can
    // instantiate SystemVerilog modules by their case-sensitive names.
    struct SvModule {
        std::string name;
        std::vector<std::string> ports, params;
    };
    std::map<std::string, SvModule> svModules;   // lower-case name -> module
    std::string timeUnit = "1ns", timePrec = "1fs";
    std::vector<std::string> warnings;

    void add(const std::vector<UP>& units) {
        for (auto& u : units) {
            switch (u->k) {
            case Unit::Entity: entities[u->name] = u; order.push_back(u); break;
            case Unit::Arch: archs[u->of].push_back(u); break;
            case Unit::Package: packages[u->name] = u; order.push_back(u); break;
            case Unit::PackageBody: bodies[u->name] = u; break;
            case Unit::Config: configs[u->name] = u; break;
            case Unit::Context: contexts[u->name] = u; break;
            case Unit::PackageInst: order.push_back(u); packages[u->name] = u; break;
            }
        }
    }

    // Names of the design entities (as written), for -top and for SV to instantiate.
    std::vector<std::string> entityNames() const {
        std::vector<std::string> r;
        for (auto& u : order)
            if (u->k == Unit::Entity) r.push_back(u->text);
        return r;
    }
    std::string entityText(const std::string& lowName) const {
        auto it = entities.find(lowName);
        return it == entities.end() ? "" : it->second->text;
    }

    std::string run() {
        builtins();
        std::ostringstream o;
        o << rtPackage();
        // packages in dependency order
        std::set<std::string> done;
        std::function<void(const UP&)> emitPkg = [&](const UP& u) {
            if (done.count(u->name)) return;
            done.insert(u->name);
            for (auto& d : allCtx(*u))
                for (auto& n : d->uses) {
                    std::string pn = usePackage(n);
                    auto it = packages.find(pn);
                    if (it != packages.end() && it->second != u) emitPkg(it->second);
                }
            if (auto b = bodies.find(u->name); b != bodies.end())
                for (auto& d : allCtx(*b->second))
                    for (auto& n : d->uses) {
                        std::string pn = usePackage(n);
                        auto it = packages.find(pn);
                        if (it != packages.end() && it->second != u) emitPkg(it->second);
                    }
            o << emitPackage(u);
        };
        for (auto& u : order)
            if (u->k == Unit::Package || u->k == Unit::PackageInst) emitPkg(u);
        for (auto& u : order)
            if (u->k == Unit::Entity) o << emitEntity(u);
        return o.str();
    }

private:
    std::map<std::string, UP> entities, packages, bodies, configs, contexts;
    std::map<std::string, std::vector<UP>> archs;
    std::vector<UP> order;
    std::map<std::string, Scope> pkgScope;   // package -> its declarations
    Scope builtinScope;
    std::map<std::string, TP> bt;            // built-in types by name

    // ------------------------------------------------------------ contexts
    struct Proc {
        std::set<std::string> prev;            // signals whose previous value the process keeps ('event)
        std::map<std::string, std::string> drv;   // signal SV name -> driver variable
        bool active = false;
        std::string label;
        std::map<std::string, TP> drvTypes;
    };
    struct Loop {
        std::string label;   // lower
        std::string exitBlk, nextBlk;
        bool exitUsed = false, nextUsed = false;
    };
    struct Ctx {
        std::string container;          // package or module name being emitted
        bool isPackage = false;
        std::vector<Scope> scopes;
        std::vector<std::string> usePkgs;   // packages made visible (.all)
        std::map<std::string, std::pair<std::string, std::string>> useItems;   // name -> (package, name)
        std::ostringstream* extra = nullptr;  // specializations / hoisted items of the container
        std::set<std::string>* specDone = nullptr;
        Proc* proc = nullptr;
        std::vector<Loop>* loops = nullptr;
        bool inFunc = false;
        TP retType;      // function return type (null: procedure)
        TP retSeen;      // type of the first return expression (specialization)
        std::string file;
        int indent = 1;
        std::map<std::string, std::string> drvNames;   // module: signals with driver variables
    };
    Ctx* cx = nullptr;
    int uniq = 0;

    static std::vector<DP> allCtx(const Unit& u) {
        std::vector<DP> r = u.ctx;
        for (auto& d : u.decls)
            if (d->k == DK::Use) r.push_back(d);
        return r;
    }
    static std::string usePackage(const EP& n) {
        // work.p.all / lib.p.x / p.all -> p
        std::vector<std::string> parts;
        for (EP e = n; e; e = e->a) {
            if (e->k == EK::Sel || e->k == EK::Name) parts.insert(parts.begin(), e->name);
            if (e->k == EK::Name) break;
        }
        if (parts.size() >= 3) return parts[1];
        if (parts.size() == 2) return parts[0] == "work" ? parts[1] : parts[0] == "ieee" || parts[0] == "std" ? parts[1] : parts[0];
        return parts.empty() ? "" : parts[0];
    }

    [[noreturn]] void fail(int line, const std::string& m) {
        throw Error((cx ? cx->file : std::string("?")) + ":" + std::to_string(line) + ": " + m);
    }
    void warn(int line, const std::string& m) { warnings.push_back((cx ? cx->file : std::string("?")) + ":" + std::to_string(line) + ": " + m); }

    std::string ind() const { return std::string(cx ? cx->indent * 2 : 2, ' '); }
    std::string lineDir(int line) const { return "`line " + std::to_string(line) + " \"" + (cx ? cx->file : std::string()) + "\" 0\n"; }

    // ----------------------------------------------------------- built-ins
    TP mkType(Type::K k, const std::string& name, const std::string& home = "*") {
        auto t = std::make_shared<Type>();
        t->k = k;
        t->name = name;
        t->text = name;
        t->home = home;
        bt[name] = t;
        return t;
    }
    TP mkArray(const std::string& name, TP elem, TP idx, const std::string& flavor) {
        TP t = mkType(Type::Array, name);
        Rng r;
        r.box = true;
        r.idx = idx;
        t->dims.push_back(r);
        t->elem = elem;
        t->flavor = flavor;
        return t;
    }
    void addSym(Scope& s, Sym y) { s[y.name].push_back(y); }
    void addType(Scope& s, TP t) {
        Sym y;
        y.k = Sym::TypeK;
        y.name = t->name;
        y.text = t->text;
        y.type = t;
        y.home = "*";
        addSym(s, y);
    }
    void addBuiltinFn(const std::string& name, const std::string& id = "") {
        Sym y;
        y.k = Sym::Sub;
        y.name = name;
        y.text = name;
        y.builtin = id.empty() ? name : id;
        y.home = "*";
        addSym(builtinScope, y);
    }

    void builtins() {
        if (!bt.empty()) return;
        Scope& s = builtinScope;
        TP logic = mkType(Type::Logic, "std_ulogic");
        logic->lits = {"'u'", "'x'", "'0'", "'1'", "'z'", "'w'", "'l'", "'h'", "'-'"};
        TP sl = std::make_shared<Type>(*logic);
        sl->name = sl->text = "std_logic";
        sl->base = logic;
        sl->resolved = true;
        bt["std_logic"] = sl;
        for (auto n : {"x01", "x01z", "ux01", "ux01z"}) { TP x = std::make_shared<Type>(*logic); x->name = x->text = n; x->base = logic; bt[n] = x; addType(s, x); }
        TP bit = mkType(Type::Bit, "bit");
        TP boolean = mkType(Type::Bool, "boolean");
        TP chr = mkType(Type::Char, "character");
        TP integer = mkType(Type::Int, "integer");
        TP natural = mkType(Type::Int, "natural");
        natural->base = integer;
        natural->range = Rng{"0", "2147483647", false, 0, 2147483647};
        TP positive = mkType(Type::Int, "positive");
        positive->base = integer;
        positive->range = Rng{"1", "2147483647", false, 1, 2147483647};
        TP real = mkType(Type::Real, "real");
        TP time = mkType(Type::Time, "time");
        TP dl = mkType(Type::Time, "delay_length");
        dl->base = time;
        TP str = mkType(Type::String, "string");
        str->elem = chr;
        TP sev = mkType(Type::Enum, "severity_level");
        sev->lits = {"note", "warning", "error", "failure"};
        sev->litSv = {"0", "1", "2", "3"};
        sev->sv = "int";
        TP fok = mkType(Type::Enum, "file_open_kind");
        fok->lits = {"read_mode", "write_mode", "append_mode"};
        fok->litSv = {"0", "1", "2"};
        fok->sv = "int";
        TP fos = mkType(Type::Enum, "file_open_status");
        fos->lits = {"open_ok", "status_error", "name_error", "mode_error"};
        fos->litSv = {"0", "1", "2", "3"};
        fos->sv = "int";
        TP side = mkType(Type::Enum, "side");
        side->lits = {"right", "left"};
        side->litSv = {"0", "1"};
        side->sv = "int";
        TP width = mkType(Type::Int, "width");
        width->base = integer;
        TP line = mkType(Type::Line, "line");
        line->target = str;
        TP text = mkType(Type::File, "text");
        text->target = str;
        TP bv = mkArray("bit_vector", bit, natural, "bv");
        TP bov = mkArray("boolean_vector", boolean, natural, "");
        TP iv = mkArray("integer_vector", integer, natural, "");
        TP rv = mkArray("real_vector", real, natural, "");
        TP tv = mkArray("time_vector", time, natural, "");
        TP sulv = mkArray("std_ulogic_vector", logic, natural, "slv");
        TP slv = mkArray("std_logic_vector", sl, natural, "slv");
        slv->base = sulv;
        TP uns = mkArray("unsigned", sl, natural, "unsigned");
        TP sgn = mkArray("signed", sl, natural, "signed");
        TP uuns = mkArray("unresolved_unsigned", logic, natural, "unsigned");
        TP usgn = mkArray("unresolved_signed", logic, natural, "signed");
        TP u_u = mkArray("u_unsigned", logic, natural, "unsigned");
        TP u_s = mkArray("u_signed", logic, natural, "signed");
        uuns->base = uns;
        usgn->base = sgn;
        u_u->base = uns;
        u_s->base = sgn;
        TP ul = std::make_shared<Type>(*logic);
        ul->name = ul->text = "std_ulogic";
        for (auto& [n, t] : bt) {
            Sym y;
            y.k = Sym::TypeK;
            y.name = n;
            y.text = n;
            y.type = t;
            y.home = "*";
            if (!s.count(n)) addSym(s, y);
        }
        auto lit = [&](const std::string& n, TP t, const std::string& sv) {
            Sym y;
            y.k = Sym::Lit;
            y.name = n;
            y.text = n;
            y.type = t;
            y.sv = sv;
            y.home = "*";
            addSym(s, y);
        };
        lit("true", boolean, "1'b1");
        lit("false", boolean, "1'b0");
        for (size_t i = 0; i < sev->lits.size(); ++i) lit(sev->lits[i], sev, sev->litSv[i]);
        for (size_t i = 0; i < fok->lits.size(); ++i) lit(fok->lits[i], fok, fok->litSv[i]);
        for (size_t i = 0; i < fos->lits.size(); ++i) lit(fos->lits[i], fos, fos->litSv[i]);
        lit("right", side, "0");
        lit("left", side, "1");
        // physical units of time
        auto unit = [&](const std::string& n, int64_t fs) {
            Sym y;
            y.k = Sym::Unit;
            y.name = n;
            y.unitFs = fs;
            y.type = time;
            y.home = "*";
            addSym(s, y);
        };
        unit("fs", 1);
        unit("ps", 1000);
        unit("ns", 1000000);
        unit("us", 1000000000LL);
        unit("ms", 1000000000000LL);
        unit("sec", 1000000000000000LL);
        unit("min", 60000000000000000LL);
        unit("hr", 3600000000000000000LL);
        // objects of std.textio and math_real
        auto obj = [&](const std::string& n, TP t, const std::string& sv, const std::string& cls = "constant") {
            Sym y;
            y.k = Sym::Obj;
            y.cls = cls;
            y.name = n;
            y.text = n;
            y.type = t;
            y.sv = sv;
            y.home = "*";
            addSym(s, y);
        };
        obj("output", text, "32'h8000_0001", "file");
        obj("input", text, "32'h8000_0000", "file");
        obj("math_pi", real, "3.14159265358979323846");
        obj("math_2_pi", real, "6.28318530717958647693");
        obj("math_pi_over_2", real, "1.57079632679489661923");
        obj("math_e", real, "2.71828182845904523536");
        obj("math_log2_of_e", real, "1.44269504088896340736");
        obj("math_log_of_2", real, "0.69314718055994530942");
        obj("math_sqrt_2", real, "1.41421356237309504880");
        obj("math_1_over_pi", real, "0.31830988618379067154");
        for (auto n : {"rising_edge", "falling_edge", "to_bit", "to_bitvector", "to_bv", "to_stdulogic", "to_stdlogicvector", "to_slv",
                       "to_stdulogicvector", "to_sulv", "to_x01", "to_x01z", "to_ux01", "is_x", "to_01", "to_integer", "to_unsigned",
                       "to_signed", "resize", "shift_left", "shift_right", "rotate_left", "rotate_right", "std_match",
                       "conv_integer", "conv_unsigned", "conv_signed", "conv_std_logic_vector", "ext", "sxt", "shl", "shr",
                       "to_string", "to_hstring", "to_ostring", "to_bstring", "to_hex_string", "to_octal_string",
                       "minimum", "maximum", "floor", "ceil", "round", "trunc", "sqrt", "log2", "log10", "log", "exp", "sin", "cos", "tan",
                       "arctan", "sign", "realmax", "realmin", "uniform", "now", "write", "writeline", "read", "readline", "hwrite", "hread", "owrite",
                       "oread", "bwrite", "bread", "swrite", "sread", "endfile", "file_open", "file_close", "flush", "deallocate", "stop", "finish",
                       "resolution_limit", "find_leftmost", "find_rightmost", "and_reduce", "or_reduce", "xor_reduce", "nand_reduce",
                       "nor_reduce", "xnor_reduce", "to_real", "justify", "abs", "std_logic_vector", "tee", "fwrite", "unsigned_num_bits", "signed_num_bits"})
            addBuiltinFn(n);
    }

    // ------------------------------------------------------------- lookups
    const std::vector<Sym>* lookupAll(const std::string& n) {
        for (auto it = cx->scopes.rbegin(); it != cx->scopes.rend(); ++it) {
            auto f = it->find(n);
            if (f != it->end() && !f->second.empty()) return &f->second;
        }
        if (auto ui = cx->useItems.find(n); ui != cx->useItems.end()) {
            auto& ps = pkgScope[ui->second.first];
            auto f = ps.find(ui->second.second);
            if (f != ps.end()) return &f->second;
        }
        static std::vector<Sym> merged;
        const std::vector<Sym>* found = nullptr;
        int count = 0;
        for (auto& p : cx->usePkgs) {
            auto& ps = pkgScope[p];
            auto f = ps.find(n);
            if (f != ps.end() && !f->second.empty()) {
                if (!found) found = &f->second;
                ++count;
            }
        }
        if (count > 1) {   // overloads from several packages
            static std::vector<std::vector<Sym>> keep;
            keep.emplace_back();
            for (auto& p : cx->usePkgs) {
                auto& ps = pkgScope[p];
                auto f = ps.find(n);
                if (f != ps.end()) keep.back().insert(keep.back().end(), f->second.begin(), f->second.end());
            }
            return &keep.back();
        }
        if (found) return found;
        auto f = builtinScope.find(n);
        if (f != builtinScope.end()) return &f->second;
        return nullptr;
    }
    const Sym* lookup(const std::string& n) {
        auto v = lookupAll(n);
        return v && !v->empty() ? &v->front() : nullptr;
    }
    const Sym* lookupIn(const std::string& pkg, const std::string& n) {
        auto it = pkgScope.find(pkg);
        if (it != pkgScope.end()) {
            auto f = it->second.find(n);
            if (f != it->second.end() && !f->second.empty()) return &f->second.front();
        }
        auto f = builtinScope.find(n);
        if (f != builtinScope.end() && !f->second.empty()) return &f->second.front();
        return nullptr;
    }
    bool isBuiltinPkg(const std::string& p) const {
        static const std::set<std::string> b = {"std_logic_1164", "numeric_std", "numeric_bit", "textio", "standard", "math_real",
                                                "env", "std_logic_arith", "std_logic_unsigned", "std_logic_signed",
                                                "numeric_std_unsigned", "numeric_std_signed", "std_logic_textio", "math_complex",
                                                "numeric_bit_unsigned", "std_logic_misc", "vital_timing", "vital_primitives",
                                                "fixed_pkg", "float_pkg", "fixed_generic_pkg", "float_generic_pkg", "fixed_float_types"};
        return b.count(p) > 0;
    }

    // Qualified SystemVerilog name of a symbol, from the current container.
    std::string qual(const Sym& y) {
        if (!y.home.empty() && y.home != "*" && y.home != cx->container) return svId(y.home) + "::" + (y.sv.empty() ? svId(y.text) : y.sv);
        return y.sv.empty() ? svId(y.text) : y.sv;
    }
    std::string typeName(const TP& t) {
        if (t->home.empty() || t->home == "*" || t->home == cx->container) return t->sv;
        return svId(t->home) + "::" + t->sv;
    }

    void declare(const Sym& y) { cx->scopes.back()[y.name].push_back(y); }

    // ------------------------------------------------------- folding
    std::optional<int64_t> fold(const EP& e) {
        if (!e) return std::nullopt;
        switch (e->k) {
        case EK::Int: return e->ival;
        case EK::Un: {
            auto a = fold(e->a);
            if (!a) return std::nullopt;
            if (e->op == "-") return -*a;
            if (e->op == "+" || e->op == "()") return *a;
            if (e->op == "abs") return *a < 0 ? -*a : *a;
            return std::nullopt;
        }
        case EK::Bin: {
            auto a = fold(e->a), b = fold(e->b);
            if (!a || !b) return std::nullopt;
            if (e->op == "+") return *a + *b;
            if (e->op == "-") return *a - *b;
            if (e->op == "*") return *a * *b;
            if (e->op == "/" && *b) return *a / *b;
            if (e->op == "rem" && *b) return *a % *b;
            if (e->op == "mod" && *b) { int64_t r = *a % *b; if (r && ((r < 0) != (*b < 0))) r += *b; return r; }
            if (e->op == "**" && *b >= 0 && *b < 63) { int64_t r = 1; for (int64_t i = 0; i < *b; ++i) r *= *a; return r; }
            return std::nullopt;
        }
        case EK::Name: {
            auto y = lookup(e->name);
            if (y && y->k == Sym::Obj && y->cls == "constant" && y->decl && y->decl->init && y->decl.get() != foldGuard) {
                auto* g = foldGuard;
                foldGuard = y->decl.get();
                auto r = fold(y->decl->init);
                foldGuard = g;
                return r;
            }
            return std::nullopt;
        }
        default: return std::nullopt;
        }
    }
    Decl* foldGuard = nullptr;

    static std::optional<int64_t> parseInt(const std::string& s) {
        if (s.empty()) return std::nullopt;
        size_t i = 0;
        bool neg = false;
        std::string t = s;
        while (t.size() > 2 && t.front() == '(' && t.back() == ')') t = t.substr(1, t.size() - 2);
        if (t[0] == '-') { neg = true; i = 1; }
        if (i >= t.size()) return std::nullopt;
        int64_t v = 0;
        for (; i < t.size(); ++i) {
            if (!std::isdigit((unsigned char)t[i])) return std::nullopt;
            v = v * 10 + (t[i] - '0');
        }
        return neg ? -v : v;
    }
    static std::string num(int64_t v) { return v < 0 ? "(" + std::to_string(v) + ")" : std::to_string(v); }

    // length of a range as SV text
    static std::string lenOf(const Rng& r) {
        if (r.lv && r.rv) {
            int64_t n = r.downto ? *r.lv - *r.rv + 1 : *r.rv - *r.lv + 1;
            return std::to_string(n < 0 ? 0 : n);
        }
        if (r.downto) return "((" + r.l + ")-(" + r.r + ")+1)";
        return "((" + r.r + ")-(" + r.l + ")+1)";
    }
    static std::string widthOf(const TP& t) {
        if (!t) return "1";
        if (t->k == Type::Array && !t->dims.empty()) {
            std::string w = lenOf(t->dims[0]);
            for (size_t i = 1; i < t->dims.size(); ++i) w = mulText(w, lenOf(t->dims[i]));
            if (!isScalarBit(t->elem)) w = mulText(w, widthOf(t->elem));
            return w;
        }
        if (t->k == Type::Int || t->k == Type::Enum) return "32";
        if (t->k == Type::Char) return "8";
        if (t->k == Type::Record) {
            std::string w = "0";
            for (auto& f : t->ftypes) w = addText(w, widthOf(f));
            return w;
        }
        return "1";
    }
    static std::string addText(const std::string& a, const std::string& b) {
        auto x = parseInt(a), y = parseInt(b);
        if (x && y) return std::to_string(*x + *y);
        if (x && *x == 0) return b;
        if (y && *y == 0) return a;
        return "(" + a + "+" + b + ")";
    }
    static std::string subText(const std::string& a, const std::string& b) {
        auto x = parseInt(a), y = parseInt(b);
        if (x && y) return num(*x - *y);
        if (y && *y == 0) return a;
        return "(" + a + "-" + b + ")";
    }
    static std::string mulText(const std::string& a, const std::string& b) {
        auto x = parseInt(a), y = parseInt(b);
        if (x && y) return std::to_string(*x * *y);
        if (x && *x == 1) return b;
        if (y && *y == 1) return a;
        return "(" + a + "*" + b + ")";
    }
    static std::string maxText(const std::string& a, const std::string& b) {
        auto x = parseInt(a), y = parseInt(b);
        if (x && y) return std::to_string(std::max(*x, *y));
        if (a == b) return a;
        return "((" + a + ")>(" + b + ")?(" + a + "):(" + b + "))";
    }
    static Rng mkRng(const std::string& l, const std::string& r, bool downto) {
        Rng g;
        g.l = l;
        g.r = r;
        g.downto = downto;
        g.lv = parseInt(l);
        g.rv = parseInt(r);
        return g;
    }
    // A one-dimensional array type of `base` with `width` elements, (w-1 downto 0) or (0 to w-1).
    TP vecOf(const TP& base, const std::string& width, bool downto = true, const std::string& left = "") {
        TP t = std::make_shared<Type>(*baseOf(base));
        t->base = baseOf(base);
        std::string w1 = subText(width, "1");
        if (!left.empty()) {
            // keep the left bound, direction from `downto`
            t->dims = {downto ? mkRng(left, subText(addText(left, "1"), width), true) : mkRng(left, subText(addText(left, width), "1"), false)};
        } else if (downto) t->dims = {mkRng(w1, "0", true)};
        else t->dims = {mkRng("0", w1, false)};
        if (!t->dims[0].idx) t->dims[0].idx = bt["natural"];
        return t;
    }

    // --------------------------------------------------------- type mapping
    std::string svBase(const TP& t0) {
        TP t = t0;
        if (!t) return "logic";
        switch (t->k) {
        case Type::Logic: return "logic";
        case Type::Bit: case Type::Bool: return "bit";
        case Type::Char: return "byte unsigned";
        case Type::Int: return "int";
        case Type::Real: return "real";
        case Type::Time: return "realtime";
        case Type::Enum: return t->sv == "int" ? "int" : typeName(t);
        case Type::Record: return typeName(t);
        case Type::String: case Type::Line: return "string";
        case Type::File: return "int";
        case Type::Access: return t->target && t->target->k == Type::String ? "string" : "int";
        default: return "logic";
        }
    }
    // Declaration of `name` with type t: "logic [7:0] name" or "logic [7:0] name [0:3]".
    std::string svDecl(const TP& t, const std::string& name) {
        std::string pre, post;
        svParts(t, pre, post);
        return pre + " " + name + post;
    }
    void svParts(const TP& t, std::string& pre, std::string& post) {
        if (t && t->k == Type::Array) {
            if (t->elem && t->elem->k == Type::Char && t->dims.size() == 1) { pre = "string"; return; }
            std::string dims;
            for (auto& d : t->dims) {
                if (d.box) dims += "[]";
                else dims += "[" + d.l + ":" + d.r + "]";
            }
            if (isScalarBit(t->elem)) {
                pre = svBase(t->elem) + (t->flavor == "signed" ? " signed " : " ") + dims;
                return;
            }
            std::string ep, epost;
            svParts(t->elem, ep, epost);
            pre = ep;
            post = dims + epost;
            return;
        }
        pre = svBase(t);
        if (t && t->k == Type::Int && t->range && t->range->lv && *t->range->lv >= 0 && false) pre = "int";
    }
    std::string svTypeExpr(const TP& t) {   // a type usable in casts / typedefs (packed only)
        std::string pre, post;
        svParts(t, pre, post);
        return pre;
    }
    // T'left of a scalar type as SV text, or "" when SystemVerilog's default is already right.
    std::string defaultInit(const TP& t) {
        if (!t) return "";
        switch (t->k) {
        case Type::Int:
            if (t->range && t->range->lv) return *t->range->lv == 0 ? "" : num(*t->range->lv);
            if (t->range) return "";
            return "32'sh8000_0000";
        case Type::Real: return t->range ? "" : "-1.7976931348623157e308";
        case Type::Time: return "";
        case Type::Enum:
            if (t->range && !t->range->l.empty()) return t->range->l;
            return "";
        default: return "";
        }
    }

    // ------------------------------------------------------- subtype ind.
    TP typeOfMark(const EP& m) {
        if (!m) return bt["integer"];
        if (m->k == EK::Attr) {
            if (m->name == "subtype" || m->name == "base") {
                Val v = ex(m->a, nullptr);
                if (m->name == "base") return baseOf(v.t);
                return v.t;
            }
            if (m->name == "element") {
                Val v = ex(m->a, nullptr);
                return v.t && v.t->elem ? v.t->elem : v.t;
            }
        }
        const Sym* y = nullptr;
        if (m->k == EK::Name) y = lookup(m->name);
        else if (m->k == EK::Sel) y = lookupSel(m);
        if (!y) fail(m->line, "unknown type '" + m->text + "'");
        if (y->k == Sym::TypeK) return y->type;
        if (y->k == Sym::Obj) return y->type;   // e.g. a'subtype written as a name
        fail(m->line, "'" + m->text + "' is not a type");
    }
    const Sym* lookupSel(const EP& m) {
        // lib.pkg.name or pkg.name
        if (m->k != EK::Sel) return nullptr;
        EP pre = m->a;
        std::string pkg;
        if (pre->k == EK::Name) pkg = pre->name;
        else if (pre->k == EK::Sel) pkg = pre->name;
        if (packages.count(pkg) || pkgScope.count(pkg) || isBuiltinPkg(pkg)) return lookupIn(pkg, m->name);
        return nullptr;
    }

    Rng rangeOf(const EP& r, TP* idxType = nullptr) {
        if (r->k == EK::Range) {
            Val a = ex(r->a, nullptr), b = ex(r->b, a.t && a.t->k != Type::Int ? a.t : nullptr);
            if (a.t && a.t->k == Type::Int && b.t && b.t->k != Type::Int) a = ex(r->a, b.t);
            Rng g = mkRng(a.s, b.s, r->downto);
            if (a.iv) g.lv = a.iv;
            if (b.iv) g.rv = b.iv;
            g.idx = a.t && a.t->k == Type::Enum ? a.t : bt["integer"];
            if (idxType) *idxType = g.idx;
            return g;
        }
        if (r->k == EK::Attr && (r->name == "range" || r->name == "reverse_range")) {
            int dim = r->b ? (int)fold(r->b).value_or(1) - 1 : 0;
            TP t = typeOrObjType(r->a);
            Rng g;
            if (t && t->k == Type::Array && dim < (int)t->dims.size() && !t->dims[dim].box) g = t->dims[dim];
            else if (t && t->k == Type::Array) {
                // unconstrained object (e.g. a string): use SystemVerilog queries
                Val v = ex(r->a, nullptr);
                g = mkRng("$left(" + v.s + ")", "$right(" + v.s + ")", true);
                if (t->elem && t->elem->k == Type::Char) g = mkRng("1", v.s + ".len()", false);
            } else if (t && t->range) g = *t->range;
            else if (t && t->k == Type::Enum) g = mkRng(t->litSv.front(), t->litSv.back(), false);
            else if (t && t->k == Type::Int) g = mkRng("-2147483648", "2147483647", false);
            if (r->name == "reverse_range") { std::swap(g.l, g.r); std::swap(g.lv, g.rv); g.downto = !g.downto; }
            if (idxType) *idxType = g.idx ? g.idx : bt["integer"];
            return g;
        }
        if (r->k == EK::SubtypeRange) {
            TP t = typeOfMark(r->a);
            if (r->b && r->b->k != EK::Box) return rangeOf(r->b, idxType);
            if (t->range) { if (idxType) *idxType = t; return *t->range; }
        }
        // a type name: its range
        if (r->k == EK::Name || r->k == EK::Sel) {
            const Sym* y = r->k == EK::Name ? lookup(r->name) : lookupSel(r);
            if (y && y->k == Sym::TypeK) {
                TP t = y->type;
                if (idxType) *idxType = t;
                if (t->range) return *t->range;
                if (t->k == Type::Enum) { Rng g = mkRng(std::to_string(0), std::to_string(t->lits.size() - 1), false); g.idx = t; g.l = t->litSv.front(); g.r = t->litSv.back(); return g; }
                if (t->k == Type::Int) return mkRng("-2147483648", "2147483647", false);
                if (t->k == Type::Bool) { Rng g = mkRng("0", "1", false); g.idx = t; return g; }
                if (t->k == Type::Logic) { Rng g = mkRng("0", "8", false); g.idx = t; return g; }
                if (t->k == Type::Char) { Rng g = mkRng("0", "255", false); g.idx = t; return g; }
            }
        }
        fail(r->line, "expected a range");
    }
    TP typeOrObjType(const EP& e) {
        if (e->k == EK::Name || e->k == EK::Sel) {
            const Sym* y = e->k == EK::Name ? lookup(e->name) : lookupSel(e);
            if (y && y->k == Sym::TypeK) return y->type;
        }
        return ex(e, nullptr).t;
    }

    TP subtype(const SubInd& si, const EP& initForWidth = nullptr) {
        TP t = typeOfMark(si.mark);
        if (si.range) {
            TP n = std::make_shared<Type>(*t);
            n->base = t->base ? t->base : t;
            n->name = n->text = "";
            n->range = rangeOf(si.range);
            return n;
        }
        if (!si.idx.empty() && t->k == Type::Array) {
            TP n = std::make_shared<Type>(*t);
            n->base = baseOf(t);
            n->name = n->text = "";
            n->dims.clear();
            for (size_t i = 0; i < si.idx.size(); ++i) {
                if (si.idx[i]->k == EK::Open) { n->dims.push_back(t->dims[i]); continue; }
                TP it;
                Rng g = rangeOf(si.idx[i], &it);
                if (!g.idx) g.idx = it ? it : (i < t->dims.size() ? t->dims[i].idx : bt["integer"]);
                n->dims.push_back(g);
            }
            if (!si.elemIdx.empty() && n->elem && n->elem->k == Type::Array) {
                SubInd e;
                e.mark = nullptr;
                TP el = std::make_shared<Type>(*n->elem);
                el->dims.clear();
                for (auto& r : si.elemIdx[0]) el->dims.push_back(rangeOf(r));
                n->elem = el;
            }
            return n;
        }
        if (!si.idx.empty() && t->k == Type::String) {
            return t;
        }
        return t;
    }

    // ===================================================================
    // Expressions
    // ===================================================================
    static bool ambiguous(const EP& e) {
        if (!e) return true;
        switch (e->k) {
        case EK::Char: case EK::Str: case EK::Agg: case EK::Others: case EK::Null: return true;
        case EK::Un: return e->op == "()" ? ambiguous(e->a) : false;
        default: return false;
        }
    }

    std::string logicChar(char c) {
        switch (std::tolower((unsigned char)c)) {
        case '0': case 'l': return "0";
        case '1': case 'h': return "1";
        case 'z': return "z";
        default: return "x";
        }
    }
    std::string bitsLit(const std::string& bits, bool fourState = true) {
        if (bits.empty()) return "0";
        std::string r = std::to_string(bits.size()) + "'b";
        for (char c : bits) r += fourState ? logicChar(c) : (c == '1' ? std::string("1") : std::string("0"));
        return r;
    }

    Val charLit(const EP& e, const TP& want) {
        Val v;
        char c = e->text[0];
        TP w = want ? baseOf(want) : nullptr;
        if (w && w->k == Type::Char) {
            v.s = "8'd" + std::to_string((unsigned char)c);
            v.t = bt["character"];
        } else if (w && w->k == Type::Bit) {
            v.s = c == '1' ? "1'b1" : "1'b0";
            v.t = bt["bit"];
        } else if (w && w->k == Type::Enum) {
            std::string key = std::string("'") + c + "'";
            for (size_t i = 0; i < w->lits.size(); ++i)
                if (w->lits[i] == key || w->lits[i] == lowerStr(key)) { v.s = enumLitSv(w, i); v.t = want; return v; }
            v.s = "8'd" + std::to_string((unsigned char)c);
            v.t = bt["character"];
        } else {
            v.s = "1'b" + logicChar(c);
            v.t = bt["std_ulogic"];
        }
        v.isConst = true;
        return v;
    }
    std::string enumLitSv(const TP& t, size_t i) {
        if (t->sv == "int") return t->litSv[i];
        if (t->home.empty() || t->home == "*" || t->home == cx->container) return t->litSv[i];
        return svId(t->home) + "::" + t->litSv[i];
    }

    Val strLit(const std::string& text, const TP& want, int line) {
        Val v;
        v.isConst = true;
        TP w = want ? baseOf(want) : nullptr;
        if (w && w->k == Type::Array && isScalarBit(w->elem)) {
            bool four = w->elem->k == Type::Logic;
            v.s = bitsLit(text, four);
            v.t = vecOf(want, std::to_string(text.size()), wantDownto(want), wantLeft(want, text.size()));
            if (want && want->k == Type::Array) v.t->flavor = want->flavor;
            return v;
        }
        if (w && w->k == Type::Array && w->elem && w->elem->k == Type::Enum) {
            // array of a character enumeration
            std::string r = "'{";
            for (size_t i = 0; i < text.size(); ++i) {
                EP c = std::make_shared<Expr>(EK::Char, line);
                c->text = std::string(1, text[i]);
                r += (i ? ", " : "") + charLit(c, w->elem).s;
            }
            v.s = r + "}";
            v.t = want;
            return v;
        }
        std::string s = "\"";
        for (char c : text) {
            if (c == '\\' || c == '"') s.push_back('\\');
            s.push_back(c);
        }
        v.s = s + "\"";
        v.t = bt["string"];
        return v;
    }
    static bool wantDownto(const TP& w) { return !(w && w->k == Type::Array && !w->dims.empty() && !w->dims[0].box && !w->dims[0].downto); }
    static std::string wantLeft(const TP& w, size_t n) {
        (void)n;
        if (w && w->k == Type::Array && !w->dims.empty() && !w->dims[0].box && !w->dims[0].downto) return w->dims[0].l;
        return "";
    }

    // The main entry: SV text and type of e; `want` is the expected type.
    Val ex(const EP& e, const TP& want) {
        Val v = ex0(e, want);
        if (!v.iv) v.iv = parseInt(v.s);
        return v;
    }

    Val ex0(const EP& e, const TP& want) {
        Val v;
        switch (e->k) {
        case EK::Int:
            if (want && baseOf(want)->k == Type::Real) { v.s = std::to_string(e->ival) + ".0"; v.t = bt["real"]; return v; }
            v.s = e->ival < 0 ? "(" + std::to_string(e->ival) + ")" : std::to_string(e->ival);
            if (e->ival > 2147483647LL) v.s = "64'sd" + std::to_string(e->ival);
            v.t = bt["integer"];
            v.iv = e->ival;
            v.isConst = true;
            return v;
        case EK::Real: {
            std::ostringstream o;
            o.precision(17);
            o << e->rval;
            std::string s = o.str();
            if (s.find_first_of(".en") == std::string::npos) s += ".0";
            v.s = s;
            v.t = bt["real"];
            v.isConst = true;
            return v;
        }
        case EK::Phys: return physLit(e->k == EK::Phys ? (e->rval) : 0, e->name, e->line);
        case EK::Char: return charLit(e, want);
        case EK::Str: return strLit(e->text, want, e->line);
        case EK::BitStr: {
            TP w = want ? baseOf(want) : nullptr;
            if (w && w->k == Type::String) return strLit(e->text, want, e->line);
            bool four = !(w && w->k == Type::Array && w->elem && w->elem->k == Type::Bit);
            v.s = bitsLit(e->text, four);
            v.t = vecOf(want && want->k == Type::Array ? want : bt["std_logic_vector"], std::to_string(e->text.size()), wantDownto(want), wantLeft(want, e->text.size()));
            if (want && want->k == Type::Array) v.t->flavor = want->flavor;
            v.isConst = true;
            return v;
        }
        case EK::Null: v.s = "\"\""; v.t = want ? want : bt["line"]; return v;
        case EK::Agg: return aggregate(e, want);
        case EK::Qual: {
            TP t = typeOfMark(e->a);
            Val x = ex(e->b, t);
            if (t->k == Type::Array && x.t && x.t->k == Type::Array && isUnconstrained(t)) return x;
            x.t = isUnconstrained(t) ? x.t : t;
            return x;
        }
        case EK::Un: return unary(e, want);
        case EK::Bin: return binary(e, want);
        case EK::Name: case EK::Sel: case EK::Call: case EK::Attr: return nameExpr(e, want);
        case EK::Others: fail(e->line, "'others' outside an aggregate");
        case EK::Open: v.s = ""; v.t = want; return v;
        case EK::New: {
            // new T'(x) / new string'(...) on access types: line is a string
            if (e->a && e->a->k == EK::Qual) return ex(e->a->b, bt["string"]);
            v.s = "\"\"";
            v.t = bt["line"];
            return v;
        }
        case EK::External: {
            // << signal .tb.dut.s : T >> -> hierarchical reference
            std::string p = e->text;
            std::string r;
            size_t i = 0;
            while (i < p.size() && (p[i] == '.' || p[i] == '^' || p[i] == '@')) ++i;
            r = p.substr(i);
            v.s = r;
            v.t = typeOfMark(e->a);
            return v;
        }
        default: break;
        }
        fail(e->line, "unsupported expression");
    }

    Val physLit(double val, const std::string& unit, int line) {
        const Sym* u = lookup(unit);
        if (!u || u->k != Sym::Unit) fail(line, "unknown unit '" + unit + "'");
        Val v;
        v.t = u->type;
        if (u->type->k == Type::Time) {
            // time in femtoseconds, written in SV time literal units
            double fs = val * (double)u->unitFs;
            v.s = timeLit(fs);
            v.isConst = true;
            return v;
        }
        // user physical type: an integer count of base units
        v.s = std::to_string((int64_t)(val * (double)u->unitFs));
        return v;
    }
    static std::string timeLit(double fs) {
        if (fs == 0) return "0.0";
        static const std::pair<const char*, double> units[] = {{"s", 1e15}, {"ms", 1e12}, {"us", 1e9}, {"ns", 1e6}, {"ps", 1e3}, {"fs", 1}};
        for (auto& [n, f] : units) {
            double q = fs / f;
            if (q >= 1 && q == (double)(int64_t)q) return "(" + std::to_string((int64_t)q) + n + ")";
        }
        std::ostringstream o;
        o.precision(17);
        o << fs;
        return "(" + o.str() + "fs)";
    }

    // ------------------------------------------------------------ aggregates
    Val aggregate(const EP& e, const TP& want0) {
        TP want = want0;
        Val v;
        if (!want) {
            // positional aggregate of std_logic: guess std_logic_vector
            want = bt["std_logic_vector"];
        }
        TP w = baseOf(want);
        if (w->k == Type::Record) {
            TP rt = want->k == Type::Record ? want : w;
            std::string s = "'{";
            size_t pos = 0;
            bool first = true;
            std::vector<std::string> vals(rt->fnames.size());
            std::string others;
            for (auto& el : e->agg) {
                if (el.choices.empty()) {
                    if (pos < rt->fnames.size()) vals[pos] = ex(el.value, rt->ftypes[pos]).s;
                    ++pos;
                    continue;
                }
                for (auto& c : el.choices) {
                    if (c->k == EK::Others) { others = "?"; for (size_t i = 0; i < vals.size(); ++i) if (vals[i].empty()) vals[i] = ex(el.value, rt->ftypes[i]).s; continue; }
                    std::string fn = c->k == EK::Name ? c->name : "";
                    for (size_t i = 0; i < rt->fnames.size(); ++i)
                        if (rt->fnames[i] == fn) vals[i] = ex(el.value, rt->ftypes[i]).s;
                }
            }
            for (size_t i = 0; i < vals.size(); ++i) {
                s += (first ? "" : ", ") + svId(rt->ftext[i]) + ": " + (vals[i].empty() ? std::string("'0") : vals[i]);
                first = false;
            }
            v.s = typeName(rt) + "'(" + s + "})";
            v.t = rt;
            return v;
        }
        if (w->k != Type::Array && w->k != Type::String) fail(e->line, "aggregate for a non-composite type");
        if (w->k == Type::String) {
            // positional aggregate of characters
            std::string s = "{";
            for (size_t i = 0; i < e->agg.size(); ++i) s += (i ? ", " : "") + std::string("string'(") + ex(e->agg[i].value, bt["character"]).s + ")";
            v.s = s + "}";
            v.t = bt["string"];
            return v;
        }
        TP elemT = want->elem ? want->elem : w->elem;
        // multi-dimensional arrays: element type is the array with the first dimension removed
        if (want->dims.size() > 1) {
            TP sub = std::make_shared<Type>(*want);
            sub->dims.erase(sub->dims.begin());
            sub->name.clear();
            elemT = sub;
        }
        bool packed = isScalarBit(elemT) && want->dims.size() == 1;
        bool known = !want->dims.empty() && !want->dims[0].box;
        // Pure others: fill
        if (e->agg.size() == 1 && e->agg[0].choices.size() == 1 && e->agg[0].choices[0]->k == EK::Others) {
            Val x = ex(e->agg[0].value, elemT);
            if (packed) {
                if (x.s == "1'b0") v.s = "'0";
                else if (x.s == "1'b1") v.s = "'1";
                else if (x.s == "1'bz") v.s = "'z";
                else if (x.s == "1'bx") v.s = "'x";
                else v.s = known ? "{" + lenOf(want->dims[0]) + "{" + x.s + "}}" : "'{default: " + x.s + "}";
                if (v.s[0] == '\'' && known && v.s.size() == 2) v.s = "{" + lenOf(want->dims[0]) + "{" + x.s + "}}";
            } else
                v.s = "'{default: " + x.s + "}";
            v.t = want;
            return v;
        }
        // positional only
        bool positional = true;
        for (auto& el : e->agg) if (!el.choices.empty()) positional = false;
        if (positional) {
            std::vector<std::string> parts;
            std::string totalW = "0";
            bool anyArray = false;
            for (auto& el : e->agg) {
                // element or (VHDL-2008) a slice of the aggregate's type
                Val x = ex(el.value, ambiguous(el.value) ? elemT : nullptr);
                if (x.t && x.t->k == Type::Array && packed) { anyArray = true; totalW = addText(totalW, widthOf(x.t)); }
                else {
                    if (!ambiguous(el.value) && x.t && elemT && x.t->k != elemT->k && isScalarBit(elemT)) x = ex(el.value, elemT);
                    totalW = addText(totalW, "1");
                }
                parts.push_back(x.s);
            }
            (void)anyArray;
            std::string s;
            if (packed) {
                s = "{";
                for (size_t i = 0; i < parts.size(); ++i) s += (i ? ", " : "") + parts[i];
                s += "}";
                v.t = known ? want : vecOf(want, totalW, wantDownto(want));
            } else {
                s = "'{";
                for (size_t i = 0; i < parts.size(); ++i) s += (i ? ", " : "") + parts[i];
                s += "}";
                v.t = known ? want : vecOf(want, std::to_string(parts.size()), false);
                if (!known) { v.t->elem = elemT; }
            }
            v.s = s;
            return v;
        }
        // named: index => value, others
        if (packed && known) {
            // build bit by bit when the range is a constant, else an assignment pattern
            const Rng& d = want->dims[0];
            if (d.lv && d.rv) {
                int64_t lo = std::min(*d.lv, *d.rv), hi = std::max(*d.lv, *d.rv);
                if (hi - lo < 4096) {
                    std::map<int64_t, std::string> bits;
                    std::string others;
                    std::vector<std::pair<std::string, std::string>> dynamic;
                    for (auto& el : e->agg) {
                        for (auto& c : el.choices) {
                            if (c->k == EK::Others) { others = ex(el.value, elemT).s; continue; }
                            if (c->k == EK::Range || (c->k == EK::Attr && c->name == "range")) {
                                Rng r = rangeOf(c);
                                if (r.lv && r.rv) {
                                    Val x = ex(el.value, elemT);
                                    for (int64_t i = std::min(*r.lv, *r.rv); i <= std::max(*r.lv, *r.rv); ++i) bits[i] = x.s;
                                    continue;
                                }
                                fail(c->line, "aggregate choice range must be static");
                            }
                            auto iv = fold(c);
                            Val x = ex(el.value, elemT);
                            if (iv) bits[*iv] = x.s;
                            else dynamic.push_back({ex(c, nullptr).s, x.s});
                        }
                    }
                    if (dynamic.empty()) {
                        std::string s = "{";
                        bool first = true;
                        std::vector<int64_t> idx;
                        if (d.downto) for (int64_t i = hi; i >= lo; --i) idx.push_back(i);
                        else for (int64_t i = lo; i <= hi; ++i) idx.push_back(i);
                        for (int64_t i : idx) {
                            auto it = bits.find(i);
                            s += (first ? "" : ", ") + (it != bits.end() ? it->second : others.empty() ? std::string("1'bx") : others);
                            first = false;
                        }
                        v.s = s + "}";
                        v.t = want;
                        return v;
                    }
                }
            }
        }
        // assignment pattern with index keys
        {
            std::string s = "'{";
            bool first = true;
            for (auto& el : e->agg) {
                Val x = ex(el.value, elemT);
                for (auto& c : el.choices) {
                    if (c->k == EK::Others) s += std::string(first ? "" : ", ") + "default: " + x.s;
                    else if (c->k == EK::Range) {
                        Rng r = rangeOf(c);
                        if (!(r.lv && r.rv)) fail(c->line, "aggregate choice range must be static");
                        for (int64_t i = std::min(*r.lv, *r.rv); i <= std::max(*r.lv, *r.rv); ++i) {
                            s += std::string(first ? "" : ", ") + std::to_string(i) + ": " + x.s;
                            first = false;
                        }
                        continue;
                    } else {
                        Val k = ex(c, want->dims.empty() ? nullptr : want->dims[0].idx);
                        s += std::string(first ? "" : ", ") + k.s + ": " + x.s;
                    }
                    first = false;
                }
            }
            s += "}";
            if (packed) v.s = svTypeExpr(want) == "" ? s : ("(" + typedCast(want) + "'(" + s + "))");
            else v.s = s;
            v.t = want;
            return v;
        }
    }
    std::string typedCast(const TP& t) {
        // a packed type usable before ' in a cast: use $bits for vectors
        if (isVec(t) && !t->dims[0].box) return widthOf(t);
        return svTypeExpr(t);
    }

    // ----------------------------------------------------------- operators
    Val boolOf(const Val& x) {
        // a condition: boolean, or (VHDL-2008) std_ulogic via ??
        TP t = baseOf(x.t);
        Val r = x;
        if (t && t->k == Type::Logic) r.s = "(" + x.s + " === 1'b1)";
        else if (t && t->k == Type::Bit) r.s = "(" + x.s + " == 1'b1)";
        r.t = bt["boolean"];
        return r;
    }

    Val unary(const EP& e, const TP& want) {
        Val v;
        if (e->op == "()") {
            Val x = ex(e->a, want);
            x.s = "(" + x.s + ")";
            return x;
        }
        if (e->op == "??") {
            Val x = ex(e->a, bt["std_ulogic"]);
            return boolOf(x);
        }
        Val x = ex(e->a, want);
        TP t = baseOf(x.t);
        v.t = x.t;
        if (e->op == "-") {
            v.s = "(-" + x.s + ")";
            if (x.iv) v.iv = -*x.iv;
            if (isNumVec(x.t)) v.s = "(" + typedCast(x.t) + "'(-" + x.s + "))";
            return v;
        }
        if (e->op == "+") { v.s = x.s; return v; }
        if (e->op == "abs") {
            if (t && t->k == Type::Real) v.s = "((" + x.s + ") < 0.0 ? -(" + x.s + ") : (" + x.s + "))";
            else if (isNumVec(x.t)) v.s = "(" + typedCast(x.t) + "'((" + x.s + ") < 0 ? -(" + x.s + ") : (" + x.s + ")))";
            else v.s = "((" + x.s + ") < 0 ? -(" + x.s + ") : (" + x.s + "))";
            return v;
        }
        if (e->op == "not") {
            if (t && t->k == Type::Bool) v.s = "(!" + x.s + ")";
            else v.s = "(~" + x.s + ")";
            return v;
        }
        // VHDL-2008 reduction operators
        static const std::map<std::string, std::string> red = {{"and", "&"}, {"or", "|"}, {"xor", "^"}, {"nand", "~&"}, {"nor", "~|"}, {"xnor", "~^"}};
        auto it = red.find(e->op);
        if (it != red.end()) {
            v.s = "(" + it->second + x.s + ")";
            v.t = x.t && x.t->elem ? x.t->elem : bt["std_ulogic"];
            return v;
        }
        fail(e->line, "unsupported operator " + e->op);
    }

    Val binary(const EP& e, const TP& want) {
        const std::string& op = e->op;
        // logical operators
        static const std::map<std::string, std::string> lg = {{"and", "&"}, {"or", "|"}, {"xor", "^"}, {"nand", "&"}, {"nor", "|"}, {"xnor", "^"}};
        if (lg.count(op)) {
            auto [a, b] = operands(e, want);
            TP t = baseOf(a.t);
            Val v;
            v.t = a.t;
            if (t && t->k == Type::Bool) {
                std::string o = op == "and" ? "&&" : op == "or" ? "||" : op == "xor" ? "^" : op == "nand" ? "&&" : op == "nor" ? "||" : "^";
                v.s = "(" + a.s + " " + o + " " + b.s + ")";
                if (op == "nand" || op == "nor" || op == "xnor") v.s = "(!" + v.s + ")";
                return v;
            }
            // std_logic scalar mixed with a vector (VHDL-2008): replicate
            if (a.t && b.t && a.t->k == Type::Array && isScalarBit(b.t)) b.s = "{" + widthOf(a.t) + "{" + b.s + "}}";
            if (a.t && b.t && b.t->k == Type::Array && isScalarBit(a.t)) { a.s = "{" + widthOf(b.t) + "{" + a.s + "}}"; v.t = b.t; }
            v.s = "(" + a.s + " " + lg.at(op) + " " + b.s + ")";
            if (op == "nand" || op == "nor" || op == "xnor") v.s = "(~" + v.s + ")";
            return v;
        }
        // relational
        static const std::set<std::string> rel = {"=", "/=", "<", "<=", ">", ">="};
        if (rel.count(op)) {
            auto [a, b] = operands(e, nullptr);
            Val v;
            v.t = bt["boolean"];
            TP ta = baseOf(a.t), tb = baseOf(b.t);
            bool four = isFourState(a.t) || isFourState(b.t);
            if (op == "=" || op == "/=") {
                bool strs = (ta && (ta->k == Type::String || ta->k == Type::Line)) || (tb && (tb->k == Type::String || tb->k == Type::Line));
                if (four && !(isNumVec(a.t) || isNumVec(b.t))) v.s = "(" + a.s + (op == "=" ? " === " : " !== ") + b.s + ")";
                else if (four) v.s = "bit'(" + numCmpOperand(a, b) + (op == "=" ? " == " : " != ") + numCmpOperand(b, a) + ")";
                else if (strs) v.s = "(" + a.s + (op == "=" ? " == " : " != ") + b.s + ")";
                else v.s = "(" + a.s + (op == "=" ? " == " : " != ") + b.s + ")";
                return v;
            }
            std::string o = op;
            if (four) v.s = "bit'(" + numCmpOperand(a, b) + " " + o + " " + numCmpOperand(b, a) + ")";
            else v.s = "(" + a.s + " " + o + " " + b.s + ")";
            return v;
        }
        // matching relational (VHDL-2008): std_ulogic result
        if (op.size() > 1 && op[0] == '?') {
            auto [a, b] = operands(e, nullptr);
            Val v;
            v.t = bt["std_ulogic"];
            std::string o = op.substr(1);
            if (o == "=") v.s = "(" + a.s + " ==? " + b.s + ")";
            else if (o == "/=") v.s = "(" + a.s + " !=? " + b.s + ")";
            else v.s = "(" + numCmpOperand(a, b) + " " + o + " " + numCmpOperand(b, a) + ")";
            return v;
        }
        if (op == "&") return concat(e, want);
        // shifts
        if (op == "sll" || op == "srl" || op == "sla" || op == "sra" || op == "rol" || op == "ror") {
            Val a = ex(e->a, want);
            Val b = ex(e->b, bt["integer"]);
            return shiftOp(op, a, b);
        }
        // arithmetic
        return arith(e, want);
    }

    // numeric operands of a comparison: signedness and integer operands
    std::string numCmpOperand(const Val& x, const Val& other) {
        if (isSignedVec(x.t)) return "$signed(" + x.s + ")";
        if (isSignedVec(other.t) && x.t && x.t->k == Type::Int) return x.s;
        if (isVec(x.t) && other.t && other.t->k == Type::Int) return "$unsigned(" + x.s + ")";
        return x.s;
    }

    Val shiftOp(const std::string& op, const Val& a, const Val& b) {
        Val v;
        v.t = a.t;
        std::string w = widthOf(a.t), c = typedCast(a.t);
        if (op == "sll" || op == "sla") v.s = "(" + c + "'(" + a.s + " << " + b.s + "))";
        else if (op == "srl") v.s = "(" + c + "'(" + a.s + " >> " + b.s + "))";
        else if (op == "sra") v.s = "(" + c + "'($signed(" + a.s + ") >>> " + b.s + "))";
        else {
            // rotate: through a doubled vector
            std::string n = "((" + b.s + ") % " + w + ")";
            if (op == "rol") v.s = "(" + c + "'({" + a.s + ", " + a.s + "} >> (" + w + " - " + n + ")))";
            else v.s = "(" + c + "'({" + a.s + ", " + a.s + "} >> " + n + "))";
        }
        return v;
    }

    std::pair<Val, Val> operands(const EP& e, const TP& want) {
        Val a, b;
        if (ambiguous(e->a) && !ambiguous(e->b)) {
            b = ex(e->b, want);
            a = ex(e->a, b.t);
        } else if (ambiguous(e->a) && ambiguous(e->b) && want && (want->k == Type::Bool || want->k == Type::Logic || want->k == Type::Bit)) {
            a = ex(e->a, want);
            b = ex(e->b, a.t);
        } else {
            a = ex(e->a, ambiguous(e->a) ? want : (e->a->k == EK::Int ? nullptr : want));
            if (e->a->k == EK::Int || (a.t && a.t->k == Type::Int && !ambiguous(e->b) && e->b->k != EK::Int)) {
                // the other operand decides (e.g. 1 + unsigned)
                b = ex(e->b, nullptr);
                if (b.t && b.t->k == Type::Real && e->a->k == EK::Int) a = ex(e->a, b.t);
            } else b = ex(e->b, a.t && a.t->k == Type::Array && ambiguous(e->b) ? a.t : (a.t && (a.t->k == Type::Real || a.t->k == Type::Time || a.t->k == Type::Enum || isScalarBit(a.t) || a.t->k == Type::Char) ? a.t : nullptr));
        }
        return {a, b};
    }

    Val concat(const EP& e, const TP& want) {
        // flatten a & b & c
        std::vector<EP> parts;
        std::function<void(const EP&)> flat = [&](const EP& x) {
            if (x->k == EK::Bin && x->op == "&") { flat(x->a); flat(x->b); }
            else parts.push_back(x);
        };
        flat(e);
        // string concatenation?
        bool isStr = want && (baseOf(want)->k == Type::String || baseOf(want)->k == Type::Line);
        std::vector<Val> vals(parts.size());
        TP arrT = want && want->k == Type::Array ? want : nullptr;
        if (!isStr) {
            for (size_t i = 0; i < parts.size(); ++i)
                if (!ambiguous(parts[i])) {
                    vals[i] = ex(parts[i], nullptr);
                    TP t = baseOf(vals[i].t);
                    if (t && (t->k == Type::String || t->k == Type::Line)) isStr = true;
                    if (!arrT && vals[i].t && vals[i].t->k == Type::Array) arrT = vals[i].t;
                }
            if (!arrT && !isStr) {
                bool anyBits = false;
                for (size_t i = 0; i < parts.size(); ++i) if (vals[i].t && isScalarBit(vals[i].t)) anyBits = true;
                if (!anyBits && !want) isStr = true;   // "abc" & "def"
                else if (!anyBits && want && want->k == Type::Char) isStr = true;
            }
        }
        Val v;
        if (isStr) {
            std::string s = "{";
            for (size_t i = 0; i < parts.size(); ++i) {
                Val x = vals[i].s.empty() ? ex(parts[i], parts[i]->k == EK::Char ? bt["character"] : bt["string"]) : vals[i];
                TP t = baseOf(x.t);
                if (t && t->k == Type::Char) x.s = "string'(" + x.s + ")";
                s += (i ? ", " : "") + x.s;
            }
            v.s = s + "}";
            v.t = bt["string"];
            return v;
        }
        TP elemT = arrT && arrT->elem ? arrT->elem : bt["std_ulogic"];
        std::string w = "0";
        std::string s = "{";
        bool packed = arrT ? isScalarBit(elemT) : true;
        for (size_t i = 0; i < parts.size(); ++i) {
            Val x = vals[i].s.empty() ? ex(parts[i], parts[i]->k == EK::Char ? elemT : (arrT ? arrT : bt["std_logic_vector"])) : vals[i];
            if (parts[i]->k == EK::Agg && x.t && x.t->k == Type::Array && !x.t->dims.empty() && x.t->dims[0].box) {
                // others in a concatenation: unknown width
                fail(parts[i]->line, "aggregate with 'others' in a concatenation needs a qualified type");
            }
            w = addText(w, x.t && x.t->k == Type::Array ? widthOf(x.t) : "1");
            s += (i ? ", " : "") + x.s;
        }
        s += "}";
        if (!packed) s = "'" + s;   // unpacked arrays: assignment pattern of elements
        v.s = s;
        TP base = arrT ? arrT : bt["std_logic_vector"];
        v.t = vecOf(base, w, wantDownto(want ? want : base));
        v.t->elem = elemT;
        v.t->flavor = base->flavor;
        if (!packed) v.t = arrT;
        return v;
    }

    Val arith(const EP& e, const TP& want) {
        const std::string& op = e->op;
        auto [a, b] = operands(e, want);
        TP ta = baseOf(a.t), tb = baseOf(b.t);
        Val v;
        std::string o = op == "rem" ? "%" : op;
        // integer / real / time
        if (!isVec(a.t) && !isVec(b.t)) {
            if (op == "mod") {
                if (ta && ta->k == Type::Int) { v.s = "vhdl_rt::imod(" + a.s + ", " + b.s + ")"; v.t = a.t; return v; }
            }
            if (op == "/" && ta && ta->k == Type::Time && tb && tb->k == Type::Time) {
                v.s = "longint'((" + a.s + ") / (" + b.s + "))";
                v.t = bt["integer"];
                return v;
            }
            if (op == "**" && ta && ta->k == Type::Int && a.iv && *a.iv == 2) {
                v.s = "(1 << (" + b.s + "))";
                v.t = a.t;
                return v;
            }
            v.s = "(" + a.s + " " + o + " " + b.s + ")";
            if (op == "mod") v.s = "vhdl_rt::imod(" + a.s + ", " + b.s + ")";
            // time * integer / real * time: the result is time
            if (ta && tb && (ta->k == Type::Time || tb->k == Type::Time)) v.t = ta->k == Type::Time ? a.t : b.t;
            else if (ta && tb && ta->k == Type::Real && tb->k == Type::Int) { v.t = a.t; v.s = "(" + a.s + " " + o + " real'(" + b.s + "))"; }
            else if (ta && tb && ta->k == Type::Int && tb->k == Type::Real) { v.t = b.t; v.s = "(real'(" + a.s + ") " + o + " " + b.s + ")"; }
            else v.t = ta && ta->k == Type::Int && tb && tb->k != Type::Int ? b.t : a.t;
            if (v.t && v.t->k == Type::Time && op == "/" && tb && tb->k == Type::Int) v.s = "(" + a.s + " / real'(" + b.s + "))";
            if (a.iv && b.iv) {
                if (op == "+") v.iv = *a.iv + *b.iv;
                if (op == "-") v.iv = *a.iv - *b.iv;
                if (op == "*") v.iv = *a.iv * *b.iv;
            }
            return v;
        }
        // numeric_std / std_logic_unsigned arithmetic on vectors
        TP vt = isVec(a.t) ? a.t : b.t;
        bool sgn = isSignedVec(a.t) || isSignedVec(b.t);
        std::string wa = isVec(a.t) ? widthOf(a.t) : "", wb = isVec(b.t) ? widthOf(b.t) : "";
        std::string w;
        if (op == "*") w = (!wa.empty() && !wb.empty()) ? addText(wa, wb) : addText(wa.empty() ? wb : wa, wa.empty() ? wb : wa);
        else if (op == "/") w = wa.empty() ? wb : wa;
        else if (op == "mod" || op == "rem") w = wb.empty() ? wa : wb;
        else w = (!wa.empty() && !wb.empty()) ? maxText(wa, wb) : (wa.empty() ? wb : wa);
        auto opnd = [&](const Val& x, const std::string& xw) {
            if (isVec(x.t)) return sgn ? "$signed(" + x.s + ")" : x.s;
            // integer operand: converted to the vector's width
            std::string other = xw.empty() ? (wa.empty() ? wb : wa) : xw;
            if (x.t && x.t->k == Type::Logic) return std::string(sgn ? "$signed({1'b0, " + x.s + "})" : x.s);
            return (sgn ? "$signed(" : "$unsigned(") + other + "'(" + x.s + "))";
        };
        std::string sa = opnd(a, wa), sb = opnd(b, wb);
        std::string expr;
        if (op == "mod" && sgn) expr = "vhdl_rt::smod(" + sa + ", " + sb + ")";
        else expr = sa + " " + o + " " + sb;
        v.t = vecOf(vt, w, true);
        v.t->flavor = sgn ? "signed" : vt->flavor;
        v.s = "(" + w + "'(" + expr + "))";
        if (sgn) v.s = "$signed" + v.s;
        return v;
    }

    // ------------------------------------------------------------- names
    // A name, call, index, slice, attribute or selected name.
    Val nameExpr(const EP& e, const TP& want) {
        Val v;
        if (e->k == EK::Name) {
            auto syms = lookupAll(e->name);
            if (!syms) {
                if (e->name.size() > 2 && e->name[0] == '"') fail(e->line, "unknown operator " + e->name);
                fail(e->line, "'" + e->text + "' is not declared");
            }
            // enumeration literal: pick by expected type
            const Sym* y = &syms->front();
            if (y->k == Sym::Lit && syms->size() > 1 && want) {
                for (auto& s : *syms)
                    if (s.k == Sym::Lit && sameBase(s.type, want)) { y = &s; break; }
            }
            if (y->k == Sym::Sub) {
                // function call without arguments; pick the overload with no parameters
                return callSub(e, *syms, {}, want);
            }
            return symVal(*y, e);
        }
        if (e->k == EK::Sel) {
            // package-qualified name?
            if (const Sym* y = lookupSel(e)) {
                std::string pkg = e->a->name;
                auto it = pkgScope.find(pkg);
                if (y->k == Sym::Sub) {
                    if (it != pkgScope.end() && it->second.count(e->name)) return callSub(e, it->second[e->name], {}, want);
                    return callSub(e, builtinScope[e->name], {}, want);
                }
                return symVal(*y, e);
            }
            // record element
            Val p = ex(e->a, nullptr);
            TP t = baseOf(p.t);
            if (t && t->k == Type::Line && e->name == "all") { v.s = p.s; v.t = bt["string"]; return v; }
            if (e->name == "all") { v.s = p.s; v.t = p.t && p.t->target ? p.t->target : p.t; return v; }
            TP rt = p.t && p.t->k == Type::Record ? p.t : t;
            if (!rt || rt->k != Type::Record) fail(e->line, "'" + e->text + "': prefix is not a record");
            for (size_t i = 0; i < rt->fnames.size(); ++i)
                if (rt->fnames[i] == e->name) {
                    v.s = p.s + "." + svId(rt->ftext[i]);
                    v.t = rt->ftypes[i];
                    return v;
                }
            fail(e->line, "record has no element '" + e->text + "'");
        }
        if (e->k == EK::Attr) return attribute(e, want);
        // Call: function call, index, slice or type conversion
        EP pre = e->a;
        const std::vector<Sym>* syms = nullptr;
        if (pre->k == EK::Name) syms = lookupAll(pre->name);
        else if (pre->k == EK::Sel) {
            if (const Sym* y = lookupSel(pre)) {
                auto it = pkgScope.find(pre->a->name);
                if (it != pkgScope.end() && it->second.count(pre->name)) syms = &it->second[pre->name];
                else if (builtinScope.count(pre->name)) syms = &builtinScope[pre->name];
                (void)y;
            }
        }
        if (syms && !syms->empty()) {
            const Sym& y = syms->front();
            if (y.k == Sym::Sub) return callSub(pre, *syms, e->args, want);
            if (y.k == Sym::TypeK) return conversion(y.type, e, want);
        }
        // indexed or sliced object (or a function result)
        Val p = ex(pre, nullptr);
        return indexOrSlice(p, e);
    }

    Val symVal(const Sym& y, const EP& e) {
        Val v;
        switch (y.k) {
        case Sym::Obj:
            v.s = y.home == "*" ? y.sv : qual(y);
            v.t = y.type;
            if (y.cls == "constant" && y.decl && y.decl->init) {
                auto f = fold(y.decl->init);
                if (f && (!y.type || y.type->k == Type::Int)) v.iv = f;
            }
            if (cx->proc && cx->proc->active && y.cls == "signal") {
                // reading a signal through a driver variable is not possible; read the signal
            }
            return v;
        case Sym::Lit: v.s = y.home == "*" ? y.sv : (y.home.empty() || y.home == cx->container ? y.sv : svId(y.home) + "::" + y.sv); v.t = y.type; v.isConst = true; return v;
        case Sym::Alias: v.s = y.aliasSv; v.t = y.type; return v;
        case Sym::Unit: return physLit(1, y.name, e->line);
        case Sym::TypeK: fail(e->line, "type '" + e->text + "' used as a value");
        default: fail(e->line, "'" + e->text + "' is not a value");
        }
    }

    Val indexOrSlice(const Val& p, const EP& e) {
        Val v;
        TP t = p.t;
        if (!t) fail(e->line, "cannot index this expression");
        if (t->k == Type::String || t->k == Type::Line || (t->k == Type::Array && t->elem && t->elem->k == Type::Char && t->dims.size() <= 1)) {
            // SV strings are 0-based; VHDL strings start at 'left (1 for literals)
            if (e->args.size() == 1 && e->args[0].actual->k == EK::Range) {
                Val l = ex(e->args[0].actual->a, bt["integer"]), r = ex(e->args[0].actual->b, bt["integer"]);
                v.s = p.s + ".substr((" + l.s + ")-1, (" + r.s + ")-1)";
                v.t = bt["string"];
                return v;
            }
            Val i = ex(e->args[0].actual, bt["integer"]);
            v.s = p.s + "[(" + i.s + ")-1]";
            v.t = bt["character"];
            return v;
        }
        if (t->k != Type::Array) fail(e->line, "'" + p.s + "' is not an array");
        // slice
        if (e->args.size() == 1 && (e->args[0].actual->k == EK::Range || (e->args[0].actual->k == EK::Attr && (e->args[0].actual->name == "range" || e->args[0].actual->name == "reverse_range")) ||
                                    e->args[0].actual->k == EK::SubtypeRange || isTypeName(e->args[0].actual))) {
            Rng r = rangeOf(e->args[0].actual);
            TP st = std::make_shared<Type>(*t);
            st->name.clear();
            st->base = baseOf(t);
            r.idx = t->dims[0].idx;
            st->dims[0] = r;
            v.t = st;
            if (r.lv && r.rv) v.s = p.s + "[" + r.l + ":" + r.r + "]";
            else {
                // a part-select of variable position: indexed form with a constant width when one exists
                auto w = constDiff(e->args[0].actual->a, e->args[0].actual->b);
                if (w) {
                    std::string lo = r.downto ? r.r : r.l;
                    std::string base = (!t->dims[0].box && t->dims[0].downto == r.downto) ? lo : lo;
                    v.s = p.s + "[" + base + (r.downto ? " +: " : " +: ") + std::to_string(*w + 1) + "]";
                    if (!t->dims[0].box && !t->dims[0].downto) v.s = p.s + "[" + r.l + " +: " + std::to_string(*w + 1) + "]";
                } else
                    v.s = p.s + "[" + r.l + ":" + r.r + "]";
            }
            return v;
        }
        std::string s = p.s;
        TP cur = t;
        size_t dim = 0;
        for (auto& a : e->args) {
            if (!cur || cur->k != Type::Array) fail(e->line, "too many indices");
            Val i = ex(a.actual, dim < cur->dims.size() ? cur->dims[dim].idx : bt["integer"]);
            if (i.t && i.t->k == Type::Logic) i.s = "int'(" + i.s + ")";
            s += "[" + i.s + "]";
            ++dim;
            if (dim >= cur->dims.size()) { cur = cur->elem; dim = 0; }
        }
        if (dim != 0) {
            TP st = std::make_shared<Type>(*cur);
            st->dims.erase(st->dims.begin(), st->dims.begin() + (long)dim);
            cur = st;
        }
        v.s = s;
        v.t = cur;
        return v;
    }
    bool isTypeName(const EP& e) {
        if (e->k != EK::Name && e->k != EK::Sel) return false;
        const Sym* y = e->k == EK::Name ? lookup(e->name) : lookupSel(e);
        return y && y->k == Sym::TypeK && (y->type->range || y->type->k == Type::Enum);
    }

    // l - r as a constant when both are "x + c" forms of the same x.
    std::optional<int64_t> constDiff(const EP& l, const EP& r) {
        std::map<std::string, int64_t> a, b;
        int64_t ca = 0, cb = 0;
        if (!linear(l, 1, a, ca) || !linear(r, 1, b, cb)) return std::nullopt;
        for (auto& [k, c] : b) a[k] -= c;
        for (auto& [k, c] : a)
            if (c != 0) return std::nullopt;
        int64_t d = ca - cb;
        return d < 0 ? -d : d;
    }
    bool linear(const EP& e, int64_t sign, std::map<std::string, int64_t>& terms, int64_t& c) {
        if (auto f = fold(e)) { c += sign * *f; return true; }
        if (e->k == EK::Un && e->op == "()") return linear(e->a, sign, terms, c);
        if (e->k == EK::Un && e->op == "-") return linear(e->a, -sign, terms, c);
        if (e->k == EK::Bin && (e->op == "+" || e->op == "-"))
            return linear(e->a, sign, terms, c) && linear(e->b, e->op == "+" ? sign : -sign, terms, c);
        if (e->k == EK::Bin && e->op == "*") {
            if (auto f = fold(e->a)) return linear(e->b, sign * *f, terms, c);
            if (auto f = fold(e->b)) return linear(e->a, sign * *f, terms, c);
        }
        Val v = ex(e, nullptr);
        terms[v.s] += sign;
        return true;
    }

    // ---------------------------------------------------------- conversion
    Val conversion(const TP& to, const EP& e, const TP& want) {
        (void)want;
        if (e->args.size() != 1) fail(e->line, "type conversion takes one operand");
        Val x = ex(e->args[0].actual, ambiguous(e->args[0].actual) ? to : nullptr);
        Val v;
        TP tt = baseOf(to), ft = baseOf(x.t);
        v.t = to;
        if (to->k == Type::Array && isUnconstrained(to) && x.t && x.t->k == Type::Array) {
            TP n = std::make_shared<Type>(*x.t);
            n->flavor = to->flavor;
            n->elem = to->elem;
            n->base = baseOf(to);
            n->name = "";
            v.t = n;
        }
        if (tt->k == Type::Int) {
            if (ft && ft->k == Type::Real) v.s = "int'(" + x.s + ")";   // rounds to nearest, like VHDL
            else v.s = x.s;
            return v;
        }
        if (tt->k == Type::Real) { v.s = "real'(" + x.s + ")"; return v; }
        if (isVec(to)) {
            if (to->flavor == "signed") v.s = "$signed(" + x.s + ")";
            else if (isSignedVec(x.t)) v.s = "$unsigned(" + x.s + ")";
            else v.s = x.s;
            return v;
        }
        v.s = x.s;
        return v;
    }

    // ------------------------------------------------------------ attributes
    Val attribute(const EP& e, const TP& want) {
        Val v;
        const std::string& a = e->name;
        // prefix may be a type or an object
        TP pt;
        bool isType = false;
        Val p;
        if ((e->a->k == EK::Name || e->a->k == EK::Sel)) {
            const Sym* y = e->a->k == EK::Name ? lookup(e->a->name) : lookupSel(e->a);
            if (y && y->k == Sym::TypeK) { pt = y->type; isType = true; }
        }
        if (!isType) {
            p = ex(e->a, nullptr);
            pt = p.t;
        }
        TP bt0 = baseOf(pt);
        // signal attributes
        if (a == "event" || a == "last_value" || a == "active" || a == "stable" || a == "last_event" || a == "quiet" || a == "transaction" || a == "delayed" || a == "last_active" || a == "driving" || a == "driving_value") {
            return signalAttr(e, p);
        }
        if (a == "image") {
            Val x = ex(e->b, pt);
            return image(pt, x);
        }
        if (a == "value") {
            Val x = ex(e->b, bt["string"]);
            if (bt0->k == Type::Int) { v.s = "vhdl_rt::int_value(" + x.s + ")"; v.t = pt; return v; }
            if (bt0->k == Type::Real) { v.s = "vhdl_rt::real_value(" + x.s + ")"; v.t = pt; return v; }
            if (bt0->k == Type::Bool) { v.s = "bit'(vhdl_rt::lower(" + x.s + ") == \"true\")"; v.t = pt; return v; }
            if (bt0->k == Type::Enum && bt0->sv != "int") {
                std::string s = "vhdl_rt::lower(" + x.s + ")", r = "'0";
                for (size_t i = bt0->lits.size(); i-- > 0;) r = "(" + s + " == \"" + bt0->lits[i] + "\" ? " + enumLitSv(bt0, i) + " : " + r + ")";
                v.s = typeName(bt0) + "'(" + r + ")";
                v.t = pt;
                return v;
            }
            fail(e->line, "'value of this type is not supported");
        }
        if (a == "pos") {
            Val x = ex(e->b, pt);
            v.t = bt["integer"];
            if (bt0->k == Type::Logic) v.s = "vhdl_rt::sl_pos(" + x.s + ")";
            else v.s = "int'(" + x.s + ")";
            return v;
        }
        if (a == "val") {
            Val x = ex(e->b, bt["integer"]);
            v.t = pt;
            if (bt0->k == Type::Enum && bt0->sv != "int") v.s = typeName(bt0) + "'(" + x.s + ")";
            else if (bt0->k == Type::Logic) v.s = "vhdl_rt::sl_val(" + x.s + ")";
            else if (bt0->k == Type::Char) v.s = "8'(" + x.s + ")";
            else if (bt0->k == Type::Bool || bt0->k == Type::Bit) v.s = "1'(" + x.s + ")";
            else v.s = x.s;
            return v;
        }
        if (a == "succ" || a == "pred" || a == "leftof" || a == "rightof") {
            Val x = ex(e->b, pt);
            bool up = a == "succ" || a == "rightof";
            v.t = pt;
            if (bt0->k == Type::Enum && bt0->sv != "int") v.s = typeName(bt0) + "'(int'(" + x.s + ")" + (up ? " + 1" : " - 1") + ")";
            else v.s = "(" + x.s + (up ? " + 1" : " - 1") + ")";
            return v;
        }
        // array / scalar range attributes
        int dim = e->b ? (int)fold(e->b).value_or(1) - 1 : 0;
        if (pt && (pt->k == Type::String || pt->k == Type::Line || (pt->k == Type::Array && pt->elem && pt->elem->k == Type::Char && pt->dims.size() == 1 && (pt->dims[0].box)))) {
            v.t = bt["integer"];
            if (a == "length") { v.s = p.s + ".len()"; return v; }
            if (a == "left" || a == "low") { v.s = "1"; return v; }
            if (a == "right" || a == "high") { v.s = p.s + ".len()"; return v; }
            if (a == "ascending") { v.s = "1'b1"; v.t = bt["boolean"]; return v; }
        }
        if (pt && pt->k == Type::Array) {
            if (dim >= (int)pt->dims.size()) fail(e->line, "dimension out of range");
            const Rng& r = pt->dims[dim];
            v.t = r.idx ? r.idx : bt["integer"];
            if (v.t && v.t->k != Type::Enum) v.t = bt["integer"];
            if (r.box) {
                std::string d = e->b ? ", " + std::to_string(dim + 1) : "";
                if (a == "length") { v.s = "$size(" + p.s + d + ")"; v.t = bt["integer"]; return v; }
                if (a == "left") { v.s = "$left(" + p.s + d + ")"; return v; }
                if (a == "right") { v.s = "$right(" + p.s + d + ")"; return v; }
                if (a == "high") { v.s = "$high(" + p.s + d + ")"; return v; }
                if (a == "low") { v.s = "$low(" + p.s + d + ")"; return v; }
                if (a == "ascending") { v.s = "bit'($increment(" + p.s + d + ") == -1)"; v.t = bt["boolean"]; return v; }
                fail(e->line, "unsupported attribute '" + e->text + "'");
            }
            if (a == "length") { v.s = lenOf(r); v.t = bt["integer"]; return v; }
            if (a == "left") { v.s = r.l; v.iv = r.lv; return v; }
            if (a == "right") { v.s = r.r; v.iv = r.rv; return v; }
            if (a == "high") { v.s = r.downto ? r.l : r.r; v.iv = r.downto ? r.lv : r.rv; return v; }
            if (a == "low") { v.s = r.downto ? r.r : r.l; v.iv = r.downto ? r.rv : r.lv; return v; }
            if (a == "ascending") { v.s = r.downto ? "1'b0" : "1'b1"; v.t = bt["boolean"]; return v; }
            if (a == "element") { v.t = pt->elem; return v; }
        }
        // scalar types
        if (pt) {
            v.t = pt;
            std::optional<Rng> r = pt->range;
            if (!r) {
                if (bt0->k == Type::Int) r = mkRng("(-2147483647-1)", "2147483647", false);
                else if (bt0->k == Type::Enum) r = mkRng(enumLitSv(bt0, 0), enumLitSv(bt0, bt0->lits.size() - 1), false);
                else if (bt0->k == Type::Bool || bt0->k == Type::Bit) r = mkRng("1'b0", "1'b1", false);
                else if (bt0->k == Type::Logic) r = mkRng("1'bx", "1'bx", false);
                else if (bt0->k == Type::Char) r = mkRng("8'd0", "8'd255", false);
                else if (bt0->k == Type::Real) r = mkRng("-1.7976931348623157e308", "1.7976931348623157e308", false);
                else if (bt0->k == Type::Time) r = mkRng("(-9223372036854775807.0*1fs)", "(9223372036854775807.0*1fs)", false);
            }
            if (r) {
                if (a == "left") { v.s = r->l; v.iv = r->lv; return v; }
                if (a == "right") { v.s = r->r; v.iv = r->rv; return v; }
                if (a == "high") { v.s = r->downto ? r->l : r->r; v.iv = r->downto ? r->lv : r->rv; return v; }
                if (a == "low") { v.s = r->downto ? r->r : r->l; v.iv = r->downto ? r->rv : r->lv; return v; }
                if (a == "ascending") { v.s = r->downto ? "1'b0" : "1'b1"; v.t = bt["boolean"]; return v; }
            }
        }
        (void)want;
        fail(e->line, "unsupported attribute '" + e->text + "'");
    }

    Val image(const TP& t, const Val& x) {
        Val v;
        v.t = bt["string"];
        TP b = baseOf(t);
        switch (b->k) {
        case Type::Int: v.s = "$sformatf(\"%0d\", " + x.s + ")"; break;
        case Type::Real: v.s = "vhdl_rt::real_image(" + x.s + ")"; break;
        case Type::Time: v.s = "vhdl_rt::time_image(" + x.s + ")"; break;
        case Type::Bool: v.s = "((" + x.s + ") ? \"true\" : \"false\")"; break;
        case Type::Bit: v.s = "((" + x.s + ") ? \"'1'\" : \"'0'\")"; break;
        case Type::Logic: v.s = "{\"'\", vhdl_rt::sl_str(" + x.s + "), \"'\"}"; break;
        case Type::Char: v.s = "{\"'\", string'(" + x.s + "), \"'\"}"; break;
        case Type::Enum:
            if (b->sv == "int") {
                std::string r = "\"?\"";
                for (size_t i = b->lits.size(); i-- > 0;) r = "(" + x.s + " == " + b->litSv[i] + " ? \"" + b->lits[i] + "\" : " + r + ")";
                v.s = r;
            } else v.s = "vhdl_rt::lower(" + x.s + ".name())";
            break;
        case Type::String: case Type::Line: v.s = x.s; break;
        default: v.s = "$sformatf(\"%p\", " + x.s + ")"; break;
        }
        return v;
    }

    Val signalAttr(const EP& e, const Val& p) {
        Val v;
        const std::string& a = e->name;
        std::string prev = prevName(p.s);
        if (a == "event" || a == "active") {
            requirePrev(p.s);
            v.s = "(" + p.s + " !== " + prev + ")";
            v.t = bt["boolean"];
            return v;
        }
        if (a == "last_value") {
            requirePrev(p.s);
            v.s = prev;
            v.t = p.t;
            return v;
        }
        if (a == "stable" || a == "quiet") {
            if (e->b) fail(e->line, "'" + a + "(T) is not supported yet");
            requirePrev(p.s);
            v.s = "(" + p.s + " === " + prev + ")";
            v.t = bt["boolean"];
            return v;
        }
        fail(e->line, "signal attribute '" + a + " is not supported yet");
    }
    std::string prevName(const std::string& s) {
        std::string r;
        for (char c : s) r.push_back(std::isalnum((unsigned char)c) || c == '_' ? c : '_');
        return r + "__prev";
    }
    void requirePrev(const std::string& s) {
        if (cx->proc) cx->proc->prev.insert(s);
    }

    // ----------------------------------------------------------- calls
    // Pick an overload of a subprogram by argument count and types.
    const Sym* pickOverload(const std::vector<Sym>& syms, const std::vector<Assoc>& args, const TP& want, int line) {
        std::vector<const Sym*> c;
        for (auto& s : syms)
            if (s.k == Sym::Sub) c.push_back(&s);
        if (c.empty()) fail(line, "not a subprogram");
        if (c.size() == 1 || !c[0]->sub) return c[0];
        const Sym* best = nullptr;
        int bestScore = -1000;
        for (auto* s : c) {
            if (!s->sub) continue;
            auto& ps = s->sub->params;
            size_t nparams = ps.size();
            if (args.size() > nparams) continue;
            int score = 0;
            bool ok = true;
            for (size_t i = nparams; i-- > args.size();)
                if (!ps[i]->init) ok = false;
            if (!ok) continue;
            for (size_t i = 0; i < args.size(); ++i) {
                size_t pi = i;
                if (args[i].formal && args[i].formal->k == EK::Name) {
                    pi = nparams;
                    for (size_t k = 0; k < nparams; ++k)
                        if (ps[k]->names[0] == args[i].formal->name) pi = k;
                    if (pi == nparams) { ok = false; break; }
                }
                TP ft = paramType(s, pi);
                if (ambiguous(args[i].actual)) {
                    if (args[i].actual->k == EK::Char && ft && !(isScalarBit(ft) || baseOf(ft)->k == Type::Char || baseOf(ft)->k == Type::Enum)) score -= 5;
                    if (args[i].actual->k == EK::Str && ft && !(ft->k == Type::Array || ft->k == Type::String)) score -= 5;
                    continue;
                }
                Val av = exQuiet(args[i].actual);
                if (!av.t || !ft) continue;
                if (sameBase(av.t, ft)) score += 3;
                else if (baseOf(av.t)->k == baseOf(ft)->k && (baseOf(ft)->k != Type::Array || baseOf(ft)->flavor == baseOf(av.t)->flavor)) score += 1;
                else if (baseOf(av.t)->k == Type::Array && baseOf(ft)->k == Type::Array && isScalarBit(av.t->elem) && isScalarBit(ft->elem) && av.t->elem->k == ft->elem->k) score += 0;
                else { ok = false; break; }
            }
            if (!ok) continue;
            if (want && s->sub->isFunc) {
                TP rt = retType(s);
                if (rt && sameBase(rt, want)) score += 2;
                else if (rt && baseOf(rt)->k != baseOf(want)->k) score -= 2;
            }
            if (score > bestScore) { bestScore = score; best = s; }
        }
        return best ? best : c[0];
    }
    Val exQuiet(const EP& e) {
        try {
            auto* g = cx->proc;
            std::set<std::string> savedPrev = g ? g->prev : std::set<std::string>();
            Val v = ex(e, nullptr);
            if (g) g->prev = savedPrev;
            return v;
        } catch (const Error&) {
            return Val{};
        }
    }
    std::map<const Subprog*, std::vector<TP>> paramTypes;
    std::map<const Subprog*, TP> retTypes;
    TP paramType(const Sym* s, size_t i) {
        auto it = paramTypes.find(s->sub.get());
        if (it != paramTypes.end() && i < it->second.size()) return it->second[i];
        return nullptr;
    }
    TP retType(const Sym* s) {
        auto it = retTypes.find(s->sub.get());
        return it == retTypes.end() ? nullptr : it->second;
    }

    Val callSub(const EP& nameE, const std::vector<Sym>& syms, const std::vector<Assoc>& args, const TP& want) {
        int line = nameE->line;
        const Sym* s = pickOverload(syms, args, want, line);
        if (!s->builtin.empty() && !s->sub) return builtinCall(s->builtin, args, want, line);
        if (!s->sub) fail(line, "not a subprogram");
        return userCall(*s, args, want, line);
    }

    // arguments in parameter order (named association resolved)
    std::vector<EP> orderArgs(const Subprog& sp, const std::vector<Assoc>& args, int line) {
        std::vector<EP> r(sp.params.size());
        size_t pos = 0;
        for (auto& a : args) {
            if (!a.formal) {
                if (pos >= r.size()) fail(line, "too many arguments to '" + sp.text + "'");
                r[pos++] = a.actual;
                continue;
            }
            std::string fn = a.formal->k == EK::Name ? a.formal->name : "";
            bool found = false;
            for (size_t k = 0; k < sp.params.size(); ++k)
                for (auto& n : sp.params[k]->names)
                    if (n == fn) { r[k] = a.actual; found = true; }
            if (!found) fail(line, "'" + sp.text + "' has no parameter '" + fn + "'");
        }
        for (size_t k = 0; k < r.size(); ++k)
            if (!r[k]) {
                if (sp.params[k]->init) r[k] = sp.params[k]->init;
                else fail(line, "missing argument '" + sp.params[k]->texts[0] + "' to '" + sp.text + "'");
            }
        return r;
    }

    // Does this subprogram need a per-call specialization?
    bool needsSpecialization(const Sym& s) {
        for (size_t i = 0; i < s.sub->params.size(); ++i) {
            auto& p = s.sub->params[i];
            if (p->cls == "signal" || p->cls == "file") return true;
            TP t = paramType(&s, i);
            if (t && isUnconstrained(t) && !(t->elem && t->elem->k == Type::Char && t->dims.size() == 1)) return true;
        }
        TP rt = retType(&s);
        if (rt && isUnconstrained(rt) && !(rt->elem && rt->elem->k == Type::Char)) return true;
        return false;
    }

    Val userCall(const Sym& s, const std::vector<Assoc>& args, const TP& want, int line) {
        (void)want;
        const Subprog& sp = *s.sub;
        std::vector<EP> ord = orderArgs(sp, args, line);
        std::vector<Val> av(ord.size());
        for (size_t i = 0; i < ord.size(); ++i) {
            TP pt = paramType(&s, i);
            if (sp.params[i]->cls == "signal" || sp.params[i]->cls == "file") av[i] = ex(ord[i], pt);
            else av[i] = ex(ord[i], pt);
        }
        Val v;
        if (needsSpecialization(s)) {
            return specializedCall(s, ord, av, line);
        }
        std::string n = qualSub(s);
        std::string r = n + "(";
        for (size_t i = 0; i < av.size(); ++i) {
            std::string a = av[i].s;
            TP pt = paramType(&s, i);
            if (pt && isScalarBit(pt) && av[i].t && av[i].t->k == Type::Array) a = av[i].s;
            r += (i ? ", " : "") + a;
        }
        v.s = r + ")";
        v.t = retType(&s);
        return v;
    }
    std::string qualSub(const Sym& s) {
        if (!s.home.empty() && s.home != "*" && s.home != cx->container) return svId(s.home) + "::" + s.sv;
        return s.sv;
    }

    // ===================================================================
    // Built-in subprograms (std, ieee)
    // ===================================================================
    Val builtinCall(const std::string& id, const std::vector<Assoc>& args, const TP& want, int line) {
        Val v;
        auto arg = [&](size_t i, const TP& w = nullptr) -> Val {
            if (i >= args.size()) fail(line, id + ": missing argument");
            return ex(args[i].actual, w);
        };
        auto named = [&](const std::string& n) -> EP {
            for (auto& a : args)
                if (a.formal && a.formal->k == EK::Name && a.formal->name == n) return a.actual;
            return nullptr;
        };
        if (id == "rising_edge" || id == "falling_edge") {
            Val s = arg(0);
            requirePrev(s.s);
            std::string p = prevName(s.s);
            if (id == "rising_edge") v.s = "(" + s.s + " === 1'b1 && " + p + " === 1'b0)";
            else v.s = "(" + s.s + " === 1'b0 && " + p + " === 1'b1)";
            v.t = bt["boolean"];
            return v;
        }
        if (id == "now") { v.s = "$realtime"; v.t = bt["time"]; return v; }
        if (id == "to_integer" || id == "conv_integer") {
            Val x = arg(0);
            v.t = bt["integer"];
            if (isSignedVec(x.t) || (id == "conv_integer" && isSignedVec(x.t))) v.s = "int'($signed(" + x.s + "))";
            else if (x.t && x.t->k == Type::Int) v.s = x.s;
            else if (x.t && isScalarBit(x.t)) v.s = "int'(" + x.s + ")";
            else v.s = "int'($unsigned(" + x.s + "))";
            return v;
        }
        if (id == "to_unsigned" || id == "to_signed" || id == "conv_unsigned" || id == "conv_signed" || id == "conv_std_logic_vector" ||
            id == "to_slv" || id == "to_stdlogicvector" || id == "to_stdulogicvector" || id == "to_sulv" || id == "to_bitvector" || id == "to_bv" ||
            id == "resize" || id == "ext" || id == "sxt") {
            Val x = arg(0);
            bool isSz = args.size() > 1;
            std::string w;
            TP basis;
            if (id == "to_unsigned" || id == "conv_unsigned") basis = bt["unsigned"];
            else if (id == "to_signed" || id == "conv_signed") basis = bt["signed"];
            else if (id == "to_bitvector" || id == "to_bv") basis = bt["bit_vector"];
            else if (id == "resize") basis = x.t;
            else basis = bt["std_logic_vector"];
            if (isSz) {
                Val sz = arg(1);
                // resize(x, size_res => y) / to_unsigned(x, y) where y is a vector (2008)
                if (sz.t && sz.t->k == Type::Array) w = widthOf(sz.t);
                else w = sz.s;
            } else if (x.t && x.t->k == Type::Array) w = widthOf(x.t);
            else w = "32";
            bool sgnSrc = isSignedVec(x.t) || id == "sxt" || ((id == "to_signed" || id == "conv_signed") && x.t && x.t->k == Type::Int);
            if (id == "conv_std_logic_vector" && x.t && x.t->k == Type::Int) sgnSrc = true;
            v.t = vecOf(basis ? basis : bt["std_logic_vector"], w, true);
            v.t->flavor = basis ? basis->flavor : "slv";
            if (id == "resize" && isSignedVec(x.t) && !isVec(x.t)) sgnSrc = false;
            std::string src = x.s;
            if (x.t && x.t->k == Type::Array && !isSignedVec(x.t) && sgnSrc) src = "$signed(" + x.s + ")";
            if (id == "resize" && isSignedVec(x.t)) {
                // numeric_std resize of signed keeps the sign bit and the low bits
                v.s = "$signed(" + w + "'($signed(" + x.s + ")))";
                std::string ow = widthOf(x.t);
                v.s = "(" + w + " >= " + ow + " ? $signed(" + w + "'($signed(" + x.s + "))) : $signed({" + x.s + "[$left(" + x.s + ")], " + w + "'(" + x.s + ")} ))";
                if (auto wi = parseInt(w), oi = parseInt(ow); wi && oi) {
                    if (*wi >= *oi) v.s = "$signed(" + w + "'($signed(" + x.s + ")))";
                    else if (*wi == 1) v.s = "$signed(" + x.s + "[$left(" + x.s + ")])";
                    else v.s = "$signed({" + x.s + "[$left(" + x.s + ")], " + std::to_string(*wi - 1) + "'(" + x.s + ")})";
                }
                return v;
            }
            if (isScalarBit(x.t) && x.t->k != Type::Int) src = x.s;
            v.s = w + "'(" + src + ")";
            if (v.t->flavor == "signed") v.s = "$signed(" + v.s + ")";
            if (id == "to_bitvector" || id == "to_bv") v.s = "(" + w + "'(" + x.s + "))";
            return v;
        }
        if (id == "std_logic_vector") {
            Val x = arg(0);
            v.s = x.s;
            v.t = x.t;
            return v;
        }
        if (id == "to_bit") { Val x = arg(0); v.s = "bit'(" + x.s + ")"; v.t = bt["bit"]; return v; }
        if (id == "to_stdulogic") { Val x = arg(0); v.s = "logic'(" + x.s + ")"; v.t = bt["std_ulogic"]; return v; }
        if (id == "to_x01" || id == "to_x01z" || id == "to_ux01") { Val x = arg(0); v = x; return v; }
        if (id == "to_01") {
            Val x = arg(0);
            v.t = x.t;
            v.s = "(($isunknown(" + x.s + ")) ? " + (isVec(x.t) ? typedCast(x.t) + "'(0)" : std::string("1'b0")) + " : " + x.s + ")";
            return v;
        }
        if (id == "is_x") { Val x = arg(0); v.s = "bit'($isunknown(" + x.s + "))"; v.t = bt["boolean"]; return v; }
        if (id == "std_match") {
            Val a = arg(0);
            Val b = ex(args[1].actual, a.t);
            v.s = "vhdl_rt::std_match(" + a.s + ", " + b.s + ", " + widthOf(a.t) + ")";
            if (isVec(a.t)) v.s = "bit'((" + a.s + ") ==? (" + b.s + "))";
            else v.s = "bit'((" + a.s + ") ==? (" + b.s + "))";
            v.t = bt["boolean"];
            return v;
        }
        if (id == "shift_left" || id == "shift_right" || id == "rotate_left" || id == "rotate_right" || id == "shl" || id == "shr") {
            Val a = arg(0), n = arg(1, bt["integer"]);
            if (n.t && n.t->k == Type::Array) n.s = "int'($unsigned(" + n.s + "))";
            std::string op = id == "shift_left" || id == "shl" ? "sll" : id == "rotate_left" ? "rol" : id == "rotate_right" ? "ror" : (isSignedVec(a.t) ? "sra" : "srl");
            Val r = shiftOp(op, a, n);
            if (isSignedVec(a.t)) r.s = "$signed" + r.s;
            return r;
        }
        if (id == "to_string" || id == "to_hstring" || id == "to_ostring" || id == "to_bstring" || id == "to_hex_string" || id == "to_octal_string") {
            Val x = arg(0);
            v.t = bt["string"];
            TP b = baseOf(x.t);
            if (id == "to_string" && x.t && x.t->k == Type::Array && isScalarBit(x.t->elem)) {
                v.s = x.t->elem->k == Type::Logic ? "vhdl_rt::slv_str(" + x.s + ", " + widthOf(x.t) + ")" : "$sformatf(\"%b\", " + x.s + ")";
                return v;
            }
            if (id == "to_hstring" || id == "to_hex_string") { v.s = "vhdl_rt::hstr(" + x.s + ", " + widthOf(x.t) + ", 4)"; return v; }
            if (id == "to_ostring" || id == "to_octal_string") { v.s = "vhdl_rt::hstr(" + x.s + ", " + widthOf(x.t) + ", 3)"; return v; }
            if (id == "to_bstring") { v.s = "vhdl_rt::slv_str(" + x.s + ", " + widthOf(x.t) + ")"; return v; }
            if (b && b->k == Type::Logic) { v.s = "vhdl_rt::sl_str(" + x.s + ")"; return v; }
            if (b && b->k == Type::Char) { v.s = "string'(" + x.s + ")"; return v; }
            if (b && b->k == Type::Real && args.size() > 1) {
                Val f = arg(1);
                if (f.t && f.t->k == Type::Int) v.s = "vhdl_rt::real_digits(" + x.s + ", " + f.s + ")";
                else v.s = "$sformatf(" + f.s + ", " + x.s + ")";
                return v;
            }
            if (b && b->k == Type::Time && args.size() > 1) {
                Val u = arg(1, bt["time"]);
                v.s = "vhdl_rt::time_image_unit(" + x.s + ", " + u.s + ")";
                return v;
            }
            Val r = image(x.t, x);
            if (b && (b->k == Type::Bool || b->k == Type::Bit)) r.s = b->k == Type::Bool ? "((" + x.s + ") ? \"true\" : \"false\")" : "((" + x.s + ") ? \"1\" : \"0\")";
            if (b && b->k == Type::Enum && b->sv != "int") r.s = "vhdl_rt::lower(" + x.s + ".name())";
            return r;
        }
        if (id == "minimum" || id == "maximum") {
            Val a = arg(0);
            Val b = arg(1, a.t);
            std::string o = id == "minimum" ? "<" : ">";
            v.t = a.t;
            if (isVec(a.t)) v.s = "((" + numCmpOperand(a, b) + " " + o + " " + numCmpOperand(b, a) + ") ? " + a.s + " : " + b.s + ")";
            else v.s = "((" + a.s + " " + o + " " + b.s + ") ? " + a.s + " : " + b.s + ")";
            return v;
        }
        // math_real
        static const std::map<std::string, std::string> mr = {{"floor", "$floor"}, {"ceil", "$ceil"}, {"sqrt", "$sqrt"}, {"exp", "$exp"},
                                                               {"log", "$ln"}, {"log10", "$log10"}, {"sin", "$sin"}, {"cos", "$cos"},
                                                               {"tan", "$tan"}, {"arctan", "$atan"}};
        if (mr.count(id)) {
            Val x = arg(0, bt["real"]);
            if (id == "log" && args.size() > 1) { Val b = arg(1, bt["real"]); v.s = "($ln(" + x.s + ") / $ln(" + b.s + "))"; v.t = bt["real"]; return v; }
            v.s = mr.at(id) + "(" + x.s + ")";
            v.t = bt["real"];
            return v;
        }
        if (id == "log2") { Val x = arg(0, bt["real"]); v.s = "($ln(" + x.s + ") / $ln(2.0))"; v.t = bt["real"]; return v; }
        if (id == "round") { Val x = arg(0, bt["real"]); v.s = "vhdl_rt::round(" + x.s + ")"; v.t = bt["real"]; return v; }
        if (id == "trunc") { Val x = arg(0, bt["real"]); v.s = "vhdl_rt::trunc(" + x.s + ")"; v.t = bt["real"]; return v; }
        if (id == "sign") { Val x = arg(0, bt["real"]); v.s = "((" + x.s + ") > 0.0 ? 1.0 : (" + x.s + ") < 0.0 ? -1.0 : 0.0)"; v.t = bt["real"]; return v; }
        if (id == "realmax" || id == "realmin") {
            Val a = arg(0, bt["real"]), b = arg(1, bt["real"]);
            v.s = "((" + a.s + (id == "realmax" ? " > " : " < ") + b.s + ") ? " + a.s + " : " + b.s + ")";
            v.t = bt["real"];
            return v;
        }
        if (id == "to_real") { Val x = arg(0); v.s = "real'(" + x.s + ")"; v.t = bt["real"]; return v; }
        static const std::map<std::string, std::string> redm = {{"and_reduce", "&"}, {"or_reduce", "|"}, {"xor_reduce", "^"},
                                                                {"nand_reduce", "~&"}, {"nor_reduce", "~|"}, {"xnor_reduce", "~^"}};
        if (redm.count(id)) { Val x = arg(0); v.s = "(" + redm.at(id) + x.s + ")"; v.t = bt["std_ulogic"]; return v; }
        if (id == "endfile") { Val f = arg(0); v.s = "bit'($feof(" + f.s + "))"; v.t = bt["boolean"]; return v; }
        if (id == "find_leftmost" || id == "find_rightmost") {
            Val a = arg(0), b = arg(1, bt["std_ulogic"]);
            v.s = "vhdl_rt::" + id + "(" + a.s + ", " + b.s + ", " + widthOf(a.t) + ", " + (a.t->dims[0].downto ? a.t->dims[0].r : a.t->dims[0].l) + ")";
            v.t = bt["integer"];
            return v;
        }
        if (id == "resolution_limit") { v.s = "(1fs)"; v.t = bt["time"]; return v; }
        if (id == "abs") { Val x = arg(0); v.t = x.t; v.s = "((" + x.s + ") < 0 ? -(" + x.s + ") : (" + x.s + "))"; return v; }
        if (id == "unsigned_num_bits") { Val x = arg(0); v.s = "vhdl_rt::num_bits(" + x.s + ")"; v.t = bt["integer"]; return v; }
        (void)named;
        (void)want;
        fail(line, "'" + id + "' is not supported as a function yet");
    }

    // Built-in procedures (textio, env, file I/O, uniform)
    std::string builtinProc(const std::string& id, const std::vector<Assoc>& args, int line) {
        auto arg = [&](size_t i, const TP& w = nullptr) -> Val {
            if (i >= args.size()) fail(line, id + ": missing argument");
            return ex(args[i].actual, w);
        };
        if (id == "stop" || id == "finish") {
            if (id == "stop") return "$stop;";
            return "$finish;";
        }
        if (id == "writeline") {
            Val f = arg(0), l = arg(1);
            return "vhdl_rt::writeline(" + f.s + ", " + l.s + ");";
        }
        if (id == "write" || id == "hwrite" || id == "owrite" || id == "bwrite" || id == "swrite" || id == "fwrite") {
            Val l = arg(0);
            if (l.t && l.t->k == Type::File) {   // write(file, value) on a file of a type
                Val x = arg(1);
                return "$fwrite(" + l.s + ", \"%0d\\n\", " + x.s + ");";
            }
            EP xe = args[1].actual;
            Val x = ex(xe, ambiguous(xe) ? (xe->k == EK::Char ? bt["character"] : bt["string"]) : nullptr);
            std::string just = "0", width = "0";
            if (args.size() > 2) { Val j = arg(2, bt["side"]); just = j.s; }
            if (args.size() > 3) { Val w = arg(3, bt["integer"]); width = w.s; }
            std::string img;
            TP b = baseOf(x.t);
            if (id == "hwrite") img = "vhdl_rt::hstr(" + x.s + ", " + widthOf(x.t) + ", 4)";
            else if (id == "owrite") img = "vhdl_rt::hstr(" + x.s + ", " + widthOf(x.t) + ", 3)";
            else if (b && b->k == Type::Array && isScalarBit(b->elem)) img = b->elem->k == Type::Logic ? "vhdl_rt::slv_str(" + x.s + ", " + widthOf(x.t) + ")" : "$sformatf(\"%b\", " + x.s + ")";
            else if (b && b->k == Type::Logic) img = "vhdl_rt::sl_str(" + x.s + ")";
            else if (b && b->k == Type::Bit) img = "((" + x.s + ") ? \"1\" : \"0\")";
            else if (b && b->k == Type::Char) img = "string'(" + x.s + ")";
            else if (b && b->k == Type::Real && args.size() > 4) { Val d = arg(4, bt["integer"]); img = "vhdl_rt::real_digits(" + x.s + ", " + d.s + ")"; }
            else if (b && b->k == Type::Time && args.size() > 4) { Val u = arg(4, bt["time"]); img = "vhdl_rt::time_image_unit(" + x.s + ", " + u.s + ")"; }
            else if (b && b->k == Type::Time) img = "vhdl_rt::time_image_unit(" + x.s + ", 1ns)";
            else if (b && (b->k == Type::Bool)) img = "((" + x.s + ") ? \"true\" : \"false\")";
            else img = image(x.t, x).s;
            if (b && b->k == Type::Enum && b->sv != "int") img = "vhdl_rt::lower(" + x.s + ".name())";
            return "vhdl_rt::write(" + l.s + ", " + img + ", " + just + ", " + width + ");";
        }
        if (id == "readline") {
            Val f = arg(0), l = arg(1);
            return "vhdl_rt::readline(" + f.s + ", " + l.s + ");";
        }
        if (id == "read" || id == "hread" || id == "oread" || id == "bread" || id == "sread") {
            Val l = arg(0), x = arg(1);
            std::string good = args.size() > 2 ? arg(2).s : "";
            TP b = baseOf(x.t);
            std::string fn;
            std::string extra;
            if (id == "hread" || id == "oread") { fn = "read_hex"; extra = ", " + std::string(id == "hread" ? "4" : "3"); }
            else if (b && b->k == Type::Int) fn = "read_int";
            else if (b && b->k == Type::Real) fn = "read_real";
            else if (b && b->k == Type::Bool) fn = "read_bool";
            else if (b && (b->k == Type::Logic || b->k == Type::Bit)) fn = "read_sl";
            else if (b && b->k == Type::Char) fn = "read_char";
            else if (b && b->k == Type::Time) fn = "read_time";
            else if (b && b->k == Type::Array && isScalarBit(b->elem)) fn = "read_slv";
            else if (b && (b->k == Type::String)) fn = "read_str";
            else fail(line, "read of this type is not supported yet");
            std::string tmp = "vhdl_rt::" + fn;
            if (fn == "read_slv" || fn == "read_hex") {
                return "begin logic [" + widthOf(x.t) + "-1:0] t__; bit ok__; ok__ = vhdl_rt::" + fn + "(" + l.s + ", t__, " + widthOf(x.t) + extra + "); " + x.s + " = t__;" + (good.empty() ? "" : " " + good + " = ok__;") + " end";
            }
            if (fn == "read_str") return "begin bit ok__; ok__ = vhdl_rt::read_str(" + l.s + ", " + x.s + ", " + widthOf(x.t) + ");" + (good.empty() ? "" : " " + good + " = ok__;") + " end";
            return "begin bit ok__; ok__ = " + tmp + "(" + l.s + ", " + x.s + ");" + (good.empty() ? "" : " " + good + " = ok__;") + " end";
        }
        if (id == "file_open") {
            // file_open([status,] f, name, mode)
            size_t k = 0;
            std::string st;
            if (args.size() == 4) { st = arg(0).s; k = 1; }
            Val f = arg(k), n = arg(k + 1, bt["string"]);
            std::string mode = args.size() > k + 2 ? arg(k + 2, bt["file_open_kind"]).s : "0";
            std::string r = f.s + " = vhdl_rt::fopen(" + n.s + ", " + mode + ");";
            if (!st.empty()) r += " " + st + " = (" + f.s + " != 0) ? 0 : 2;";
            return r;
        }
        if (id == "file_close") { Val f = arg(0); return "$fclose(" + f.s + ");"; }
        if (id == "flush") { Val f = arg(0); return "$fflush(" + f.s + ");"; }
        if (id == "deallocate") { Val l = arg(0); return l.s + " = \"\";"; }
        if (id == "uniform") {
            Val s1 = arg(0), s2 = arg(1), x = arg(2);
            return "vhdl_rt::uniform(" + s1.s + ", " + s2.s + ", " + x.s + ");";
        }
        if (id == "tee") { Val f = arg(0), l = arg(1); return "vhdl_rt::writeline(" + f.s + ", " + l.s + ");"; }
        fail(line, "'" + id + "' is not supported as a procedure yet");
    }

    // ===================================================================
    // SystemVerilog runtime support package
    // ===================================================================
    std::string rtPackage() {
        return R"SV(// Generated by msim from VHDL sources: runtime support (src/vhdl/sv.hpp).
package vhdl_rt;
  timeunit )SV" + timeUnit + "; timeprecision " + timePrec + R"SV(;
  int errors = 0;
  function automatic int imod(int a, int b);
    int r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
  endfunction
  function automatic longint smod(longint a, longint b);
    longint r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
  endfunction
  function automatic string lower(string s);
    return s.tolower();
  endfunction
  function automatic string sl_str(logic v);
    case (v)
      1'b0: return "0";
      1'b1: return "1";
      1'bz: return "Z";
      default: return "X";
    endcase
  endfunction
  function automatic int sl_pos(logic v);
    case (v) 1'b0: return 2; 1'b1: return 3; 1'bz: return 4; default: return 1; endcase
  endfunction
  function automatic logic sl_val(int p);
    case (p) 2, 6: return 1'b0; 3, 7: return 1'b1; 4: return 1'bz; default: return 1'bx; endcase
  endfunction
  function automatic string slv_str(logic [4095:0] v, int n);
    string s = "";
    for (int i = n - 1; i >= 0; i--) s = {s, sl_str(v[i])};
    return s;
  endfunction
  function automatic string hstr(logic [4095:0] v, int n, int b);
    string s = "";
    int digits = (n + b - 1) / b;
    for (int d = digits - 1; d >= 0; d--) begin
      logic [3:0] q = 0;
      bit unk = 0, allz = 1;
      for (int k = 0; k < b; k++) begin
        int i = d * b + k;
        logic x = i < n ? v[i] : 1'b0;
        if (x === 1'bx) unk = 1;
        if (x !== 1'bz) allz = 0;
        q[k] = x;
      end
      if (allz) s = {s, "Z"};
      else if (unk || $isunknown(q)) s = {s, "X"};
      else s = {s, $sformatf("%h", q)};
    end
    return s.toupper();
  endfunction
  function automatic string real_image(real r);
    string s;
    if (r == 0.0) return "0.0";
    s = $sformatf("%.15g", r);
    if (s.len() > 0) begin
      bit dot = 0;
      for (int i = 0; i < s.len(); i++) if (s[i] == "." || s[i] == "e") dot = 1;
      if (!dot) s = {s, ".0"};
    end
    return s;
  endfunction
  function automatic string real_digits(real r, int d);
    if (d == 0) return real_image(r);
    return $sformatf($sformatf("%%.%0df", d), r);
  endfunction
  function automatic string time_image(realtime t);
    longint fs = longint'(t * 1.0e6);
    return $sformatf("%0d fs", fs);
  endfunction
  function automatic string time_image_unit(realtime t, realtime u);
    real q = t / u;
    string un;
    if (u == 1fs) un = "fs"; else if (u == 1ps) un = "ps"; else if (u == 1ns) un = "ns";
    else if (u == 1us) un = "us"; else if (u == 1ms) un = "ms"; else if (u == 1s) un = "sec"; else un = "fs";
    if (q == real'(longint'(q))) return $sformatf("%0d %s", longint'(q), un);
    return $sformatf("%s %s", real_image(q), un);
  endfunction
  // @<time> as GHDL prints it in reports
  function automatic string at_time();
    longint fs = longint'($realtime * 1.0e6);
    if (fs == 0) return "0ms";
    if (fs % 64'd1000000000000000 == 0) return $sformatf("%0dsec", fs / 64'd1000000000000000);
    if (fs % 64'd1000000000000 == 0) return $sformatf("%0dms", fs / 64'd1000000000000);
    if (fs % 64'd1000000000 == 0) return $sformatf("%0dus", fs / 64'd1000000000);
    if (fs % 64'd1000000 == 0) return $sformatf("%0dns", fs / 64'd1000000);
    if (fs % 64'd1000 == 0) return $sformatf("%0dps", fs / 64'd1000);
    return $sformatf("%0dfs", fs);
  endfunction
  task automatic report(string msg, int sev, string loc, bit is_assert);
    string sv;
    case (sev) 0: sv = "note"; 1: sv = "warning"; 2: sv = "error"; default: sv = "failure"; endcase
    $display("%s:@%s:(%s %s): %s", loc, at_time(), is_assert ? "assertion" : "report", sv, msg);
    if (sev >= 2) errors++;
    if (sev >= 3) $fatal(0, "VHDL severity failure");
  endtask
  function automatic int int_value(string s);
    int v;
    void'($sscanf(s, "%d", v));
    return v;
  endfunction
  function automatic real real_value(string s);
    real v;
    void'($sscanf(s, "%f", v));
    return v;
  endfunction
  function automatic real round(real x);
    return x >= 0.0 ? $floor(x + 0.5) : $ceil(x - 0.5);
  endfunction
  function automatic real trunc(real x);
    return x >= 0.0 ? $floor(x) : $ceil(x);
  endfunction
  function automatic int num_bits(int x);
    int n = 1;
    while (x > 1) begin n++; x = x / 2; end
    return n;
  endfunction
  function automatic int find_leftmost(logic [4095:0] v, logic y, int n, int lo);
    for (int i = n - 1; i >= 0; i--) if (v[i] ==? y) return lo + i;
    return lo - 1;
  endfunction
  function automatic int find_rightmost(logic [4095:0] v, logic y, int n, int lo);
    for (int i = 0; i < n; i++) if (v[i] ==? y) return lo + i;
    return lo - 1;
  endfunction
  // textio: a line is a string
  task automatic write(inout string l, input string img, input int just, input int width);
    while (img.len() < width) img = just ? {img, " "} : {" ", img};
    l = {l, img};
  endtask
  task automatic writeline(input int f, inout string l);
    if (f == 32'h8000_0001) $display("%s", l);
    else $fdisplay(f, "%s", l);
    l = "";
  endtask
  function automatic int fopen(string name, int mode);
    if (name == "STD_OUTPUT") return 32'h8000_0001;
    return $fopen(name, mode == 0 ? "r" : mode == 1 ? "w" : "a");
  endfunction
  task automatic readline(input int f, output string l);
    string s;
    l = "";
    if (f == 32'h8000_0000) return;
    if ($fgets(s, f) == 0) return;
    while (s.len() > 0 && (s[s.len()-1] == "\n" || s[s.len()-1] == 8'd13)) s = s.substr(0, s.len() - 2);
    l = s;
  endtask
  function automatic void skip_ws(inout string l);
    int i = 0;
    while (i < l.len() && (l[i] == " " || l[i] == "\t")) i++;
    l = l.substr(i, l.len() - 1);
  endfunction
  function automatic string token(inout string l);
    int i = 0;
    string t;
    skip_ws(l);
    while (i < l.len() && l[i] != " " && l[i] != "\t") i++;
    t = l.substr(0, i - 1);
    l = l.substr(i, l.len() - 1);
    return t;
  endfunction
  function automatic bit read_int(inout string l, output int v);
    string t = token(l);
    if (t.len() == 0) return 0;
    return $sscanf(t, "%d", v) == 1;
  endfunction
  function automatic bit read_real(inout string l, output real v);
    string t = token(l);
    if (t.len() == 0) return 0;
    return $sscanf(t, "%f", v) == 1;
  endfunction
  function automatic bit read_bool(inout string l, output bit v);
    string t = token(l);
    v = t.tolower() == "true";
    return t.len() > 0;
  endfunction
  function automatic bit read_char(inout string l, output byte unsigned v);
    if (l.len() == 0) return 0;
    v = l[0];
    l = l.substr(1, l.len() - 1);
    return 1;
  endfunction
  function automatic bit read_sl(inout string l, output logic v);
    skip_ws(l);
    if (l.len() == 0) return 0;
    case (l[0])
      "0", "L", "l": v = 1'b0;
      "1", "H", "h": v = 1'b1;
      "Z", "z": v = 1'bz;
      default: v = 1'bx;
    endcase
    l = l.substr(1, l.len() - 1);
    return 1;
  endfunction
  function automatic bit read_slv(inout string l, output logic [4095:0] v, input int n);
    int got = 0;
    v = '0;
    skip_ws(l);
    while (got < n && l.len() > 0) begin
      logic b;
      if (l[0] == "_") begin l = l.substr(1, l.len() - 1); continue; end
      void'(read_sl(l, b));
      v = {v[4094:0], b};
      got++;
    end
    return got == n;
  endfunction
  function automatic bit read_hex(inout string l, output logic [4095:0] v, input int n, input int b);
    int got = 0;
    v = '0;
    skip_ws(l);
    while (got < n && l.len() > 0) begin
      byte c = l[0];
      logic [3:0] d;
      l = l.substr(1, l.len() - 1);
      if (c == "_") continue;
      if (c >= "0" && c <= "9") d = c - "0";
      else if (c >= "a" && c <= "f") d = c - "a" + 10;
      else if (c >= "A" && c <= "F") d = c - "A" + 10;
      else d = 'x;
      v = (v << b) | d;
      got += b;
    end
    return got >= n;
  endfunction
  function automatic bit read_str(inout string l, output string v, input int n);
    v = l.substr(0, n - 1);
    l = l.len() > n ? l.substr(n, l.len() - 1) : "";
    return v.len() == n;
  endfunction
  function automatic bit read_time(inout string l, output realtime v);
    real x;
    string u;
    if ($sscanf(token(l), "%f", x) != 1) return 0;
    u = token(l);
    case (u.tolower())
      "fs": v = x * 1fs; "ps": v = x * 1ps; "ns": v = x * 1ns; "us": v = x * 1us; "ms": v = x * 1ms; "sec": v = x * 1s;
      default: v = x * 1ns;
    endcase
    return 1;
  endfunction
  // ieee.math_real.uniform: L'Ecuyer's combined generator (CACM 31(6), 1988)
  task automatic uniform(inout int s1, inout int s2, output real x);
    int k, z;
    k = s1 / 53668; s1 = 40014 * (s1 - k * 53668) - k * 12211; if (s1 < 0) s1 += 2147483563;
    k = s2 / 52774; s2 = 40692 * (s2 - k * 52774) - k * 3791; if (s2 < 0) s2 += 2147483399;
    z = s1 - s2; if (z < 1) z += 2147483562;
    x = real'(z) * 4.656613057e-10;
  endtask
endpackage
)SV";
    }

    // ===================================================================
    // Declarations
    // ===================================================================
    // declare the parameters of a subprogram in a scope; fills paramTypes / retTypes
    void subprogramTypes(const Sym& s) {
        if (paramTypes.count(s.sub.get())) return;
        std::vector<TP> pts;
        for (auto& p : s.sub->params)
            for (size_t k = 0; k < p->names.size(); ++k) pts.push_back(subtype(p->type));
        paramTypes[s.sub.get()] = pts;
        if (s.sub->isFunc) retTypes[s.sub.get()] = subtype(s.sub->ret);
    }
    // VHDL lists several names per parameter declaration; split them.
    static void splitParams(Subprog& sp) {
        std::vector<DP> out;
        for (auto& p : sp.params) {
            if (p->names.size() <= 1) { out.push_back(p); continue; }
            for (size_t k = 0; k < p->names.size(); ++k) {
                auto d = std::make_shared<Decl>(*p);
                d->names = {p->names[k]};
                d->texts = {p->texts[k]};
                out.push_back(d);
            }
        }
        sp.params = out;
    }

    std::string subSvName(const Subprog& sp) {
        if (sp.name.size() > 2 && sp.name[0] == '"') {
            static const std::map<std::string, std::string> opn = {{"\"+\"", "op_add"}, {"\"-\"", "op_sub"}, {"\"*\"", "op_mul"},
                                                                   {"\"/\"", "op_div"}, {"\"=\"", "op_eq"}, {"\"/=\"", "op_ne"},
                                                                   {"\"<\"", "op_lt"}, {"\">\"", "op_gt"}, {"\"<=\"", "op_le"},
                                                                   {"\">=\"", "op_ge"}, {"\"and\"", "op_and"}, {"\"or\"", "op_or"},
                                                                   {"\"xor\"", "op_xor"}, {"\"not\"", "op_not"}, {"\"&\"", "op_cat"},
                                                                   {"\"mod\"", "op_mod"}, {"\"rem\"", "op_rem"}, {"\"**\"", "op_pow"}};
            auto it = opn.find(sp.name);
            return (it == opn.end() ? std::string("op") : it->second) + "__" + std::to_string(++uniq);
        }
        return svId(sp.text);
    }

    // emit declarations into `o`; `where`: "package", "module", "process", "function"
    void emitDecls(std::ostringstream& o, const std::vector<DP>& decls, const std::string& where, const Unit* body = nullptr) {
        for (auto& d : decls) emitDecl(o, d, where, body);
    }

    std::map<std::string, std::vector<std::shared_ptr<Subprog>>> pendingBodies;   // package -> bodies
    std::shared_ptr<Subprog> findBody(const std::string& pkg, const Subprog& spec) {
        auto it = pendingBodies.find(pkg);
        if (it == pendingBodies.end()) return nullptr;
        for (auto& b : it->second) {
            if (b->name != spec.name || b->params.size() != spec.params.size() || b->isFunc != spec.isFunc) continue;
            bool same = true;
            for (size_t i = 0; i < b->params.size(); ++i) {
                auto* m1 = b->params[i]->type.mark.get();
                auto* m2 = spec.params[i]->type.mark.get();
                if (m1 && m2 && m1->name != m2->name) same = false;
            }
            if (same) return b;
        }
        return nullptr;
    }

    std::string sigDeclKeyword(const std::string& /*name*/) { return ""; }

    void emitDecl(std::ostringstream& o, const DP& d, const std::string& where, const Unit* body) {
        (void)body;
        cx->file = cx->file;
        switch (d->k) {
        case DK::Use: {
            for (auto& n : d->uses) useClause(n);
            return;
        }
        case DK::Library: case DK::Context: case DK::Attribute: case DK::AttrSpec: case DK::Config: case DK::Group:
        case DK::Disconnect: case DK::Ignored: case DK::PackageInst:
            return;
        case DK::Type: return emitTypeDecl(o, d, where);
        case DK::Subtype: {
            TP t = subtype(d->type);
            TP n = std::make_shared<Type>(*t);
            n->name = d->names[0];
            n->text = d->texts[0];
            if (!n->base && (t->name != n->name)) n->base = t->base ? t->base : (t->name.empty() ? nullptr : t);
            if (n->base == n) n->base = nullptr;
            Sym y;
            y.k = Sym::TypeK;
            y.name = d->names[0];
            y.text = d->texts[0];
            y.type = n;
            y.home = cx->isPackage ? cx->container : "";
            declare(y);
            return;
        }
        case DK::Signal: case DK::Constant: case DK::Variable: return emitObject(o, d, where);
        case DK::File: {
            for (size_t k = 0; k < d->names.size(); ++k) {
                Sym y;
                y.k = Sym::Obj;
                y.cls = "file";
                y.name = d->names[k];
                y.text = d->texts[k];
                y.sv = svId(d->texts[k]);
                y.type = subtype(d->type);
                y.home = cx->isPackage ? cx->container : "";
                y.decl = d;
                declare(y);
                o << ind() << (where == "process" ? "static " : "") << "int " << y.sv;
                if (d->target) {
                    Val n = ex(d->target, bt["string"]);
                    std::string mode = d->openKind ? ex(d->openKind, bt["file_open_kind"]).s : (d->mode == "out" ? "1" : "0");
                    o << " = vhdl_rt::fopen(" << n.s << ", " << mode << ")";
                }
                o << ";\n";
            }
            return;
        }
        case DK::Component: {
            Sym y;
            y.k = Sym::Comp;
            y.name = d->names[0];
            y.text = d->texts[0];
            y.decl = d;
            y.home = cx->isPackage ? cx->container : "";
            declare(y);
            return;
        }
        case DK::Alias: {
            Sym y;
            y.k = Sym::Alias;
            y.name = d->names[0];
            y.text = d->texts[0];
            // alias of a subprogram or type: make the target visible under the new name
            if (d->target->k == EK::Name || d->target->k == EK::Sel) {
                auto* all = d->target->k == EK::Name ? lookupAll(d->target->name) : nullptr;
                const Sym* t = d->target->k == EK::Name ? (all ? &all->front() : nullptr) : lookupSel(d->target);
                if (t && (t->k == Sym::Sub || t->k == Sym::TypeK)) {
                    if (all) for (auto s : *all) { s.name = y.name; declare(s); }
                    else { Sym s = *t; s.name = y.name; declare(s); }
                    return;
                }
            }
            Val v = ex(d->target, nullptr);
            y.aliasSv = v.s;
            y.type = d->type.present() ? subtype(d->type) : v.t;
            if (d->type.present() && isUnconstrained(y.type)) y.type = v.t;
            if (d->type.present() && v.t && v.t->k == Type::Array && y.type->k == Type::Array && !isUnconstrained(y.type)) {
                // alias with its own index range: the SV text keeps the object's range
                if (lenOf(y.type->dims[0]) == lenOf(v.t->dims[0])) {
                    if (y.type->dims[0].l != v.t->dims[0].l || y.type->dims[0].downto != v.t->dims[0].downto) {
                        // renumbered: only equal ranges are supported exactly; keep the object's type
                        y.type = v.t;
                    }
                }
            }
            declare(y);
            return;
        }
        case DK::Subprog: return emitSubprogDecl(o, d, where);
        default: return;
        }
    }

    void useClause(const EP& n) {
        // use lib.pkg.all / lib.pkg.item / pkg.all
        std::vector<std::string> parts;
        for (EP e = n; e; e = e->a) {
            if (e->k == EK::Sel || e->k == EK::Name) parts.insert(parts.begin(), e->name);
            if (e->k == EK::Name) break;
        }
        if (parts.size() < 2) return;
        std::string pkg, item;
        if (parts.size() >= 3) { pkg = parts[1]; item = parts[2]; }
        else if (parts[0] == "work" || packages.count(parts[1])) { pkg = parts[1]; item = "all"; }
        else { pkg = parts[0]; item = parts[1]; }
        if (isBuiltinPkg(pkg)) return;
        if (!packages.count(pkg) && !pkgScope.count(pkg)) {
            if (parts.size() == 2 && !packages.count(pkg)) return;   // use lib.entity: nothing to import
            warn(n->line, "package '" + pkg + "' is not in the design");
            return;
        }
        if (pkg == cx->container) return;
        if (item == "all") {
            if (std::find(cx->usePkgs.begin(), cx->usePkgs.end(), pkg) == cx->usePkgs.end()) cx->usePkgs.push_back(pkg);
        } else cx->useItems[item] = {pkg, item};
    }

    void emitTypeDecl(std::ostringstream& o, const DP& d, const std::string& where) {
        auto& td = *d->tdef;
        auto t = std::make_shared<Type>();
        t->name = d->names[0];
        t->text = d->texts[0];
        t->home = cx->isPackage ? cx->container : "";
        bool hoist = where == "process" || where == "function";
        std::ostringstream& out = hoist && cx->extra ? *cx->extra : o;
        std::string pre = hoist && cx->extra ? std::string("  ") : ind();
        switch (td.k) {
        case TypeDef::Enum: {
            t->k = Type::Enum;
            t->lits = td.lits;
            t->sv = svId(t->text + (hoist ? "__" + std::to_string(++uniq) : ""));
            std::string body;
            std::set<std::string> used;
            for (size_t i = 0; i < td.lits.size(); ++i) {
                std::string ln = td.lits[i];
                std::string svn;
                if (ln[0] == '\'') {
                    char c = td.litText[i][1];
                    svn = t->text + "__c" + std::to_string((unsigned char)c);
                    if (std::isalnum((unsigned char)c)) svn = t->text + "__" + std::string(1, c);
                } else svn = td.litText[i];
                // a literal already used by another enumeration in this scope gets the type name as prefix
                if (litNames.count(lowerStr(svn)) || svKeyword(svn)) svn = t->text + "__" + svn;
                litNames.insert(lowerStr(svn));
                t->litSv.push_back(svId(svn));
                body += (i ? ", " : "") + t->litSv.back();
            }
            out << pre << "typedef enum int {" << body << "} " << t->sv << ";\n";
            Sym ty;
            ty.k = Sym::TypeK;
            ty.name = t->name;
            ty.text = t->text;
            ty.type = t;
            ty.home = t->home;
            declare(ty);
            for (size_t i = 0; i < td.lits.size(); ++i) {
                Sym y;
                y.k = Sym::Lit;
                y.name = td.lits[i];
                y.text = td.litText[i];
                y.type = t;
                y.sv = t->litSv[i];
                y.home = t->home;
                y.lit = (int)i;
                declare(y);
            }
            return;
        }
        case TypeDef::Int: case TypeDef::Real: {
            t->k = td.k == TypeDef::Int ? Type::Int : Type::Real;
            t->range = rangeOf(td.range);
            Sym y;
            y.k = Sym::TypeK;
            y.name = t->name;
            y.text = t->text;
            y.type = t;
            y.home = t->home;
            declare(y);
            return;
        }
        case TypeDef::Phys: {
            // a user physical type: integer count of the base unit
            t->k = Type::Int;
            t->range = rangeOf(td.range);
            Sym y;
            y.k = Sym::TypeK;
            y.name = t->name;
            y.text = t->text;
            y.type = t;
            y.home = t->home;
            declare(y);
            Sym u;
            u.k = Sym::Unit;
            u.name = td.baseUnit;
            u.unitFs = 1;
            u.type = t;
            declare(u);
            Parser::extraUnits().insert(td.baseUnit);
            for (auto& [n, val] : td.units) {
                Sym s2;
                s2.k = Sym::Unit;
                s2.name = n;
                Parser::extraUnits().insert(n);
                int64_t f = 1;
                if (val->k == EK::Phys) {
                    auto* b = lookup(val->name);
                    f = (int64_t)val->rval * (b ? b->unitFs : 1);
                }
                s2.unitFs = f;
                s2.type = t;
                declare(s2);
            }
            return;
        }
        case TypeDef::Array: {
            t->k = Type::Array;
            t->elem = subtype(td.elem);
            for (auto& r : td.index) {
                if (r->k == EK::SubtypeRange && r->b && r->b->k == EK::Box) {
                    Rng g;
                    g.box = true;
                    g.idx = typeOfMark(r->a);
                    t->dims.push_back(g);
                } else {
                    TP it;
                    Rng g = rangeOf(r, &it);
                    g.idx = it ? it : bt["integer"];
                    t->dims.push_back(g);
                }
            }
            if (t->elem && t->elem->k == Type::Char && t->dims.size() == 1) t->flavor = "string";
            else if (isScalarBit(t->elem)) t->flavor = baseOf(t->elem)->k == Type::Logic ? "slv" : "bv";
            Sym y;
            y.k = Sym::TypeK;
            y.name = t->name;
            y.text = t->text;
            y.type = t;
            y.home = t->home;
            declare(y);
            return;
        }
        case TypeDef::Record: {
            t->k = Type::Record;
            t->sv = svId(t->text + (hoist ? "__" + std::to_string(++uniq) : ""));
            std::string fields;
            bool packed = true;
            for (auto& f : td.fields) {
                TP ft = subtype(f->type);
                for (size_t k = 0; k < f->names.size(); ++k) {
                    t->fnames.push_back(f->names[k]);
                    t->ftext.push_back(f->texts[k]);
                    t->ftypes.push_back(ft);
                    if (!isPackable(ft)) packed = false;
                    fields += " " + svDecl(ft, svId(f->texts[k])) + ";";
                }
            }
            out << pre << "typedef struct " << (packed ? "packed " : "") << "{" << fields << " } " << t->sv << ";\n";
            Sym y;
            y.k = Sym::TypeK;
            y.name = t->name;
            y.text = t->text;
            y.type = t;
            y.home = t->home;
            declare(y);
            return;
        }
        case TypeDef::Access: {
            t->k = Type::Access;
            t->target = subtype(td.elem);
            if (t->target->k == Type::String) t->k = Type::Line;
            Sym y;
            y.k = Sym::TypeK;
            y.name = t->name;
            y.text = t->text;
            y.type = t;
            y.home = t->home;
            declare(y);
            return;
        }
        case TypeDef::File: {
            t->k = Type::File;
            t->target = subtype(td.elem);
            Sym y;
            y.k = Sym::TypeK;
            y.name = t->name;
            y.text = t->text;
            y.type = t;
            y.home = t->home;
            declare(y);
            return;
        }
        case TypeDef::Incomplete: return;
        case TypeDef::Protected: case TypeDef::ProtectedBody:
            fail(d->line, "protected types are not supported yet");
        }
    }
    std::set<std::string> litNames;   // enumeration literal SV names used in the current container

    void emitObject(std::ostringstream& o, const DP& d, const std::string& where) {
        TP t = subtype(d->type);
        for (size_t k = 0; k < d->names.size(); ++k) {
            Sym y;
            y.k = Sym::Obj;
            y.cls = d->k == DK::Signal ? "signal" : d->k == DK::Constant ? "constant" : "variable";
            y.name = d->names[k];
            y.text = d->texts[k];
            y.sv = svId(d->texts[k]);
            y.home = cx->isPackage ? cx->container : "";
            y.decl = d;
            TP ot = t;
            std::string init;
            if (d->init) {
                Val iv = ex(d->init, t);
                init = iv.s;
                if (isUnconstrained(t) && iv.t) {
                    // constant c : std_logic_vector := x"AB": the width comes from the value
                    ot = std::make_shared<Type>(*iv.t);
                    ot->flavor = t->flavor;
                    ot->elem = t->elem && !isUnconstrained(t->elem) ? t->elem : iv.t->elem;
                    ot->base = baseOf(t);
                    if (t->k == Type::Array && iv.t->k == Type::Array && !iv.t->dims.empty() && iv.t->dims[0].box) fail(d->line, "cannot infer the range of '" + y.text + "'");
                }
            } else if (d->k == DK::Signal || d->k == DK::Variable) {
                init = defaultInit(t);
                if (t->k == Type::Array && !isScalarBit(t->elem)) {
                    std::string ei = defaultInit(t->elem);
                    if (!ei.empty()) init = "'{default: " + ei + "}";
                }
            }
            y.type = ot;
            if (d->k == DK::Constant && init.empty() && cx->isPackage) {
                // deferred constant: the value is in the package body
                y.deferred = true;
            }
            declare(y);
            if (d->k == DK::Constant && init.empty()) {
                // deferred constant: look for its value in the package body
                if (cx->isPackage) {
                    if (auto b = bodies.find(cx->container); b != bodies.end()) {
                        for (auto& bd : b->second->decls)
                            if (bd->k == DK::Constant && std::find(bd->names.begin(), bd->names.end(), y.name) != bd->names.end() && bd->init) {
                                init = ex(bd->init, ot).s;
                                auto& sv = cx->scopes.back()[y.name].back();
                                sv.decl = bd;
                            }
                    }
                }
                if (init.empty()) { if (where != "function") continue; }
            }
            std::string kw;
            if (d->k == DK::Constant) {
                if (where == "package" || where == "module") kw = "localparam ";
                else kw = where == "process" ? "static const " : "const ";
                if ((where == "package" || where == "module") && (ot->k == Type::String || ot->k == Type::Line)) kw = "localparam ";
            } else if (where == "process") kw = "static ";
            else if (d->k == DK::Variable && d->shared) kw = "";
            std::string dname = y.sv;
            if (d->k == DK::Signal && cx->drvNames.count(dname)) {
                // resolved signal with several drivers: a wire, each driver is a variable
                o << ind() << svDeclNet(ot, dname) << ";\n";
                continue;
            }
            o << ind() << kw << svDecl(ot, dname);
            if (!init.empty()) o << " = " << init;
            o << ";\n";
        }
    }
    std::string svDeclNet(const TP& t, const std::string& name) {
        std::string pre, post;
        svParts(t, pre, post);
        if (pre.rfind("logic", 0) == 0) pre = "wire " + pre;
        else pre = "wire logic";
        return pre + " " + name + post;
    }

    // ------------------------------------------------------------ subprograms
    void emitSubprogDecl(std::ostringstream& o, const DP& d, const std::string& where) {
        auto sp = d->sub;
        splitParams(*sp);
        Sym y;
        y.k = Sym::Sub;
        y.name = sp->name;
        y.text = sp->text;
        y.home = cx->isPackage ? cx->container : "";
        // a body for a declaration made earlier in this scope (package body / later in a region)?
        if (sp->hasBody) {
            auto& vec = cx->scopes.back()[sp->name];
            for (auto& s : vec)
                if (s.k == Sym::Sub && s.sub && !s.sub->hasBody && s.sub->params.size() == sp->params.size() && s.sub->isFunc == sp->isFunc) {
                    // complete the earlier declaration
                    *s.sub = *sp;
                    if (!needsSpecialization(s)) emitSubprog(o, s, where);
                    return;
                }
        }
        y.sub = sp;
        y.sv = subSvName(*sp) + (where == "process" ? "__" + std::to_string(++uniq) : "");
        if (!sp->hasBody && cx->isPackage) {
            if (auto b = findBody(cx->container, *sp)) {
                *sp = *b;
                splitParams(*sp);
            }
        }
        declare(y);
        Sym& stored = cx->scopes.back()[sp->name].back();
        subprogramTypes(stored);
        if (!sp->hasBody) return;
        if (needsSpecialization(stored)) return;   // emitted per call site
        emitSubprog(where == "process" && cx->extra ? *cx->extra : o, stored, where);
    }

    // The SV text of a subprogram. `subst`: specialization (parameter -> fixed type / actual)
    struct Spec {
        std::vector<TP> types;            // concrete parameter types
        std::vector<std::string> actual;  // signal/file parameters: the actual's SV text ("" otherwise)
    };
    void emitSubprog(std::ostringstream& o, const Sym& s, const std::string& where, const Spec* spec = nullptr, const std::string& svName = "") {
        const Subprog& sp = *s.sub;
        auto& pts = paramTypes[s.sub.get()];
        Ctx sub = *cx;
        sub.scopes.push_back(Scope());
        sub.inFunc = true;
        sub.proc = nullptr;
        std::vector<Loop> loops;
        sub.loops = &loops;
        sub.indent = 2;
        sub.retSeen = nullptr;
        Ctx* saved = cx;
        cx = &sub;
        std::ostringstream body, head;
        std::vector<std::string> formals;
        bool hasWait = containsWait(sp.body) || callsProcWithWait(sp.body);
        try {
            for (size_t i = 0; i < sp.params.size(); ++i) {
                auto& p = sp.params[i];
                TP pt = spec ? spec->types[i] : pts[i];
                Sym y;
                y.k = Sym::Obj;
                y.cls = p->cls == "signal" ? "signal" : p->cls == "file" ? "file" : p->cls == "variable" ? "variable" : "constant";
                y.name = p->names[0];
                y.text = p->texts[0];
                y.type = pt;
                if (spec && !spec->actual[i].empty()) {
                    // signal parameter: the body uses the actual signal itself
                    y.sv = spec->actual[i];
                    y.home = "*";
                    declare(y);
                    continue;
                }
                y.sv = svId(p->texts[0]);
                declare(y);
                std::string dir = p->mode == "out" ? "output" : p->mode == "inout" ? (sp.isFunc ? "inout" : "inout") : "input";
                if (pt && pt->k == Type::Line) dir = p->mode == "in" ? "input" : "inout";
                formals.push_back(dir + " " + svDecl(pt, y.sv));
            }
            TP rt = sp.isFunc ? (spec && !retTypes[s.sub.get()] ? nullptr : retTypes[s.sub.get()]) : nullptr;
            cx->retType = rt;
            std::ostringstream decls;
            emitDecls(decls, sp.decls, "function");
            std::string stmts = seqStmts(sp.body);
            body << decls.str() << stmts;
            if (sp.isFunc && rt && isUnconstrained(rt)) rt = cx->retSeen;
            if (sp.isFunc && !rt) rt = cx->retSeen ? cx->retSeen : bt["integer"];
            std::string name = svName.empty() ? s.sv : svName;
            if (sp.isFunc) {
                head << "  function automatic " << svRetType(rt) << " " << name << "(";
                retTypes[s.sub.get()] = retTypes[s.sub.get()] ? retTypes[s.sub.get()] : rt;
                if (spec) specRet = rt;
            } else head << "  task automatic " << name << "(";
            (void)hasWait;
            for (size_t i = 0; i < formals.size(); ++i) head << (i ? ", " : "") << formals[i];
            head << ");\n";
        } catch (...) {
            cx = saved;
            throw;
        }
        cx = saved;
        o << lineDir(sp.line) << head.str() << body.str() << (sp.isFunc ? "  endfunction\n" : "  endtask\n");
        (void)where;
    }
    TP specRet;
    std::string svRetType(const TP& t) {
        std::string pre, post;
        svParts(t, pre, post);
        if (!post.empty()) {
            // unpacked return: SystemVerilog needs a typedef; use a packed form when possible
            fail(0, "functions returning unpacked arrays are not supported yet");
        }
        return pre;
    }
    static bool containsWait(const std::vector<SP>& b) {
        for (auto& s : b) {
            if (s->k == SK::Wait) return true;
            for (auto& a : s->alts) if (containsWait(a.body)) return true;
            if (containsWait(s->body)) return true;
        }
        return false;
    }
    bool callsProcWithWait(const std::vector<SP>&) { return false; }

    std::map<std::string, TP> specRetTypes;
    Val specializedCall(const Sym& s, const std::vector<EP>& ord, const std::vector<Val>& av, int line) {
        (void)line;
        const Subprog& sp = *s.sub;
        Spec spec;
        std::string key = s.sv + "|" + std::to_string((uintptr_t)s.sub.get());
        auto& pts = paramTypes[s.sub.get()];
        for (size_t i = 0; i < sp.params.size(); ++i) {
            TP pt = pts[i];
            std::string act;
            if (sp.params[i]->cls == "signal" || sp.params[i]->cls == "file") {
                act = av[i].s;
                if (isUnconstrained(pt) || !pt) pt = av[i].t;
            } else if (isUnconstrained(pt) && !(pt->elem && pt->elem->k == Type::Char && pt->dims.size() == 1)) {
                TP at = av[i].t;
                if (!at || at->k != Type::Array || isUnconstrained(at)) {
                    // an aggregate or literal of unknown range: from the value's width
                    at = at ? at : pt;
                }
                TP n = std::make_shared<Type>(*pt);
                n->dims = at && at->k == Type::Array ? at->dims : pt->dims;
                if (at && at->k == Type::Array && n->dims.size() == 1 && !at->dims.empty() && !at->dims[0].box && at->dims[0].idx && pt->dims[0].idx &&
                    baseOf(at->dims[0].idx)->k != baseOf(pt->dims[0].idx)->k)
                    n->dims[0].idx = pt->dims[0].idx;
                if (n->elem && isUnconstrained(n->elem) && at && at->elem) n->elem = at->elem;
                pt = n;
            }
            spec.types.push_back(pt);
            spec.actual.push_back(act);
            std::string pre, post;
            svParts(pt, pre, post);
            key += "|" + pre + post + "|" + act;
        }
        std::string name;
        auto& done = *cx->specDone;
        auto it = specNames.find(key);
        if (it == specNames.end() || !done.count(key)) {
            name = (it == specNames.end()) ? s.sv + "__s" + std::to_string(++uniq) : it->second;
            specNames[key] = name;
            done.insert(key);
            std::ostringstream fo;
            TP keepRet = retTypes[s.sub.get()];
            if (keepRet && isUnconstrained(keepRet)) retTypes[s.sub.get()] = nullptr;
            specRet = nullptr;
            emitSubprog(fo, s, "module", &spec, name);
            specRetTypes[key] = sp.isFunc ? (specRet ? specRet : retTypes[s.sub.get()]) : nullptr;
            retTypes[s.sub.get()] = keepRet;
            *cx->extra << fo.str();
        } else name = it->second;
        Val v;
        std::string r = name + "(";
        bool first = true;
        for (size_t i = 0; i < av.size(); ++i) {
            if (!spec.actual[i].empty()) continue;
            r += (first ? "" : ", ") + av[i].s;
            first = false;
        }
        v.s = r + ")";
        v.t = specRetTypes[key];
        (void)ord;
        return v;
    }
    std::map<std::string, std::string> specNames;

    // ===================================================================
    // Sequential statements
    // ===================================================================
    std::string seqStmts(const std::vector<SP>& b) {
        std::string r;
        for (auto& s : b) r += seqStmt(*s);
        return r;
    }

    std::string target(const EP& t, bool isSignal) {
        if (t->k == EK::Agg) {
            std::string s = "{";
            for (size_t i = 0; i < t->agg.size(); ++i) s += (i ? ", " : "") + target(t->agg[i].value, isSignal);
            return s + "}";
        }
        Val v = ex(t, nullptr);
        if (isSignal && cx->proc) {
            // the process drives the signal through its own variable
            std::string base = v.s;
            for (auto& [sig, drv] : cx->proc->drv)
                if (base == sig || base.rfind(sig + "[", 0) == 0 || base.rfind(sig + ".", 0) == 0) return drv + base.substr(sig.size());
        }
        return v.s;
    }
    TP targetType(const EP& t) {
        if (t->k == EK::Agg) {
            std::string w = "0";
            for (auto& a : t->agg) { TP x = targetType(a.value); w = addText(w, x && x->k == Type::Array ? widthOf(x) : "1"); }
            return vecOf(bt["std_logic_vector"], w);
        }
        return ex(t, nullptr).t;
    }

    std::string assignRhs(const EP& val, const TP& tt) {
        Val v = ex(val, tt);
        return fitTo(v, tt);
    }
    // adapt a value to the target type where SystemVerilog would not do the same thing
    std::string fitTo(const Val& v, const TP& tt) {
        if (!tt || !v.t) return v.s;
        TP a = baseOf(tt), b = baseOf(v.t);
        if (a->k == Type::Bool && b->k == Type::Logic) return v.s;
        if (a->k == Type::Real && b->k == Type::Int) return "real'(" + v.s + ")";
        return v.s;
    }

    std::string seqStmt(const Stmt& s) {
        std::string r;
        std::string I = ind();
        switch (s.k) {
        case SK::VarAssign: {
            TP tt = targetType(s.target);
            r = I + target(s.target, false) + " = " + assignRhs(s.value, tt) + ";\n";
            break;
        }
        case SK::SigAssign: {
            TP tt = targetType(s.target);
            std::string tg = target(s.target, true);
            if (s.waves.empty()) break;
            for (auto& w : s.waves) {
                if (w.val->k == EK::Null) continue;
                std::string d = w.after ? "#(" + ex(w.after, bt["time"]).s + ") " : "";
                r += I + tg + " <= " + d + assignRhs(w.val, tt) + ";\n";
            }
            break;
        }
        case SK::CondSigAssign: case SK::CondVarAssign: {
            bool sig = s.k == SK::CondSigAssign;
            TP tt = targetType(s.target);
            std::string tg = target(s.target, sig);
            for (size_t i = 0; i < s.alts.size(); ++i) {
                auto& a = s.alts[i];
                std::string body;
                for (auto& w : a.waves) {
                    std::string d = w.after ? "#(" + ex(w.after, bt["time"]).s + ") " : "";
                    body += tg + (sig ? " <= " + d : " = ") + assignRhs(w.val, tt) + "; ";
                }
                if (a.cond) r += I + (i ? "else if (" : "if (") + boolOf(ex(a.cond, bt["boolean"])).s + ") begin " + body + "end\n";
                else r += I + "else begin " + body + "end\n";
            }
            break;
        }
        case SK::SelSigAssign: case SK::SelVarAssign: {
            bool sig = s.k == SK::SelSigAssign;
            TP tt = targetType(s.target);
            std::string tg = target(s.target, sig);
            Val sel = ex(s.value, nullptr);
            r += I + caseKw(s, sel) + " (" + sel.s + ")\n";
            for (auto& a : s.alts) {
                std::string body;
                for (auto& w : a.waves) {
                    std::string d = w.after ? "#(" + ex(w.after, bt["time"]).s + ") " : "";
                    body += tg + (sig ? " <= " + d : " = ") + assignRhs(w.val, tt) + "; ";
                }
                r += I + "  " + choiceList(a.choices, sel.t, s.matching) + ": begin " + body + "end\n";
            }
            r += I + "endcase\n";
            break;
        }
        case SK::If: {
            for (size_t i = 0; i < s.alts.size(); ++i) {
                auto& a = s.alts[i];
                if (a.cond) r += I + (i ? "end else if (" : "if (") + boolOf(ex(a.cond, bt["boolean"])).s + ") begin\n";
                else r += I + "end else begin\n";
                cx->indent++;
                r += seqStmts(a.body);
                cx->indent--;
            }
            r += I + "end\n";
            break;
        }
        case SK::Case: {
            Val sel = ex(s.value, nullptr);
            r += I + caseKw(s, sel) + " (" + sel.s + ")\n";
            for (auto& a : s.alts) {
                r += I + "  " + choiceList(a.choices, sel.t, s.matching) + ": begin\n";
                cx->indent += 2;
                r += seqStmts(a.body);
                cx->indent -= 2;
                r += I + "  end\n";
            }
            r += I + "endcase\n";
            break;
        }
        case SK::Loop: r = loopStmt(s); break;
        case SK::Next: case SK::Exit: {
            std::string what = s.k == SK::Next ? "continue" : "break";
            if (!s.loopLabel.empty() && cx->loops && !cx->loops->empty() && cx->loops->back().label != s.loopLabel) {
                // to an outer loop: disable the named block around it (or its body)
                for (auto it = cx->loops->rbegin(); it != cx->loops->rend(); ++it)
                    if (it->label == s.loopLabel) {
                        if (s.k == SK::Exit) { it->exitUsed = true; what = "disable " + it->exitBlk; }
                        else { it->nextUsed = true; what = "disable " + it->nextBlk; }
                        break;
                    }
            }
            if (s.cond) r = I + "if (" + boolOf(ex(s.cond, bt["boolean"])).s + ") " + what + ";\n";
            else r = I + what + ";\n";
            break;
        }
        case SK::Return: {
            if (!s.value) { r = I + "return;\n"; break; }
            Val v = ex(s.value, cx->retType && !isUnconstrained(cx->retType) ? cx->retType : (cx->retType && cx->retType->k == Type::Array ? cx->retType : cx->retType));
            if (!cx->retSeen && v.t) cx->retSeen = v.t;
            r = I + "return " + fitTo(v, cx->retType) + ";\n";
            break;
        }
        case SK::Null: r = I + ";\n"; break;
        case SK::Wait: r = waitStmt(s); break;
        case SK::Assert: {
            Val c = boolOf(ex(s.cond, bt["boolean"]));
            r = I + "if (!" + c.s + ") " + reportCall(s.report, s.severity, s.line, true, "2", s.col) + "\n";
            break;
        }
        case SK::Report: r = I + reportCall(s.report, s.severity, s.line, false, "0", s.col) + "\n"; break;
        case SK::Call: r = I + procCall(s.call, s.line) + "\n"; break;
        case SK::Force: {
            Val v = ex(s.value, targetType(s.target));
            r = I + "force " + ex(s.target, nullptr).s + " = " + v.s + ";\n";
            break;
        }
        case SK::Release: r = I + "release " + ex(s.target, nullptr).s + ";\n"; break;
        default: fail(s.line, "statement not allowed here");
        }
        if (!s.label.empty() && s.k != SK::Loop) return lineDir(s.line) + r;
        return lineDir(s.line) + r;
    }

    std::string caseKw(const Stmt& s, const Val& sel) {
        bool ranges = false;
        for (auto& a : s.alts)
            for (auto& c : a.choices)
                if (c->k == EK::Range || c->k == EK::SubtypeRange || (c->k == EK::Attr && c->name == "range") || (isTypeName(c) && !(sel.t && sel.t->k == Type::Enum))) ranges = true;
        if (s.matching) return "casez";
        return ranges ? "case" : "case";
    }
    std::string choiceList(const std::vector<EP>& cs, const TP& selT, bool matching) {
        std::string r;
        bool def = false;
        std::vector<std::string> items;
        for (auto& c : cs) {
            if (c->k == EK::Others) { def = true; continue; }
            if (c->k == EK::Range || c->k == EK::SubtypeRange || (c->k == EK::Attr && c->name == "range") || isTypeName(c)) {
                Rng g = rangeOf(c);
                if (g.lv && g.rv) {
                    int64_t lo = std::min(*g.lv, *g.rv), hi = std::max(*g.lv, *g.rv);
                    if (hi - lo <= 1024) {
                        for (int64_t i = lo; i <= hi; ++i) items.push_back(std::to_string(i));
                        continue;
                    }
                }
                // enumeration subrange or large integer range: list the values via inside
                items.push_back("[" + (g.downto ? g.r : g.l) + ":" + (g.downto ? g.l : g.r) + "]");
                continue;
            }
            Val v = ex(c, selT);
            if (matching) {
                std::string s = v.s;
                for (auto& ch : s) if (ch == 'x' && s.find("'b") != std::string::npos && &ch > &s[s.find("'b")]) ch = '?';
                items.push_back(s);
                continue;
            }
            items.push_back(v.s);
        }
        if (def && items.empty()) return "default";
        for (size_t i = 0; i < items.size(); ++i) r += (i ? ", " : "") + items[i];
        if (def) r = "default";
        // ranges with brackets need "case inside"; written as a list instead
        for (auto& it : items)
            if (!it.empty() && it[0] == '[') {
                // expand to an inside-free form: (sel >= lo && sel <= hi) cannot be a case item, so use a range compare via inside
                return r;
            }
        return r;
    }

    std::string loopStmt(const Stmt& s) {
        std::string I = ind();
        std::string r;
        Loop lp;
        lp.label = lowerStr(s.label);
        lp.exitBlk = s.label.empty() ? "" : svId(s.label + "__exit");
        lp.nextBlk = s.label.empty() ? "" : svId(s.label + "__next");
        std::vector<Loop> local;
        if (!cx->loops) cx->loops = &local;
        cx->loops->push_back(lp);
        std::string head;
        cx->scopes.push_back(Scope());
        if (s.loop == Stmt::For) {
            TP it;
            Rng g = rangeOf(s.range, &it);
            Sym y;
            y.k = Sym::Obj;
            y.cls = "constant";
            y.name = lowerStr(s.var);
            y.text = s.var;
            y.sv = svId(s.var);
            y.type = it && it->k == Type::Enum ? it : bt["integer"];
            if (it && (it->k == Type::Logic || it->k == Type::Char || it->k == Type::Bool)) y.type = it;
            declare(y);
            if (y.type->k == Type::Enum && y.type->sv != "int") {
                std::string iv = svId(s.var + "__i");
                head = "for (int " + iv + " = int'(" + g.l + "); " + iv + (g.downto ? " >= " : " <= ") + "int'(" + g.r + "); " + iv + (g.downto ? "--" : "++") + ") begin\n" +
                       I + "  automatic " + typeName(y.type) + " " + y.sv + " = " + typeName(y.type) + "'(" + iv + ");\n";
            } else
                head = "for (int " + y.sv + " = " + g.l + "; " + y.sv + (g.downto ? " >= " : " <= ") + g.r + "; " + y.sv + (g.downto ? "--" : "++") + ") begin\n";
        } else if (s.loop == Stmt::While) {
            head = "while (" + boolOf(ex(s.cond, bt["boolean"])).s + ") begin\n";
        } else head = "forever begin\n";
        cx->indent++;
        std::string body = seqStmts(s.body);
        cx->indent--;
        cx->scopes.pop_back();
        Loop done = cx->loops->back();
        cx->loops->pop_back();
        if (cx->loops == &local) cx->loops = nullptr;
        if (done.nextUsed) body = I + "  begin : " + done.nextBlk + "\n" + body + I + "  end\n";
        r = I + head + body + I + "end\n";
        if (done.exitUsed) r = I + "begin : " + done.exitBlk + "\n" + r + I + "end\n";
        return r;
    }

    // signals read by an expression (for implicit sensitivity)
    void readSignals(const EP& e, std::vector<std::string>& out) {
        if (!e) return;
        switch (e->k) {
        case EK::Name: {
            const Sym* y = lookup(e->name);
            if (y && y->k == Sym::Obj && y->cls == "signal") {
                Val v = ex(e, nullptr);
                if (v.t && (isPackable(v.t) || v.t->k == Type::Real || v.t->k == Type::Time)) addUnique(out, v.s);
                else if (v.t && v.t->k == Type::Array) addUnique(out, v.s);
            } else if (y && y->k == Sym::Alias) {
                addUnique(out, y->aliasSv);
            }
            return;
        }
        case EK::Sel: {
            if (lookupSel(e)) return;
            readSignals(e->a, out);
            return;
        }
        case EK::Call: {
            // index of an unpacked array signal: the element as the event
            if (e->a->k == EK::Name) {
                const Sym* y = lookup(e->a->name);
                if (y && y->k == Sym::Obj && y->cls == "signal" && y->type && y->type->k == Type::Array && !isScalarBit(y->type->elem)) {
                    for (auto& a : e->args) readSignals(a.actual, out);
                    Val v = exQuiet(e);
                    if (!v.s.empty()) addUnique(out, v.s);
                    return;
                }
            }
            readSignals(e->a, out);
            for (auto& a : e->args) readSignals(a.actual, out);
            return;
        }
        case EK::Attr:
            readSignals(e->a, out);
            readSignals(e->b, out);
            return;
        case EK::Agg:
            for (auto& a : e->agg) { readSignals(a.value, out); for (auto& c : a.choices) readSignals(c, out); }
            return;
        case EK::Range: case EK::Bin: case EK::Un: case EK::Qual:
            readSignals(e->a, out);
            readSignals(e->b, out);
            return;
        default: return;
        }
    }
    static void addUnique(std::vector<std::string>& v, const std::string& s) {
        if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
    }
    void readSignalsStmts(const std::vector<SP>& b, std::vector<std::string>& out) {
        for (auto& s : b) {
            readSignals(s->value, out);
            readSignals(s->cond, out);
            readSignals(s->call, out);
            readSignals(s->report, out);
            for (auto& w : s->waves) readSignals(w.val, out);
            if (s->target && (s->k == SK::SigAssign || s->k == SK::VarAssign || s->k == SK::CondSigAssign || s->k == SK::SelSigAssign)) {
                // indices of the target are read
                if (s->target->k == EK::Call) for (auto& a : s->target->args) readSignals(a.actual, out);
            }
            for (auto& a : s->alts) {
                readSignals(a.cond, out);
                for (auto& w : a.waves) readSignals(w.val, out);
                readSignalsStmts(a.body, out);
            }
            if (s->k == SK::Loop) readSignals(s->range, out);
            readSignalsStmts(s->body, out);
        }
    }
    std::string eventList(const std::vector<std::string>& sigs) {
        std::string r;
        for (size_t i = 0; i < sigs.size(); ++i) r += (i ? " or " : "") + sigs[i];
        return r;
    }

    std::string prevUpdate() {
        if (!cx->proc || cx->proc->prev.empty()) return "";
        return "@@PREV@@";   // filled in when the process is complete
    }

    std::string waitStmt(const Stmt& s) {
        std::string I = ind();
        if (!cx->proc && !cx->inFunc) fail(s.line, "wait outside a process");
        std::string pu = prevUpdate();
        // wait until rising_edge(clk) / falling_edge(clk) / clk = '1' with clk'event
        if (s.cond && s.on.empty() && !s.forTime) {
            std::string edge = edgeOf(s.cond);
            if (!edge.empty()) return I + pu + "@(" + edge + ");\n";
        }
        std::vector<std::string> sigs;
        for (auto& n : s.on) {
            Val v = ex(n, nullptr);
            addUnique(sigs, v.s);
        }
        if (s.on.empty() && s.cond) readSignals(s.cond, sigs);
        std::string tm = s.forTime ? ex(s.forTime, bt["time"]).s : "";
        if (!s.cond && sigs.empty() && tm.empty()) return I + pu + "wait (0);\n";   // wait; : forever
        if (!s.cond && sigs.empty()) return I + pu + "#(" + tm + ");\n";
        std::string cond = s.cond ? boolOf(ex(s.cond, bt["boolean"])).s : "";
        if (tm.empty()) {
            if (!s.cond) return I + pu + "@(" + eventList(sigs) + ");\n";
            if (sigs.empty()) return I + "wait (0);\n";
            return I + "do begin " + pu + "@(" + eventList(sigs) + "); end while (!" + cond + ");\n";
        }
        // with a timeout
        std::string r = I + "begin : " + svId("wait__" + std::to_string(++uniq)) + "\n";
        std::string blk = "wait__" + std::to_string(uniq);
        r += I + "  fork\n";
        r += I + "    #(" + tm + ");\n";
        if (!sigs.empty()) {
            if (s.cond) r += I + "    do @(" + eventList(sigs) + "); while (!" + cond + ");\n";
            else r += I + "    @(" + eventList(sigs) + ");\n";
        }
        r += I + "  join_any\n";
        r += I + "  disable fork;\n";
        r += I + "end\n";
        if (!pu.empty()) r = I + pu + "\n" + r;
        return r;
    }
    // clock edge of a condition: rising_edge(c), falling_edge(c), c'event and c = '1', c = '1' (wait until)
    std::string edgeOf(const EP& c0) {
        EP c = c0;
        while (c->k == EK::Un && c->op == "()") c = c->a;
        if (c->k == EK::Call && c->a->k == EK::Name && (c->a->name == "rising_edge" || c->a->name == "falling_edge") && c->args.size() == 1) {
            const Sym* y = lookup(c->a->name);
            if (y && y->k == Sym::Sub && !y->builtin.empty()) {
                Val s = ex(c->args[0].actual, nullptr);
                return (c->a->name == "rising_edge" ? "posedge " : "negedge ") + s.s;
            }
        }
        if (c->k == EK::Bin && c->op == "and") {
            EP a = c->a, b = c->b;
            if (a->k == EK::Attr && a->name == "event") std::swap(a, b);
            if (b->k == EK::Attr && b->name == "event" && a->k == EK::Bin && a->op == "=" && a->b->k == EK::Char) {
                Val s = ex(b->a, nullptr), s2 = ex(a->a, nullptr);
                if (s.s == s2.s) return (a->b->text == "1" ? "posedge " : "negedge ") + s.s;
            }
        }
        return "";
    }

    std::string reportCall(const EP& msg, const EP& sev, int line, bool isAssert, const std::string& defSev, int col = 0) {
        std::string m = msg ? ex(msg, bt["string"]).s : std::string(isAssert ? "\"Assertion violation.\"" : "\"\"");
        if (msg) {
            Val mv = ex(msg, bt["string"]);
            TP b = baseOf(mv.t);
            m = mv.s;
            if (b && b->k == Type::Array && isScalarBit(b->elem)) m = "\"" + std::string() + "\"";
        }
        std::string sv = sev ? ex(sev, bt["severity_level"]).s : defSev;
        std::string loc = "\"" + cx->file + ":" + std::to_string(line) + (col ? ":" + std::to_string(col) : std::string()) + "\"";
        return "vhdl_rt::report(" + m + ", " + sv + ", " + loc + ", " + (isAssert ? "1" : "0") + ");";
    }

    std::string procCall(const EP& c, int line) {
        EP n = c->k == EK::Call ? c->a : c;
        std::vector<Assoc> args = c->k == EK::Call ? c->args : std::vector<Assoc>();
        const std::vector<Sym>* syms = nullptr;
        if (n->k == EK::Name) syms = lookupAll(n->name);
        else if (n->k == EK::Sel) {
            if (lookupSel(n)) {
                auto it = pkgScope.find(n->a->name);
                if (it != pkgScope.end() && it->second.count(n->name)) syms = &it->second[n->name];
                else if (builtinScope.count(n->name)) syms = &builtinScope[n->name];
            }
        }
        if (!syms || syms->empty()) fail(line, "unknown procedure '" + (n->text.empty() ? n->name : n->text) + "'");
        std::vector<Sym> procs;
        for (auto& s : *syms)
            if (s.k == Sym::Sub && (!s.sub || !s.sub->isFunc)) procs.push_back(s);
        if (procs.empty()) fail(line, "'" + n->text + "' is not a procedure");
        const Sym* s = pickOverload(procs, args, nullptr, line);
        if (!s->sub) return builtinProc(s->builtin, args, line);
        Val v = userCall(*s, args, nullptr, line);
        return v.s + ";";
    }

    // ===================================================================
    // Design entities
    // ===================================================================
    struct PortInfo {
        std::string name, text, mode;
        TP type;
    };

    std::string emitPackage(const UP& u) {
        Ctx c;
        c.container = u->name;
        c.isPackage = true;
        c.scopes.push_back(Scope());
        c.file = u->file;
        std::ostringstream extra;
        std::set<std::string> specDone;
        c.extra = &extra;
        c.specDone = &specDone;
        Ctx* saved = cx;
        cx = &c;
        litNames.clear();
        std::ostringstream o;
        try {
            for (auto& d : allCtx(*u))
                for (auto& n : d->uses) useClause(n);
            if (u->k == Unit::PackageInst) fail(u->line, "package instantiation is not supported yet");
            if (!u->generics.empty()) fail(u->line, "generic packages are not supported yet");
            // the package body's subprograms complete the declarations
            if (auto b = bodies.find(u->name); b != bodies.end()) {
                for (auto& d : b->second->decls)
                    if (d->k == DK::Subprog) { splitParams(*d->sub); pendingBodies[u->name].push_back(d->sub); }
                for (auto& d : allCtx(*b->second))
                    for (auto& n : d->uses) useClause(n);
            }
            o << lineDir(u->line) << "package " << svId(u->text) << ";\n";
            o << "  timeunit " << timeUnit << "; timeprecision " << timePrec << ";\n";
            emitDecls(o, u->decls, "package");
            // body-only declarations (constants, helper subprograms) after the visible part
            if (auto b = bodies.find(u->name); b != bodies.end()) {
                c.file = b->second->file;
                for (auto& d : b->second->decls) {
                    if (d->k == DK::Subprog) {
                        // already emitted with its declaration?
                        bool declared = false;
                        auto it = cx->scopes.back().find(d->sub->name);
                        if (it != cx->scopes.back().end())
                            for (auto& s : it->second)
                                if (s.sub && s.sub->hasBody && s.sub->body.size() == d->sub->body.size() && s.sub->line == d->sub->line) declared = true;
                        if (declared) continue;
                        emitDecl(o, d, "package", nullptr);
                        continue;
                    }
                    if (d->k == DK::Constant) {
                        bool deferred = false;
                        auto it = cx->scopes.back().find(d->names[0]);
                        if (it != cx->scopes.back().end()) deferred = true;
                        if (deferred) continue;
                    }
                    emitDecl(o, d, "package", nullptr);
                }
            }
            o << extra.str();
            o << "endpackage\n";
        } catch (...) {
            cx = saved;
            throw;
        }
        pkgScope[u->name] = c.scopes.front();
        cx = saved;
        return o.str();
    }

    UP archOf(const std::string& entity, const std::string& want = "") {
        auto it = archs.find(entity);
        if (it == archs.end() || it->second.empty()) return nullptr;
        if (!want.empty())
            for (auto& a : it->second)
                if (a->name == want) return a;
        return it->second.back();   // the most recently analyzed one
    }

    // ports and generics of an entity, an SV module, or a component
    std::vector<PortInfo> entityPorts(const UP& e) {
        std::vector<PortInfo> r;
        for (auto& p : e->ports)
            for (size_t k = 0; k < p->names.size(); ++k) r.push_back({p->names[k], p->texts[k], p->mode, nullptr});
        return r;
    }

    std::string emitEntity(const UP& ent) {
        UP arch = archOf(ent->name);
        Ctx c;
        c.container = ent->text;
        c.scopes.push_back(Scope());
        c.file = ent->file;
        std::ostringstream extra;
        std::set<std::string> specDone;
        c.extra = &extra;
        c.specDone = &specDone;
        Ctx* saved = cx;
        cx = &c;
        litNames.clear();
        std::ostringstream o;
        try {
            for (auto& d : allCtx(*ent))
                for (auto& n : d->uses) useClause(n);
            if (arch) {
                for (auto& d : allCtx(*arch))
                    for (auto& n : d->uses) useClause(n);
            }
            // drivers of each signal, to find the ones that need resolution
            std::vector<SP> stmts = ent->stmts;
            if (arch) stmts.insert(stmts.end(), arch->stmts.begin(), arch->stmts.end());
            std::ostringstream params, ports, body;
            // generics
            std::vector<std::string> plist;
            for (auto& g : ent->generics) {
                if (g->cls == "type" || g->cls == "subprogram" || g->cls == "package") fail(g->line, "generic types, subprograms and packages are not supported yet");
                TP t = subtype(g->type);
                for (size_t k = 0; k < g->names.size(); ++k) {
                    Sym y;
                    y.k = Sym::Obj;
                    y.cls = "constant";
                    y.name = g->names[k];
                    y.text = g->texts[k];
                    y.sv = svId(g->texts[k]);
                    y.type = t;
                    declare(y);
                    std::string init = g->init ? ex(g->init, t).s : "";
                    TP dt = t;
                    if (isUnconstrained(t) && g->init) dt = ex(g->init, t).t;
                    std::string decl = "parameter " + svDecl(isUnconstrained(dt) ? bt["integer"] : dt, y.sv);
                    if (dt->k == Type::String || (dt->k == Type::Array && dt->elem && dt->elem->k == Type::Char)) decl = "parameter string " + y.sv;
                    if (!init.empty()) decl += " = " + init;
                    plist.push_back(decl);
                }
            }
            // ports
            std::vector<std::string> portList;
            std::vector<std::pair<std::string, TP>> portSigs;
            for (auto& p : ent->ports) {
                TP t = subtype(p->type);
                if (isUnconstrained(t)) fail(p->line, "unconstrained port '" + p->texts[0] + "' is not supported yet; give it a range");
                for (size_t k = 0; k < p->names.size(); ++k) {
                    Sym y;
                    y.k = Sym::Obj;
                    y.cls = "signal";
                    y.name = p->names[k];
                    y.text = p->texts[k];
                    y.sv = svId(p->texts[k]);
                    y.type = t;
                    y.mode = p->mode;
                    declare(y);
                    portSigs.push_back({y.sv, t});
                }
            }
            DriverInfo di = drivers(stmts);
            for (auto& p : ent->ports) {
                TP t = subtype(p->type);
                for (size_t k = 0; k < p->names.size(); ++k) {
                    std::string sv = svId(p->texts[k]);
                    std::string dir = p->mode == "in" ? "input" : p->mode == "inout" ? "inout" : "output";
                    std::string decl;
                    bool net = p->mode == "inout" || di.needsNet(p->names[k]);
                    if (net && isPackable(t) && isFourState(t)) decl = dir + " " + svDeclNet(t, sv);
                    else decl = dir + " " + svDecl(t, sv);
                    if (p->mode == "in" && p->init) decl += " = " + ex(p->init, t).s;
                    portList.push_back(decl);
                    if (net) c.drvNames[sv] = sv;
                }
            }
            // module header
            std::string imports = "import vhdl_rt::*;";
            for (auto& pk : c.usePkgs) imports += " import " + svId(pk) + "::*;";
            for (auto& [n, pi] : c.useItems) imports += " import " + svId(pi.first) + "::" + svIdOfPkgItem(pi.first, pi.second) + ";";
            o << lineDir(ent->line) << "module " << svId(ent->text) << " " << imports;
            if (!plist.empty()) {
                o << " #(";
                for (size_t i = 0; i < plist.size(); ++i) o << (i ? ", " : "") << plist[i];
                o << ")";
            }
            o << " (";
            for (size_t i = 0; i < portList.size(); ++i) o << (i ? ",\n    " : "\n    ") << portList[i];
            o << ");\n";
            o << "  timeunit " << timeUnit << "; timeprecision " << timePrec << ";\n";
            // signals with several drivers are wires
            if (arch) {
                for (auto& d : arch->decls)
                    if (d->k == DK::Signal)
                        for (auto& n : d->names)
                            if (di.needsNet(n)) {
                                TP t = subtype(d->type);
                                if (isPackable(t) && isFourState(t)) c.drvNames[svId(d->texts[std::find(d->names.begin(), d->names.end(), n) - d->names.begin()])] = n;
                            }
            }
            emitDecls(body, ent->decls, "module");
            if (arch) {
                c.file = arch->file;
                emitDecls(body, arch->decls, "module");
            }
            // concurrent statements
            curDrivers = &di;
            for (auto& s : stmts) body << concStmt(*s, "");
            curDrivers = nullptr;
            o << body.str() << extra.str() << "endmodule\n";
        } catch (...) {
            cx = saved;
            throw;
        }
        cx = saved;
        return o.str();
    }
    std::string svIdOfPkgItem(const std::string& pkg, const std::string& item) {
        auto it = pkgScope.find(pkg);
        if (it != pkgScope.end()) {
            auto f = it->second.find(item);
            if (f != it->second.end() && !f->second.empty()) {
                const Sym& y = f->second.front();
                if (y.k == Sym::TypeK && y.type && !y.type->sv.empty()) return y.type->sv;
                if (!y.sv.empty()) return y.sv;
                return svId(y.text);
            }
        }
        return svId(item);
    }

    // ---------------------------------------------------------- drivers
    struct DriverInfo {
        std::map<std::string, int> procDrivers, instDrivers;   // lower-case signal -> count
        bool needsNet(const std::string& n) const {
            int p = procDrivers.count(n) ? procDrivers.at(n) : 0;
            int i = instDrivers.count(n) ? instDrivers.at(n) : 0;
            return i >= 2 || (i >= 1 && p >= 1) || p >= 2;
        }
    };
    DriverInfo* curDrivers = nullptr;
    static std::string baseName(const EP& t) {
        EP e = t;
        while (e && (e->k == EK::Call || e->k == EK::Sel || e->k == EK::Attr)) {
            if (e->k == EK::Sel && e->a && e->a->k == EK::Name && false) break;
            e = e->a;
        }
        return e && e->k == EK::Name ? e->name : "";
    }
    void assignedIn(const std::vector<SP>& b, std::set<std::string>& out) {
        for (auto& s : b) {
            if (s->k == SK::SigAssign || s->k == SK::CondSigAssign || s->k == SK::SelSigAssign || s->k == SK::ConcAssign || s->k == SK::ConcCond || s->k == SK::ConcSel) {
                if (s->target && s->target->k == EK::Agg) {
                    for (auto& a : s->target->agg) out.insert(baseName(a.value));
                } else out.insert(baseName(s->target));
            }
            for (auto& a : s->alts) assignedIn(a.body, out);
            assignedIn(s->body, out);
        }
    }
    DriverInfo drivers(const std::vector<SP>& stmts) {
        DriverInfo di;
        std::function<void(const std::vector<SP>&)> walk = [&](const std::vector<SP>& ss) {
            for (auto& s : ss) {
                switch (s->k) {
                case SK::Process: {
                    std::set<std::string> a;
                    assignedIn(s->body, a);
                    for (auto& n : a) di.procDrivers[n]++;
                    break;
                }
                case SK::ConcAssign: case SK::ConcCond: case SK::ConcSel: {
                    std::set<std::string> a;
                    assignedIn({s}, a);
                    for (auto& n : a) di.procDrivers[n]++;
                    break;
                }
                case SK::Inst: {
                    // out / inout actuals drive their signals
                    std::vector<PortInfo> ports = instPorts(*s);
                    size_t pos = 0;
                    for (auto& a : s->portMap) {
                        std::string mode = "in";
                        if (a.formal) {
                            std::string fn = baseName(a.formal);
                            for (auto& p : ports) if (p.name == fn) mode = p.mode;
                        } else if (pos < ports.size()) mode = ports[pos].mode;
                        ++pos;
                        if (mode == "out" || mode == "inout" || mode == "buffer" || mode == "?") {
                            std::string n = baseName(a.actual);
                            if (!n.empty()) di.instDrivers[n]++;
                        }
                    }
                    break;
                }
                case SK::ForGen: case SK::Block: walk(s->body); break;
                case SK::IfGen: case SK::CaseGen: for (auto& a : s->alts) walk(a.body); break;
                default: break;
                }
            }
        };
        walk(stmts);
        return di;
    }
    // ports of the unit an instance names: VHDL entity, component, or SV module ("?" mode)
    std::vector<PortInfo> instPorts(const Stmt& s) {
        std::string n = s.unit->k == EK::Sel ? s.unit->name : s.unit->name;
        std::vector<PortInfo> r;
        if (s.unitKind != "component") {
            auto e = entities.find(n);
            if (e != entities.end()) return entityPorts(e->second);
        }
        const Sym* y = lookup(n);
        if (y && y->k == Sym::Comp && y->decl) {
            for (auto& p : y->decl->ports)
                for (size_t k = 0; k < p->names.size(); ++k) r.push_back({p->names[k], p->texts[k], p->mode, nullptr});
            return r;
        }
        auto e = entities.find(n);
        if (e != entities.end()) return entityPorts(e->second);
        auto m = svModules.find(n);
        if (m != svModules.end())
            for (auto& p : m->second.ports) r.push_back({lowerStr(p), p, "?", nullptr});
        return r;
    }

    // ===================================================================
    // Concurrent statements
    // ===================================================================
    std::string procName(const std::string& label) { return label.empty() ? "" : " : " + svId(label); }

    // an equivalent process: body once at time 0, then wait on the signals read
    std::string equivProcess(const std::string& label, const std::string& bodyText0, const std::vector<std::string>& sigs, bool postponed = false, const Proc* pr = nullptr) {
        std::string prevDecl, prevUpd, bodyText = bodyText0;
        if (pr)
            for (auto& p : pr->prev) {
                const Val v = exPrevType(p);
                prevDecl += "    static " + svDecl(v.t ? v.t : bt["std_ulogic"], prevName(p)) + ";\n";
                prevUpd += prevName(p) + " = " + p + "; ";
            }
        replaceAll(bodyText, "@@PREV@@", prevUpd);
        std::string r = "  always begin" + procName(label) + "\n" + prevDecl + bodyText;
        if (sigs.empty()) r += "    " + prevUpd + "wait (0);\n";
        else r += "    " + prevUpd + "@(" + eventList(sigs) + ");\n";
        if (postponed) {}
        return r + "  end\n";
    }

    std::string concStmt(const Stmt& s, const std::string& /*prefix*/) {
        std::string r = lineDir(s.line);
        switch (s.k) {
        case SK::Process: return r + process(s);
        case SK::ConcAssign: case SK::ConcCond: case SK::ConcSel: {
            // as an equivalent process
            Proc pr;
            pr.active = true;
            setupDrivers(pr, s);
            Proc* sp = cx->proc;
            cx->proc = &pr;
            cx->indent = 2;
            Stmt q = s;
            q.k = s.k == SK::ConcAssign ? SK::SigAssign : s.k == SK::ConcCond ? SK::CondSigAssign : SK::SelSigAssign;
            if (q.k == SK::SigAssign && s.guarded) fail(s.line, "guarded assignments are not supported yet");
            std::string body = seqStmt(q);
            std::vector<std::string> sigs;
            std::vector<SP> one{std::make_shared<Stmt>(q)};
            readSignalsStmts(one, sigs);
            if (s.k == SK::ConcSel) readSignals(s.value, sigs);
            cx->proc = sp;
            cx->indent = 1;
            std::string decl = driverDecls(pr);
            // q <= d when rising_edge(clk): a flip-flop
            if (s.k == SK::ConcCond && s.alts.size() == 1 && s.alts[0].cond && s.alts[0].waves.size() == 1 && !s.alts[0].waves[0].after) {
                std::string e = edgeOf(s.alts[0].cond);
                if (!e.empty()) {
                    TP tt = targetType(s.target);
                    Proc p2;
                    p2.drv = pr.drv;
                    cx->proc = &p2;
                    std::string tg = target(s.target, true);
                    std::string rhs = assignRhs(s.alts[0].waves[0].val, tt);
                    cx->proc = sp;
                    if (p2.prev.empty()) return r + decl + "  always @(" + e + ") " + tg + " <= " + rhs + ";\n";
                }
            }
            // a plain `assign` would lose VHDL's delta; keep the process
            return r + decl + equivProcess(s.label, body, sigs, s.postponed, &pr);
        }
        case SK::ConcAssert: {
            Proc pr;
            Proc* sp = cx->proc;
            cx->proc = &pr;
            cx->indent = 2;
            Stmt q = s;
            q.k = SK::Assert;
            std::string body = seqStmt(q);
            std::vector<std::string> sigs;
            readSignals(s.cond, sigs);
            readSignals(s.report, sigs);
            cx->proc = sp;
            cx->indent = 1;
            return r + equivProcess(s.label, body, sigs, false, &pr);
        }
        case SK::ConcCall: {
            Proc pr;
            Proc* sp = cx->proc;
            cx->proc = &pr;
            cx->indent = 2;
            std::string body = "    " + procCall(s.call, s.line) + "\n";
            std::vector<std::string> sigs;
            readSignals(s.call, sigs);
            cx->proc = sp;
            cx->indent = 1;
            return r + equivProcess(s.label, body, sigs, false, &pr);
        }
        case SK::Inst: return r + instance(s);
        case SK::ForGen: {
            TP it;
            Rng g = rangeOf(s.range, &it);
            cx->scopes.push_back(Scope());
            Sym y;
            y.k = Sym::Obj;
            y.cls = "constant";
            y.name = lowerStr(s.var);
            y.text = s.var;
            y.sv = svId(s.var);
            y.type = bt["integer"];
            declare(y);
            std::string out = r + "  for (genvar " + y.sv + " = " + g.l + "; " + y.sv + (g.downto ? " >= " : " <= ") + g.r + "; " + y.sv +
                              (g.downto ? "--" : "++") + ") begin" + procName(s.label) + "\n";
            std::ostringstream d;
            emitDecls(d, s.decls, "module");
            out += d.str();
            for (auto& b : s.body) out += concStmt(*b, "");
            out += "  end\n";
            cx->scopes.pop_back();
            return out;
        }
        case SK::IfGen: {
            std::string out = r;
            for (size_t i = 0; i < s.alts.size(); ++i) {
                auto& a = s.alts[i];
                std::string lbl = !a.label.empty() ? a.label : s.label;
                if (a.cond) out += std::string("  ") + (i ? "else if (" : "if (") + boolOf(ex(a.cond, bt["boolean"])).s + ") begin" + procName(lbl) + "\n";
                else out += "  else begin" + procName(lbl) + "\n";
                cx->scopes.push_back(Scope());
                std::ostringstream d;
                emitDecls(d, a.decls, "module");
                out += d.str();
                for (auto& b : a.body) out += concStmt(*b, "");
                cx->scopes.pop_back();
                out += "  end\n";
            }
            return out;
        }
        case SK::CaseGen: {
            Val sel = ex(s.value, nullptr);
            std::string out = r + "  case (" + sel.s + ")\n";
            for (auto& a : s.alts) {
                std::string lbl = !a.label.empty() ? a.label : s.label;
                out += "    " + choiceList(a.choices, sel.t, false) + ": begin" + procName(lbl) + "\n";
                cx->scopes.push_back(Scope());
                std::ostringstream d;
                emitDecls(d, a.decls, "module");
                out += d.str();
                for (auto& b : a.body) out += concStmt(*b, "");
                cx->scopes.pop_back();
                out += "    end\n";
            }
            return out + "  endcase\n";
        }
        case SK::Block: {
            if (!s.genericMap.empty() || !s.portMap.empty()) fail(s.line, "blocks with generic or port maps are not supported yet");
            if (s.cond) fail(s.line, "guarded blocks are not supported yet");
            std::string out = r + "  if (1) begin" + procName(s.label) + "\n";
            cx->scopes.push_back(Scope());
            std::ostringstream d;
            emitDecls(d, s.decls, "module");
            out += d.str();
            for (auto& b : s.body) out += concStmt(*b, "");
            cx->scopes.pop_back();
            return out + "  end\n";
        }
        default: fail(s.line, "statement not allowed in a concurrent region");
        }
    }

    // signals this statement drives that need their own driver variable
    void setupDrivers(Proc& pr, const Stmt& s) {
        std::set<std::string> a;
        if (s.k == SK::Process) assignedIn(s.body, a);
        else assignedIn({std::make_shared<Stmt>(s)}, a);
        for (auto& n : a) {
            const Sym* y = lookup(n);
            if (!y || y->k != Sym::Obj || y->cls != "signal") continue;
            if (!cx->drvNames.count(y->sv)) continue;
            std::string dv = prevName(y->sv) .substr(0, prevName(y->sv).size() - 6) + "__drv" + std::to_string(++uniq);
            pr.drv[y->sv] = svId(dv);
            pr.drvTypes[svId(dv)] = y->type;
        }
    }
    std::string driverDecls(const Proc& pr) {
        std::string r;
        for (auto& [sig, dv] : pr.drv) {
            TP t = pr.drvTypes.at(dv);
            r += "  " + svDecl(t, dv) + " = 'z;\n  assign " + sig + " = " + dv + ";\n";
        }
        return r;
    }

    std::string process(const Stmt& s) {
        Proc pr;
        pr.active = true;
        pr.label = s.label;
        setupDrivers(pr, s);
        Proc* saved = cx->proc;
        cx->proc = &pr;
        cx->scopes.push_back(Scope());
        std::vector<Loop> loops;
        auto* savedLoops = cx->loops;
        cx->loops = &loops;
        std::string out;
        try {
            cx->indent = 2;
            std::ostringstream decls;
            emitDecls(decls, s.decls, "process");
            // fast forms for clocked processes
            std::string fast = clockedProcess(s, decls.str());
            if (!fast.empty()) out = fast;
            else {
                std::string body = seqStmts(s.body);
                std::vector<std::string> sens;
                if (s.sensAll) readSignalsStmts(s.body, sens);
                else for (auto& n : s.sens) addUnique(sens, ex(n, nullptr).s);
                if (s.hasSens && containsWait(s.body)) fail(s.line, "a process with a sensitivity list cannot contain wait");
                // previous values for 'event / rising_edge
                std::string prevDecl, prevUpd;
                for (auto& p : pr.prev) {
                    const Val v = exPrevType(p);
                    prevDecl += "    static " + svDecl(v.t ? v.t : bt["std_ulogic"], prevName(p)) + ";\n";
                    prevUpd += prevName(p) + " = " + p + "; ";
                }
                replaceAll(body, "@@PREV@@", prevUpd);
                out = "  always begin" + procName(s.label) + "\n" + decls.str() + prevDecl + body;
                if (s.hasSens) {
                    if (sens.empty()) out += "    " + prevUpd + "wait (0);\n";
                    else out += "    " + prevUpd + "@(" + eventList(sens) + ");\n";
                } else if (!containsWait(s.body)) {
                    out += "    wait (0);\n";   // a process without wait would loop forever
                }
                out += "  end\n";
            }
        } catch (...) {
            cx->proc = saved;
            cx->scopes.pop_back();
            cx->loops = savedLoops;
            cx->indent = 1;
            throw;
        }
        cx->proc = saved;
        cx->scopes.pop_back();
        cx->loops = savedLoops;
        cx->indent = 1;
        return driverDecls(pr) + out;
    }
    std::map<std::string, TP> prevTypes;
    Val exPrevType(const std::string& sv) {
        Val v;
        // find the signal by its SV text among visible symbols
        for (auto it = cx->scopes.rbegin(); it != cx->scopes.rend(); ++it)
            for (auto& [n, ys] : *it)
                for (auto& y : ys)
                    if (y.k == Sym::Obj && (y.sv == sv || qual(y) == sv)) { v.t = y.type; return v; }
        v.t = bt["std_ulogic"];
        return v;
    }
    static void replaceAll(std::string& s, const std::string& a, const std::string& b) {
        size_t p = 0;
        while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, a.size(), b); p += b.size(); }
    }

    // process (clk[, rst]) begin if rising_edge(clk) then ... end if; end process;
    // process (clk, rst) begin if rst = '1' then ... elsif rising_edge(clk) then ... end if; end process;
    std::string clockedProcess(const Stmt& s, const std::string& decls) {
        if (!s.hasSens || s.sensAll || s.body.size() != 1 || s.body[0]->k != SK::If) return "";
        const Stmt& iff = *s.body[0];
        std::vector<std::string> sens;
        for (auto& n : s.sens) addUnique(sens, ex(n, nullptr).s);
        auto edgeSig = [&](const EP& c) -> std::string {
            std::string e = edgeOf(c);
            return e;
        };
        std::string I = "    ";
        if (iff.alts.size() == 1) {
            std::string e = edgeSig(iff.alts[0].cond);
            if (e.empty()) return "";
            std::string clk = e.substr(e.find(' ') + 1);
            if (std::find(sens.begin(), sens.end(), clk) == sens.end()) return "";
            cx->indent = 2;
            std::string body = seqStmts(iff.alts[0].body);
            if (!cx->proc->prev.empty()) return "";
            return "  always @(" + e + ") begin" + procName(s.label) + "\n" + decls + body + "  end\n";
        }
        if (iff.alts.size() == 2 && iff.alts[1].cond) {
            std::string e = edgeSig(iff.alts[1].cond);
            if (e.empty()) return "";
            std::string clk = e.substr(e.find(' ') + 1);
            // reset condition: r = '1' / r = '0' / r (boolean)
            EP rc = iff.alts[0].cond;
            while (rc->k == EK::Un && rc->op == "()") rc = rc->a;
            std::string rst, edge;
            if (rc->k == EK::Bin && rc->op == "=" && rc->b->k == EK::Char && (rc->b->text == "1" || rc->b->text == "0")) {
                rst = ex(rc->a, nullptr).s;
                edge = rc->b->text == "1" ? "posedge " : "negedge ";
            } else return "";
            if (std::find(sens.begin(), sens.end(), clk) == sens.end() || std::find(sens.begin(), sens.end(), rst) == sens.end() || sens.size() != 2) return "";
            cx->indent = 3;
            std::string a = seqStmts(iff.alts[0].body);
            std::string b = seqStmts(iff.alts[1].body);
            cx->indent = 2;
            if (!cx->proc->prev.empty()) return "";
            return "  always @(" + e + " or " + edge + rst + ") begin" + procName(s.label) + "\n" + decls + I + "if (" +
                   boolOf(ex(iff.alts[0].cond, bt["boolean"])).s + ") begin\n" + a + I + "end else begin\n" + b + I + "end\n  end\n";
        }
        return "";
    }

    std::string instance(const Stmt& s) {
        std::string unit = s.unit->name;
        std::string label = s.label.empty() ? "u" + std::to_string(++uniq) : s.label;
        if (s.unitKind == "configuration") fail(s.line, "configuration instantiation is not supported yet");
        // the design unit: a VHDL entity (by component name or entity name), else an SV module
        std::string modName;
        std::vector<std::string> formalPorts, formalGenerics;   // in declaration order (SV spelling)
        std::vector<std::string> lowerPorts, lowerGenerics;
        std::vector<TP> portTypes;
        std::vector<TP> genTypes;
        UP ent;
        if (auto e = entities.find(unit); e != entities.end()) ent = e->second;
        const Sym* comp = s.unitKind == "entity" ? nullptr : lookup(unit);
        if (comp && comp->k != Sym::Comp) comp = nullptr;
        // component without an entity: an SV module of the same name (case-insensitively)
        if (ent) {
            modName = svId(ent->text);
            Ctx tmp;
            tmp.container = "";
            tmp.scopes.push_back(Scope());
            tmp.file = ent->file;
            Ctx* sv = cx;
            // the entity's port types are needed for literals in port maps; evaluate them in the entity's context
            tmp.usePkgs = cx->usePkgs;
            tmp.useItems = cx->useItems;
            tmp.extra = cx->extra;
            tmp.specDone = cx->specDone;
            cx = &tmp;
            try {
                for (auto& d : allCtx(*ent)) for (auto& n : d->uses) useClause(n);
                for (auto& g : ent->generics) {
                    TP t = g->cls.empty() || g->cls == "constant" ? subtype(g->type) : nullptr;
                    for (size_t k = 0; k < g->names.size(); ++k) {
                        lowerGenerics.push_back(g->names[k]);
                        formalGenerics.push_back(svId(g->texts[k]));
                        genTypes.push_back(t);
                        Sym y;
                        y.k = Sym::Obj;
                        y.cls = "constant";
                        y.name = g->names[k];
                        y.text = g->texts[k];
                        y.sv = svId(g->texts[k]);
                        y.type = t;
                        declare(y);
                    }
                }
                for (auto& p : ent->ports) {
                    TP t;
                    try { t = subtype(p->type); } catch (const Error&) { t = nullptr; }
                    for (size_t k = 0; k < p->names.size(); ++k) {
                        lowerPorts.push_back(p->names[k]);
                        formalPorts.push_back(svId(p->texts[k]));
                        portTypes.push_back(t);
                    }
                }
            } catch (...) {
                cx = sv;
                throw;
            }
            cx = sv;
        } else if (auto m = svModules.find(unit); m != svModules.end()) {
            modName = m->second.name;
            for (auto& p : m->second.ports) { formalPorts.push_back(p); lowerPorts.push_back(lowerStr(p)); portTypes.push_back(nullptr); }
            for (auto& p : m->second.params) { formalGenerics.push_back(p); lowerGenerics.push_back(lowerStr(p)); genTypes.push_back(nullptr); }
        } else if (comp) {
            modName = svId(comp->text);
            warn(s.line, "no entity or module '" + comp->text + "' found; assuming a module with the component's names");
        } else {
            modName = svId(s.unit->text);
            warn(s.line, "'" + s.unit->text + "' is not a known entity, component or module");
        }
        // component ports (names and types) when instantiating through a component
        std::vector<std::string> compPorts, compGenerics;
        std::vector<TP> compPortTypes, compGenTypes;
        if (comp && comp->decl) {
            for (auto& g : comp->decl->generics)
                for (size_t k = 0; k < g->names.size(); ++k) { compGenerics.push_back(g->names[k]); compGenTypes.push_back(g->cls == "type" ? nullptr : subtype(g->type)); }
            for (auto& p : comp->decl->ports) {
                TP t;
                try { t = subtype(p->type); } catch (const Error&) { t = nullptr; }
                for (size_t k = 0; k < p->names.size(); ++k) { compPorts.push_back(p->names[k]); compPortTypes.push_back(t); }
            }
            if (formalPorts.empty()) {
                for (auto& p : comp->decl->ports) for (auto& t : p->texts) { formalPorts.push_back(svId(t)); lowerPorts.push_back(lowerStr(t)); portTypes.push_back(nullptr); }
                for (auto& g : comp->decl->generics) for (auto& t : g->texts) { formalGenerics.push_back(svId(t)); lowerGenerics.push_back(lowerStr(t)); genTypes.push_back(nullptr); }
            }
        }
        auto mapList = [&](const std::vector<Assoc>& m, const std::vector<std::string>& order, const std::vector<std::string>& lowerNames,
                           const std::vector<TP>& types, const std::vector<std::string>& compOrder, const std::vector<TP>& compTypes, bool isPort) {
            std::vector<std::string> items;
            std::map<std::string, std::vector<std::pair<EP, EP>>> partial;   // formal -> (formal expr, actual)
            size_t pos = 0;
            for (auto& a : m) {
                std::string fl;
                TP ft;
                if (a.formal) {
                    fl = baseName(a.formal);
                    if (a.formal->k == EK::Call && isPort && !(a.formal->args.size() == 1 && a.formal->a->k == EK::Name && !lookupFormal(fl, lowerNames))) {
                        // partial association: a(0) => x
                        partial[fl].push_back({a.formal, a.actual});
                        continue;
                    }
                    if (a.formal->k == EK::Call && a.formal->a->k == EK::Name && lookupFormal(a.formal->a->name, lowerNames) == false) {
                        // conversion function on the formal: not supported
                        fail(s.line, "conversion functions in port maps are not supported yet");
                    }
                } else {
                    // positional: by the component's (or entity's) order
                    if (!compOrder.empty() && pos < compOrder.size()) fl = compOrder[pos];
                    else if (pos < lowerNames.size()) fl = lowerNames[pos];
                    else fail(s.line, "too many associations");
                }
                ++pos;
                // the formal's type: from the entity, else the component
                for (size_t i = 0; i < lowerNames.size(); ++i) if (lowerNames[i] == fl) ft = types[i];
                if (!ft) for (size_t i = 0; i < compOrder.size(); ++i) if (compOrder[i] == fl && i < compTypes.size()) ft = compTypes[i];
                std::string formalSv;
                for (size_t i = 0; i < lowerNames.size(); ++i) if (lowerNames[i] == fl) formalSv = order[i];
                if (formalSv.empty()) formalSv = svId(fl);
                std::string act;
                if (a.actual->k == EK::Open) act = "";
                else act = ex(a.actual, ft).s;
                items.push_back("." + formalSv + "(" + act + ")");
            }
            for (auto& [fl, parts] : partial) {
                // a(1) => x, a(0) => y: concatenate in the formal's index order
                std::string formalSv;
                TP ft;
                for (size_t i = 0; i < lowerNames.size(); ++i) if (lowerNames[i] == fl) { formalSv = order[i]; ft = types[i]; }
                if (!ft) for (size_t i = 0; i < compOrder.size(); ++i) if (compOrder[i] == fl && i < compTypes.size()) ft = compTypes[i];
                if (!ft || ft->k != Type::Array || !ft->dims[0].lv || !ft->dims[0].rv) fail(s.line, "partial association of '" + fl + "' needs a constant range");
                std::map<int64_t, std::string> bits;
                for (auto& [f, act] : parts) {
                    if (f->args.size() != 1) fail(s.line, "partial association form not supported");
                    EP ix = f->args[0].actual;
                    if (ix->k == EK::Range) {
                        Rng g = rangeOf(ix);
                        if (!g.lv || !g.rv) fail(s.line, "partial association needs constant indices");
                        Val v = ex(act, nullptr);
                        int64_t hi = std::max(*g.lv, *g.rv);
                        bits[hi] = v.s;   // a slice: placed at its high index
                        for (int64_t i = std::min(*g.lv, *g.rv); i < hi; ++i) bits[i] = "";
                    } else {
                        auto iv = fold(ix);
                        if (!iv) fail(s.line, "partial association needs constant indices");
                        bits[*iv] = ex(act, ft->elem).s;
                    }
                }
                std::string cat = "{";
                bool first = true;
                std::vector<int64_t> keys;
                for (auto& [k, v] : bits) keys.push_back(k);
                if (ft->dims[0].downto) std::reverse(keys.begin(), keys.end());
                for (auto k : keys) {
                    if (bits[k].empty()) continue;
                    cat += (first ? "" : ", ") + bits[k];
                    first = false;
                }
                items.push_back("." + (formalSv.empty() ? svId(fl) : formalSv) + "(" + cat + "})");
            }
            return items;
        };
        std::vector<std::string> gens = mapList(s.genericMap, formalGenerics, lowerGenerics, genTypes, compGenerics, compGenTypes, false);
        std::vector<std::string> ports = mapList(s.portMap, formalPorts, lowerPorts, portTypes, compPorts, compPortTypes, true);
        // component generics with defaults that the instance does not map: pass the component's default
        if (comp && comp->decl) {
            for (auto& g : comp->decl->generics)
                for (size_t k = 0; k < g->names.size(); ++k) {
                    bool mapped = false;
                    for (auto& a : s.genericMap) if (a.formal && baseName(a.formal) == g->names[k]) mapped = true;
                    size_t idx = 0;
                    for (auto& gg : comp->decl->generics) { if (gg == g) break; idx += gg->names.size(); }
                    if (!s.genericMap.empty() && !s.genericMap[0].formal && s.genericMap.size() > idx + k) mapped = true;
                    if (!mapped && g->init) {
                        std::string formalSv = svId(g->texts[k]);
                        for (size_t i = 0; i < lowerGenerics.size(); ++i) if (lowerGenerics[i] == g->names[k]) formalSv = formalGenerics[i];
                        gens.push_back("." + formalSv + "(" + ex(g->init, subtype(g->type)).s + ")");
                    }
                }
        }
        std::string r = "  " + modName;
        if (!gens.empty()) {
            r += " #(";
            for (size_t i = 0; i < gens.size(); ++i) r += (i ? ", " : "") + gens[i];
            r += ")";
        }
        r += " " + svId(label) + " (";
        for (size_t i = 0; i < ports.size(); ++i) r += (i ? ",\n      " : "\n      ") + ports[i];
        r += ");\n";
        return r;
    }
    static bool lookupFormal(const std::string& n, const std::vector<std::string>& names) {
        return std::find(names.begin(), names.end(), n) != names.end();
    }
};

}  // namespace vhdl
