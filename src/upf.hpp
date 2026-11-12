// upf.hpp - IEEE 1801-2024 (UPF) power intent: a reader for the Tcl subset
// UPF files use, the power-intent model, Boolean expressions, and static
// checks. No simulator dependence: msim's power-aware runtime (upf_rt.inc)
// and mlint's UPF rules both use it.
//
// Names. Every UPF object gets a rooted name relative to the power top (the
// design scope the UPF was written for), with '/' between levels:
// "PD_CORE", "u_core/VDD_SW", "PD_CORE.primary". Logic nets and instances
// are matched against the simulator's own names, which are relative to the
// simulation top and use '.', so the power top's own path ("u_dut") is put
// in front by design(): design("u_core/iso_en") -> "u_dut.u_core.iso_en".
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "tcl.hpp"
#include "upf_vct.hpp"

namespace upf {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ------------------------------------------------------------ supply values
// Supply states, in the order of upfSupplyStateE in package UPF (11.2.4).
enum SupplyState { S_OFF = 0, S_FULL_ON = 1, S_PARTIAL_ON = 2, S_UNDETERMINED = 3 };
inline const char* stateName(int s) {
    static const char* n[] = {"OFF", "FULL_ON", "PARTIAL_ON", "UNDETERMINED"};
    return s >= 0 && s < 4 ? n[s] : "?";
}
inline int stateOf(const std::string& s) {
    if (s == "OFF") return S_OFF;
    if (s == "FULL_ON" || s == "ON") return S_FULL_ON;
    if (s == "PARTIAL_ON") return S_PARTIAL_ON;
    if (s == "UNDETERMINED") return S_UNDETERMINED;
    return -1;
}
// A supply value (9.2.3): a state and a voltage, in volts. hasV is false for
// an unspecified voltage.
struct SupplyVal {
    int st = S_OFF;
    double v = 0;
    bool hasV = false;
    bool operator==(const SupplyVal& o) const {
        return st == o.st && hasV == o.hasV && (!hasV || std::fabs(v - o.v) < 1e-9);
    }
    bool operator!=(const SupplyVal& o) const { return !(*this == o); }
};

// Simstates (4.8), ordered so that a larger one has broader corruption
// (9.5.8 NOTE 2). NOT_NORMAL is simulated as CORRUPT.
enum Simstate {
    SS_NONE = -1,
    SS_NORMAL = 0,
    SS_CORRUPT_STATE_ON_CHANGE,
    SS_CORRUPT_STATE_ON_ACTIVITY,
    SS_CORRUPT_ON_CHANGE,
    SS_CORRUPT_ON_ACTIVITY,
    SS_CORRUPT
};
inline const char* simstateName(int s) {
    switch (s) {
        case SS_NORMAL: return "NORMAL";
        case SS_CORRUPT_STATE_ON_CHANGE: return "CORRUPT_STATE_ON_CHANGE";
        case SS_CORRUPT_STATE_ON_ACTIVITY: return "CORRUPT_STATE_ON_ACTIVITY";
        case SS_CORRUPT_ON_CHANGE: return "CORRUPT_ON_CHANGE";
        case SS_CORRUPT_ON_ACTIVITY: return "CORRUPT_ON_ACTIVITY";
        case SS_CORRUPT: return "CORRUPT";
    }
    return "none";
}
inline int simstateOf(const std::string& s) {
    if (s == "NORMAL") return SS_NORMAL;
    if (s == "CORRUPT" || s == "NOT_NORMAL") return SS_CORRUPT;
    if (s == "CORRUPT_ON_ACTIVITY") return SS_CORRUPT_ON_ACTIVITY;
    if (s == "CORRUPT_ON_CHANGE") return SS_CORRUPT_ON_CHANGE;
    if (s == "CORRUPT_STATE_ON_CHANGE") return SS_CORRUPT_STATE_ON_CHANGE;
    if (s == "CORRUPT_STATE_ON_ACTIVITY") return SS_CORRUPT_STATE_ON_ACTIVITY;
    return -2;
}

// -------------------------------------------------------------------- Tcl
// UPF is Tcl (5.1). The reader is the tool line's Tcl interpreter
// (tcl.hpp): variables and arrays, [command] substitution, braces and
// quotes, expr, if / foreach / for / while, proc, list and string commands,
// catch, source. Every command Tcl does not know is a UPF command: it is
// handed to onCmd as it is reached, so its result can be used by the Tcl
// around it (foreach d [find_objects ...] { ... }). A bus index in a name
// (gen[0], d[7:0]) is kept as text.
struct Cmd {
    std::string file;
    int line = 0;
    std::vector<std::string> w;
    std::string where() const { return file + ":" + std::to_string(line); }
};

// Split a Tcl list into its elements, honoring braces and quotes.
inline std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    while (i < n) {
        while (i < n && std::isspace((unsigned char)s[i])) i++;
        if (i >= n) break;
        std::string w;
        if (s[i] == '{') {
            int d = 1;
            i++;
            size_t st = i;
            while (i < n && d) {
                if (s[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (s[i] == '{') d++;
                else if (s[i] == '}') d--;
                i++;
            }
            w = s.substr(st, i - st - (d == 0 ? 1 : 0));
        } else if (s[i] == '"') {
            i++;
            while (i < n && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < n) i++;
                w += s[i++];
            }
            i++;
        } else {
            while (i < n && !std::isspace((unsigned char)s[i])) w += s[i++];
        }
        out.push_back(w);
    }
    return out;
}

struct Reader : tcl::Interp {
    std::vector<Cmd> cmds;                          // the UPF commands, when there is no onCmd
    std::function<std::string(const Cmd&)> onCmd;   // runs one UPF command; its result is the Tcl result

    Reader() {
        busBrackets = true;
        onUnknown = [this](const std::vector<std::string>& w, std::string& res) {
            Cmd c;
            c.file = file_;
            c.line = line_;
            c.w = w;
            if (!onCmd) { cmds.push_back(std::move(c)); return true; }
            // a UPF error is a Tcl error: catch {...} sees it, and the read stops at the first one
            try { res = onCmd(c); } catch (const Error& e) { throw tcl::Error(e.what()); }
            return true;
        };
    }

    // Runs `text`. The first error of the script is thrown, with its file and line.
    void parse(const std::string& text, const std::string& file) {
        size_t before = diags.size();
        tcl::Interp::parse(text, file);
        raise(before);
    }
    void load(const std::string& path) { parse(slurp(path), path); }

    // load_upf: runs another file in this interpreter, so variables and procs are shared.
    void include(const std::string& path) {
        std::string text = slurp(path);
        std::string savedFile = file_;
        int savedLine = line_;
        struct Back { Reader& r; std::string f; int l; ~Back() { r.file_ = f; r.line_ = l; } } back{*this, savedFile, savedLine};
        file_ = path;
        size_t before = diags.size();
        try { eval(text, 1); } catch (const Ret&) {}
        // an error in the file ends the load_upf command that read it
        for (size_t i = before; i < diags.size(); i++)
            if (diags[i].sev == tcl::Diag::ErrorSev) {
                std::string m = located(diags[i]);
                diags.resize(before);
                throw Error(m);
            }
    }

    static std::string slurp(const std::string& path) {
        std::ifstream f(path);
        if (!f) throw Error("cannot read UPF file " + path);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
    // "file:line: text"; a UPF command's own message already starts that way.
    static std::string located(const tcl::Diag& d) {
        size_t c = d.text.find(": ");
        if (c != std::string::npos && c > 1) {
            size_t k = c;
            while (k > 0 && std::isdigit((unsigned char)d.text[k - 1])) k--;
            if (k < c && k > 0 && d.text[k - 1] == ':') return d.text;
        }
        return d.loc.str() + ": " + d.text;
    }

private:
    void raise(size_t before) {
        for (size_t i = before; i < diags.size(); i++)
            if (diags[i].sev == tcl::Diag::ErrorSev) throw Error(located(diags[i]));
    }
};

// ------------------------------------------------------------ expressions
// Boolean expressions of UPF (5.4): logic nets and ports, supply nets,
// ports and supply set functions, power states of objects; constants;
// ! ~ && || & | ^ == != and parentheses; {STATE voltage} tuples.
struct Ex;
using ExP = std::shared_ptr<Ex>;
struct Ex {
    enum K { Id, Num, Tuple, Not, And, Or, BAnd, BOr, BXor, Eq, Ne } k = Num;
    std::string s;          // Id: the name; Num: its text
    uint64_t v = 0, x = 0;  // Num: value and unknown bits
    double real = 0;        // Num: as a voltage
    bool isReal = false;
    std::vector<ExP> kids;
};

inline ExP parseExpr(const std::string& text, const std::string& where) {
    struct P {
        const std::string& t;
        const std::string& where;
        size_t i = 0;
        void ws() { while (i < t.size() && std::isspace((unsigned char)t[i])) i++; }
        [[noreturn]] void fail(const std::string& m) {
            throw Error(where + ": " + m + " in expression {" + t + "}");
        }
        bool eat(const char* op) {
            ws();
            size_t n = std::char_traits<char>::length(op);
            if (t.compare(i, n, op) == 0) {
                // & is not &&, | is not ||, = needs ==
                if (n == 1 && (op[0] == '&' || op[0] == '|') && i + 1 < t.size() && t[i + 1] == op[0]) return false;
                i += n;
                return true;
            }
            return false;
        }
        ExP mk(Ex::K k, ExP a, ExP b = nullptr) {
            auto e = std::make_shared<Ex>();
            e->k = k;
            e->kids.push_back(a);
            if (b) e->kids.push_back(b);
            return e;
        }
        ExP orE() { ExP a = andE(); while (eat("||")) a = mk(Ex::Or, a, andE()); return a; }
        ExP andE() { ExP a = borE(); while (eat("&&")) a = mk(Ex::And, a, borE()); return a; }
        ExP borE() { ExP a = bxorE(); while (eat("|")) a = mk(Ex::BOr, a, bxorE()); return a; }
        ExP bxorE() { ExP a = bandE(); while (eat("^")) a = mk(Ex::BXor, a, bandE()); return a; }
        ExP bandE() { ExP a = eqE(); while (eat("&")) a = mk(Ex::BAnd, a, eqE()); return a; }
        ExP eqE() {
            ExP a = unary();
            for (;;) {
                if (eat("==")) a = mk(Ex::Eq, a, unary());
                else if (eat("!=")) a = mk(Ex::Ne, a, unary());
                else return a;
            }
        }
        ExP unary() {
            if (eat("!")) return mk(Ex::Not, unary());
            if (eat("~")) return mk(Ex::Not, unary());
            return prim();
        }
        ExP number(const std::string& tok) {
            auto e = std::make_shared<Ex>();
            e->k = Ex::Num;
            e->s = tok;
            size_t q = tok.find('\'');
            if (q == std::string::npos) {
                if (tok.find('.') != std::string::npos || tok.find('e') != std::string::npos) {
                    e->real = std::stod(tok);
                    e->isReal = true;
                    e->v = (uint64_t)e->real;
                } else {
                    e->v = std::stoull(tok);
                    e->real = (double)e->v;
                }
                return e;
            }
            if (q + 1 >= tok.size()) fail("bad number '" + tok + "'");
            char b = (char)std::tolower((unsigned char)tok[q + 1]);
            int sh = b == 'b' ? 1 : b == 'o' ? 3 : b == 'h' ? 4 : 0;
            std::string d = tok.substr(q + 2);
            if (!sh) {
                if (b != 'd') fail("bad number '" + tok + "'");
                e->v = std::stoull(d);
            } else
                for (char c : d) {
                    if (c == '_') continue;
                    e->v <<= sh;
                    e->x <<= sh;
                    uint64_t m = (1ull << sh) - 1;
                    char lc = (char)std::tolower((unsigned char)c);
                    if (lc == 'x' || lc == 'z' || lc == '?') e->x |= m;
                    else if (std::isdigit((unsigned char)lc)) e->v |= (uint64_t)(lc - '0');
                    else if (lc >= 'a' && lc <= 'f') e->v |= (uint64_t)(lc - 'a' + 10);
                    else fail("bad number '" + tok + "'");
                }
            e->real = (double)e->v;
            return e;
        }
        ExP prim() {
            ws();
            if (i >= t.size()) fail("missing operand");
            if (t[i] == '(') {
                i++;
                ExP e = orE();
                if (!eat(")")) fail("missing )");
                return e;
            }
            if (t[i] == '{') {   // {FULL_ON 0.8}: a supply value
                size_t e = t.find('}', i);
                if (e == std::string::npos) fail("missing }");
                auto tu = std::make_shared<Ex>();
                tu->k = Ex::Tuple;
                std::string in = t.substr(i + 1, e - i - 1);
                for (char& c : in) if (c == ',') c = ' ';
                for (auto& w : splitList(in)) {
                    if (!w.empty() && (std::isdigit((unsigned char)w[0]) || w[0] == '.' || w[0] == '-'))
                        tu->kids.push_back(number(w));
                    else {
                        auto id = std::make_shared<Ex>();
                        id->k = Ex::Id;
                        id->s = w;
                        tu->kids.push_back(id);
                    }
                }
                i = e + 1;
                return tu;
            }
            size_t st = i;
            if (std::isdigit((unsigned char)t[i]) || t[i] == '\'') {
                while (i < t.size() && (std::isalnum((unsigned char)t[i]) || t[i] == '\'' || t[i] == '.' ||
                                        t[i] == '_' || t[i] == '?'))
                    i++;
                return number(t.substr(st, i - st));
            }
            int br = 0;
            while (i < t.size()) {
                char c = t[i];
                if (c == '[') br++;
                else if (c == ']') br--;
                else if (!br && !(std::isalnum((unsigned char)c) || c == '_' || c == '/' || c == '.' || c == '$'))
                    break;
                i++;
            }
            if (i == st) fail(std::string("unexpected '") + t[i] + "'");
            auto e = std::make_shared<Ex>();
            e->k = Ex::Id;
            e->s = t.substr(st, i - st);
            return e;
        }
    } p{text, where};
    ExP e = p.orE();
    p.ws();
    if (p.i != text.size()) p.fail("unexpected text '" + text.substr(p.i) + "'");
    return e;
}
inline void exprIds(const ExP& e, std::vector<std::string>& out) {
    if (!e) return;
    if (e->k == Ex::Id) out.push_back(e->s);
    for (auto& k : e->kids) exprIds(k, out);
}

// A value during evaluation: a logic value (bits with unknowns), a supply
// value, a symbol (a state name such as FULL_ON or RUN), or an object whose
// power state is compared with a state name.
struct EVal {
    enum K { Logic, Supply, Sym, Obj } k = Logic;
    uint64_t v = 0, x = 0;
    SupplyVal sv;
    bool tupleV = false;   // a {STATE v} tuple: compare the voltage too
    std::string sym;
};
// How names in an expression are read. A callback returns false for a name
// it does not know.
struct EvalCtx {
    std::function<bool(const std::string&, EVal&)> id;
    std::function<bool(const std::string& obj, const std::string& state)> inState;   // Obj == state
};
// Three-valued truth: 0, 1, or 2 for unknown.
inline int truthOf(const EVal& a) {
    if (a.k != EVal::Logic) return 2;
    if (a.v & ~a.x) return 1;
    return a.x ? 2 : 0;
}
inline EVal logicV(int t) {
    EVal r;
    r.k = EVal::Logic;
    if (t == 2) r.x = 1;
    else r.v = (uint64_t)t;
    return r;
}
inline EVal evalEx(const ExP& e, const EvalCtx& c, const std::string& where);
inline int compareV(const EVal& a, const EVal& b, const EvalCtx& c, const std::string& where) {
    if (a.k == EVal::Logic && b.k == EVal::Logic) {
        if (a.x || b.x) return 2;
        return a.v == b.v;
    }
    const EVal* s = a.k == EVal::Supply ? &a : b.k == EVal::Supply ? &b : nullptr;
    if (s) {
        const EVal& o = s == &a ? b : a;
        if (o.k == EVal::Supply) {
            if (o.tupleV) return s->sv.st == o.sv.st && s->sv.hasV && std::fabs(s->sv.v - o.sv.v) < 1e-6;
            if (s->tupleV) return s->sv.st == o.sv.st && o.sv.hasV && std::fabs(s->sv.v - o.sv.v) < 1e-6;
            return s->sv.st == o.sv.st;
        }
        if (o.k == EVal::Sym) {
            int st = stateOf(o.sym);
            if (st < 0) throw Error(where + ": '" + o.sym + "' is not a supply state");
            return s->sv.st == st;
        }
        throw Error(where + ": a supply compared with a logic value");
    }
    const EVal* ob = a.k == EVal::Obj ? &a : b.k == EVal::Obj ? &b : nullptr;
    if (ob) {
        const EVal& o = ob == &a ? b : a;
        if (o.k != EVal::Sym) throw Error(where + ": '" + ob->sym + "' compared with something that is not a power state");
        return c.inState(ob->sym, o.sym) ? 1 : 0;
    }
    if (a.k == EVal::Sym && b.k == EVal::Sym) return a.sym == b.sym;
    throw Error(where + ": these values cannot be compared");
}
inline EVal evalEx(const ExP& e, const EvalCtx& c, const std::string& where) {
    switch (e->k) {
        case Ex::Num: {
            EVal r;
            r.v = e->v;
            r.x = e->x;
            return r;
        }
        case Ex::Id: {
            EVal r;
            if (c.id && c.id(e->s, r)) return r;
            r.k = EVal::Sym;   // a state name, compared with a supply or an object
            r.sym = e->s;
            return r;
        }
        case Ex::Tuple: {
            EVal r;
            r.k = EVal::Supply;
            if (e->kids.empty() || e->kids[0]->k != Ex::Id) throw Error(where + ": a supply value is {STATE [voltage]}");
            r.sv.st = stateOf(e->kids[0]->s);
            if (r.sv.st < 0) throw Error(where + ": '" + e->kids[0]->s + "' is not a supply state");
            if (e->kids.size() > 1) {
                r.sv.v = e->kids[1]->real;
                r.sv.hasV = r.tupleV = true;
            }
            return r;
        }
        case Ex::Not: {
            EVal a = evalEx(e->kids[0], c, where);
            int t = truthOf(a);
            return logicV(t == 2 ? 2 : !t);
        }
        case Ex::And: case Ex::BAnd: {
            int a = truthOf(evalEx(e->kids[0], c, where)), b = truthOf(evalEx(e->kids[1], c, where));
            return logicV(a == 0 || b == 0 ? 0 : a == 1 && b == 1 ? 1 : 2);
        }
        case Ex::Or: case Ex::BOr: {
            int a = truthOf(evalEx(e->kids[0], c, where)), b = truthOf(evalEx(e->kids[1], c, where));
            return logicV(a == 1 || b == 1 ? 1 : a == 0 && b == 0 ? 0 : 2);
        }
        case Ex::BXor: {
            int a = truthOf(evalEx(e->kids[0], c, where)), b = truthOf(evalEx(e->kids[1], c, where));
            return logicV(a == 2 || b == 2 ? 2 : a ^ b);
        }
        case Ex::Eq: case Ex::Ne: {
            int r = compareV(evalEx(e->kids[0], c, where), evalEx(e->kids[1], c, where), c, where);
            if (e->k == Ex::Ne && r != 2) r = !r;
            return logicV(r);
        }
    }
    return logicV(2);
}

// ------------------------------------------------------------------ model
struct PowerState {
    std::string name;
    ExP supplyExpr, logicExpr;
    int simstate = SS_NONE;
    bool illegal = false;
    std::string where;
};
struct SupplySet {
    std::string name;
    std::map<std::string, std::string> fn;   // function (power, ground, ...) -> supply net or port
    std::vector<PowerState> states;          // user states and updates of ON / OFF
    bool complete = false;
    std::string where;
};
struct Domain {
    std::string name, scope, where;
    std::vector<std::string> elements;   // rooted instance names ("." is the scope itself)
    std::vector<std::string> exclude;
    std::map<std::string, std::string> handles;   // primary -> supply set name
    std::vector<PowerState> states;
    bool complete = false;
};
struct SwitchState { std::string name, inPort; ExP e; };
struct Switch {
    std::string name, where, outPort, outNet;
    std::vector<std::pair<std::string, std::string>> ins, ctrls;   // port -> net
    std::vector<SwitchState> on, partial, off, error;
    struct Ack { std::string port, net; ExP e; uint64_t delay = 0; };
    std::vector<Ack> acks;
};
struct SigSense { std::string net; int sense = 0; };   // 0 high, 1 low, 2 posedge, 3 negedge
inline int senseOf(const std::string& s) {
    if (s == "high") return 0;
    if (s == "low") return 1;
    if (s == "posedge") return 2;
    if (s == "negedge") return 3;
    return -1;
}
struct Isolation {
    std::string name, domain, where;
    std::vector<std::string> elements, exclude;
    int appliesTo = 2;   // 0 inputs, 1 outputs, 2 both
    std::string clamp = "0";
    std::vector<std::string> signals;
    std::vector<int> senseLow;
    std::string supply, source, sink;
    bool diffSupplyOnly = true, noIso = false, clampSet = false;
    int boundary = 0;    // -applies_to_boundary: 0 both, 1 lower, 2 upper
};
struct LevelShifter {
    std::string name, domain, where;
    std::vector<std::string> elements, exclude;
    int appliesTo = 2;
    std::string inSupply, outSupply, rule = "both";
    bool noShift = false;
    int boundary = 0;
    std::string source, sink;
};
// A repeater (buffer) strategy (6.52).
struct Repeater {
    std::string name, domain, scope, where;
    std::vector<std::string> elements, exclude;
    int appliesTo = 2;   // 0 inputs, 1 outputs, 2 both
    int boundary = 0;    // 0 both, 1 lower, 2 upper
    std::string supply, source, sink;
};
// connect_logic_net (6.13): one port of the net.
struct LogicConn { std::string net, port, scope, where; bool reconnect = false; };
struct Retention {
    std::string name, domain, where;
    std::vector<std::string> elements, exclude;
    std::string supply;
    SigSense save, restore;
    ExP saveCond, restoreCond, restorePeriodCond, powerdownCond;
    bool noRet = false, useAsPrimary = false, retSupCor = true, savResCor = true;
    std::string appliesTo = "any";
};
// add_state_transition (6.7): named transitions between power states of an object.
struct Transition {
    std::string name, where;
    std::vector<std::string> from, to;                          // -from / -to (empty: every legal state)
    bool fromTo = false;
    std::vector<std::pair<std::string, std::string>> paired;    // -paired
    bool illegal = false;
    bool matches(const std::string& a, const std::string& b) const {
        for (auto& [x, y] : paired) if (x == a && y == b) return true;
        if (!fromTo) return false;
        auto in = [](const std::vector<std::string>& l, const std::string& v) {
            return l.empty() || std::find(l.begin(), l.end(), v) != l.end();
        };
        return in(from, a) && in(to, b);
    }
};
struct TransSet {
    std::vector<Transition> tr;
    bool complete = false;
    std::string where;
    char kind = 0;   // 's' supply set, 'd' domain, 'g' group, 0 any
};
// create_power_state_group (6.22).
struct StateGroup {
    std::string name, scope, where;
    std::vector<PowerState> states;
    bool complete = false;
};
struct PortStateDef { std::string name; bool off = false; double vmin = 0, vmax = 0; bool partial = false; };
// set_equivalent (6.47): supplies declared equivalent.
struct Equiv {
    std::vector<std::string> names;
    bool functionOnly = false, interchangeable = true;
    std::string where;
};
// create_composite_domain (6.17): a container of domains.
struct Composite {
    std::string name, scope, where;
    std::vector<std::string> subs;
};
// connect_supply_set (6.15): elements explicitly powered by a supply set.
struct SupplyConn {
    std::string set, scope, where;
    std::vector<std::string> elements, exclude;
    bool hasElements = false, transitive = true;
    std::vector<std::pair<std::string, std::string>> connect;   // function, pg_type
};
// set_simstate_behavior (6.56) for models or instances.
struct SimBehavior {
    std::string mode, lib, scope, where;
    std::vector<std::string> models, elements, exclude;
};
// set_retention_elements (6.54): a named list of state elements kept together.
struct RetList {
    std::string name, scope, where;
    std::vector<std::string> elements, exclude;
    std::string appliesTo;            // "", required, optional, not_required, not_optional
    bool required = true;             // -retention_purpose
};
// set_design_attributes (6.44) and set_port_attributes (6.51): attributes
// of models, instances and ports (5.6), kept as the commands gave them and
// looked up by precedence (5.7): a named port or element before a model.
struct DesignAttrRule {
    std::string scope, where;
    std::vector<std::string> models, elements, exclude;
    bool hasElements = false;
    std::map<std::string, std::string> attrs;
};
struct PortAttrRule {
    std::string scope, where, model;
    std::vector<std::string> elements, exclude, ports, excludePorts;
    bool hasElements = false, hasPorts = false;
    int dirs = 7;   // bit 0 inputs, 1 outputs, 2 inouts
    std::map<std::string, std::string> attrs;
};
struct Pst {
    std::string name, where;
    std::vector<std::string> supplies;
    struct Row { std::string name; std::vector<std::string> st; std::string where; };
    std::vector<Row> rows;
};

struct Model {
    std::string top;   // the power top's path in the simulation, '.'-separated ("" = the simulation top)
    std::string designTop;   // set_design_top
    std::map<std::string, Domain> domains;
    std::vector<std::string> domainOrder;
    std::map<std::string, SupplySet> sets;
    std::map<std::string, std::string> setAlias;   // handle -> supply set
    std::set<std::string> ports, nets;              // supply ports and nets
    std::map<std::string, std::string> portDir;     // supply port -> in / out
    std::map<std::string, std::string> resolve;     // supply net -> resolution
    std::map<std::string, std::vector<std::string>> connects;   // supply net -> ports it connects
    std::map<std::string, Vct> vcts;                            // create_hdl2upf_vct / create_upf2hdl_vct
    std::map<std::pair<std::string, std::string>, std::string> connectVct;   // {net, port} -> connect_supply_net -vct
    std::map<std::pair<std::string, std::string>, std::string> connectWhere;
    // A VCT by name: one made in this power intent, or a predefined one (Annex B).
    const Vct* vct(const std::string& n) const {
        auto it = vcts.find(n);
        if (it != vcts.end()) return &it->second;
        auto p = predefinedVcts().find(n);
        return p == predefinedVcts().end() ? nullptr : &p->second;
    }
    std::map<std::string, Switch> switches;
    std::vector<Isolation> isos;
    std::vector<LevelShifter> lss;
    std::vector<Retention> rets;
    std::vector<Repeater> reps;
    std::map<std::string, TransSet> transitions;     // object (rooted name) -> its named transitions
    std::map<std::string, StateGroup> groups;
    std::vector<std::string> groupOrder;
    std::map<std::string, std::string> logicPorts;   // create_logic_port: rooted name -> in / out / inout
    std::set<std::string> logicNets;                 // create_logic_net: rooted names
    std::vector<LogicConn> logicConns;
    std::map<std::string, std::vector<PortStateDef>> portStates;
    std::string partialOn;                           // set_partial_on_translation: "", OFF or FULL_ON
    std::vector<Equiv> eqNets, eqSets;
    std::map<std::string, Composite> composites;
    std::vector<SupplyConn> supplyConns;
    std::vector<SimBehavior> simBehaviors;
    std::map<std::string, RetList> retLists;
    std::vector<DesignAttrRule> designAttrs;
    std::vector<PortAttrRule> portAttrs;
    std::map<std::string, std::string> globalAttrs;
    std::vector<Pst> psts;
    std::string version;
    std::vector<std::string> warnings;

    // ---- names ----
    static std::string join(const std::string& scope, const std::string& n) {
        if (n.empty() || n == ".") return scope;
        if (n[0] == '/') return n.substr(1);
        return scope.empty() ? n : scope + "/" + n;
    }
    // A UPF name (rooted at the power top) as a simulator name.
    std::string design(const std::string& n) const {
        std::string d = n;
        for (char& c : d) if (c == '/') c = '.';
        if (d == "." || d.empty()) return top;
        return top.empty() ? d : top + "." + d;
    }
    std::string setOf(std::string n) const {
        for (int k = 0; k < 32; k++) {
            auto it = setAlias.find(n);
            if (it == setAlias.end()) break;
            n = it->second;
        }
        return n;
    }
    // The supply net a supply set function names: "PD.primary.power",
    // "SS.ground". Empty if n is not a function.
    std::string fnNet(const std::string& n) const {
        size_t d = n.rfind('.');
        if (d == std::string::npos) return "";
        auto it = sets.find(setOf(n.substr(0, d)));
        if (it == sets.end()) return "";
        auto f = it->second.fn.find(n.substr(d + 1));
        return f == it->second.fn.end() ? "" : f->second;
    }
    SupplySet& setRef(const std::string& n, const std::string& where) {
        std::string r = setOf(n);
        auto it = sets.find(r);
        if (it != sets.end()) return it->second;
        // a domain's handle that has not been made yet: an anonymous set
        size_t d = r.rfind('.');
        if (d != std::string::npos && domains.count(r.substr(0, d))) {
            SupplySet& s = sets[r];
            s.name = r;
            s.where = where;
            return s;
        }
        throw Error(where + ": no supply set '" + n + "'");
    }
    // The primary supply set of a domain.
    std::string primaryOf(const std::string& dom) const {
        auto it = domains.find(dom);
        if (it == domains.end()) return "";
        auto h = it->second.handles.find("primary");
        return setOf(h == it->second.handles.end() ? dom + ".primary" : h->second);
    }

    // ---- equivalence of supply sets (4.5.5, 6.47) ----
    // Two supply sets match when they are the same set, when set_equivalent
    // declares them equivalent (functional equivalence counts unless the
    // caller asks for electrical only), or when each of their functions is
    // on the same supply subnet (connect_supply_net and set_equivalent -nets).
    mutable std::map<std::string, std::string> eqElec_, eqFunc_;
    mutable bool eqBuilt_ = false;
    void buildEq() const {
        eqBuilt_ = true;
        eqElec_.clear();
        eqFunc_.clear();
        std::map<std::string, std::string> up;
        std::function<std::string(std::string)> find = [&](std::string n) {
            while (up.count(n) && up[n] != n) n = up[n];
            return n;
        };
        auto unite = [&](const std::string& a, const std::string& b) {
            std::string x = find(a), y = find(b);
            if (x != y) up[x] = y;
        };
        for (auto& [net, ps] : connects) for (auto& p : ps) unite(p, net);
        for (auto& e : eqNets)
            if (!e.functionOnly) for (auto& n : e.names) unite(n, e.names[0]);
        std::map<std::string, std::string> bySig;   // function signature -> first set with it
        for (auto& [n, st] : sets) {
            std::string sig;
            for (auto& [f, net] : st.fn) sig += f + "=" + find(net) + ";";
            if (sig.empty()) { eqElec_[n] = n; continue; }
            auto it = bySig.emplace(sig, n).first;
            eqElec_[n] = it->second;
        }
        for (int pass = 0; pass < 2; pass++) {
            auto& cls = pass == 0 ? eqElec_ : eqFunc_;
            if (pass == 1) cls = eqElec_;
            auto root = [&](std::string n) {
                for (int k = 0; k < 64; k++) {
                    auto it = cls.find(n);
                    if (it == cls.end() || it->second == n) break;
                    n = it->second;
                }
                return n;
            };
            for (auto& e : eqSets) {
                if (e.functionOnly && (pass == 0 || !e.interchangeable)) continue;
                std::string first;
                for (auto& n : e.names) {
                    std::string r = root(setOf(n));
                    if (first.empty()) first = r;
                    else if (r != first) cls[r] = first;
                }
            }
            for (auto& [n, st] : sets) { (void)st; cls[n] = root(n); }
        }
    }
    // The equivalence class of a supply set; functional = interchangeable
    // functional equivalence counts too.
    std::string eqOf(const std::string& set, bool functional = true) const {
        if (!eqBuilt_) buildEq();
        std::string n = setOf(set);
        const auto& cls = functional ? eqFunc_ : eqElec_;
        auto it = cls.find(n);
        return it == cls.end() ? n : it->second;
    }
    // Every domain a name stands for: itself, or the subdomains of a composite (6.17).
    std::vector<std::string> domainsOf(const std::string& n, int depth = 0) const {
        std::vector<std::string> r;
        auto c = composites.find(n);
        if (c == composites.end() || depth > 16) { r.push_back(n); return r; }
        for (auto& sub : c->second.subs)
            for (auto& d : domainsOf(sub, depth + 1)) r.push_back(d);
        return r;
    }

    // ---- attributes (5.6) ----
    static bool pathIn(const std::string& path, const std::string& e) {
        if (e.empty()) return true;
        return path == e || (path.size() > e.size() && path.compare(0, e.size(), e) == 0 &&
                             (path[e.size()] == '.' || path[e.size()] == '['));
    }
    static bool modelIn(const std::vector<std::string>& models, const std::string& model) {
        for (auto& mm : models) {
            size_t dot = mm.rfind('.');   // "lib.model"
            if (mm == model || (dot != std::string::npos && mm.substr(dot + 1) == model)) return true;
        }
        return false;
    }
    // The value of attribute `name` of an instance (simulator path) of
    // `model`; "" if it has none. An instance named by -elements wins over
    // its model; of the same kind, the later command wins.
    std::string designAttr(const std::string& inst, const std::string& model, const std::string& name) const {
        std::string val;
        int best = 0;
        for (auto& r : designAttrs) {
            auto a = r.attrs.find(name);
            if (a == r.attrs.end()) continue;
            int rank = 0;
            if (r.hasElements) {
                if (!r.models.empty() && !modelIn(r.models, model)) continue;
                bool in = false;
                for (auto& e : r.elements) if (design(e) == inst) in = true;
                for (auto& e : r.exclude) if (design(e) == inst) in = false;
                if (in) rank = 2;
            } else if (modelIn(r.models, model)) rank = 1;
            if (rank && rank >= best) { best = rank; val = a->second; }
        }
        return val;
    }
    // The value of attribute `name` of port `port` (direction dir: 0 in, 1
    // out, 2 inout) of instance `inst` of `model`.
    std::string portAttr(const std::string& inst, const std::string& model, const std::string& port, int dir,
                         const std::string& name) const {
        std::string val;
        int best = 0;
        std::string full = inst.empty() ? port : inst + "." + port;
        for (auto& r : portAttrs) {
            auto a = r.attrs.find(name);
            if (a == r.attrs.end()) continue;
            if (!(r.dirs & (1 << dir))) continue;
            int rank = 0;
            if (!r.model.empty()) {
                bool here = r.model == "." ? inst == design(r.scope) : modelIn({r.model}, model);
                if (!here) continue;
                if (r.hasPorts) { if (std::find(r.ports.begin(), r.ports.end(), port) != r.ports.end()) rank = 2; }
                else rank = 1;
            } else {
                if (r.hasPorts)
                    for (auto& pn : r.ports) if (design(pn) == full) rank = 4;
                if (!rank && r.hasElements)
                    for (auto& e : r.elements) if (design(e) == inst) rank = 3;
                for (auto& e : r.exclude) if (design(e) == inst) rank = 0;
            }
            for (auto& pn : r.excludePorts)
                if (pn == port || design(join(r.scope, pn)) == full) rank = 0;
            if (rank && rank >= best) { best = rank; val = a->second; }
        }
        return val;
    }

    // ---- reading ----
    void load(const std::string& path, const std::string& powerTop) {
        top = powerTop;
        Reader r;
        std::string scope;
        r.onPuts = [this](const std::string& t) { if (out) *out << t << "\n"; };
        r.onCmd = [&](const Cmd& c) {
            run({c}, scope, r, c.file);
            return std::string();
        };
        r.load(path);
        for (auto& d : r.diags) warnings.push_back(Reader::located(d));
    }
    std::ostream* out = &std::cout;   // where Tcl puts goes

    struct Opts {
        std::multimap<std::string, std::string> v;
        std::vector<std::string> pos;
        bool has(const std::string& k) const { return v.count(k) != 0; }
        std::string get(const std::string& k, const std::string& d = "") const {
            auto it = v.find(k);
            return it == v.end() ? d : it->second;
        }
        std::vector<std::string> all(const std::string& k) const {
            std::vector<std::string> r;
            auto [a, b] = v.equal_range(k);
            for (auto it = a; it != b; ++it) r.push_back(it->second);
            return r;
        }
        std::vector<std::string> list(const std::string& k) const {
            std::vector<std::string> r;
            for (auto& s : all(k))
                for (auto& w : splitList(s)) r.push_back(w);
            return r;
        }
    };
    // Options: flags take no value; -opt with an optional TRUE/FALSE value too.
    static Opts opts(const Cmd& c, std::set<std::string> flags, std::set<std::string> boolOpt = {}) {
        Opts o;
        for (size_t i = 1; i < c.w.size(); i++) {
            const std::string& a = c.w[i];
            if (a.size() > 1 && a[0] == '-' && !std::isdigit((unsigned char)a[1])) {
                if (flags.count(a)) { o.v.insert({a, ""}); continue; }
                if (boolOpt.count(a)) {
                    std::string val = "TRUE";
                    if (i + 1 < c.w.size()) {
                        std::string u = c.w[i + 1];
                        for (char& ch : u) ch = (char)std::toupper((unsigned char)ch);
                        if (u == "TRUE" || u == "FALSE") { val = u; i++; }
                    }
                    o.v.insert({a, val});
                    continue;
                }
                if (i + 1 >= c.w.size()) throw Error(c.where() + ": " + c.w[0] + " " + a + " needs a value");
                o.v.insert({a, c.w[++i]});
                continue;
            }
            o.pos.push_back(a);
        }
        return o;
    }
    static int boundaryOf(const std::string& v, const Cmd& c) {
        if (v == "both") return 0;
        if (v == "lower") return 1;
        if (v == "upper") return 2;
        throw Error(c.where() + ": -applies_to_boundary " + v + " (lower, upper or both)");
    }
    static ExP expr(const std::string& t, const Cmd& c) {
        return t.empty() ? nullptr : parseExpr(t, c.where());
    }

    void run(const std::vector<Cmd>& cmds, std::string& scope, Reader& r, const std::string& file);
};

inline void Model::run(const std::vector<Cmd>& cmds, std::string& scope, Reader& rd, const std::string& file) {
    for (const Cmd& c : cmds) {
        const std::string& n = c.w[0];
        auto need = [&](const Opts& o, size_t k, const char* what) {
            if (o.pos.size() < k) throw Error(c.where() + ": " + n + " needs " + what);
        };
        auto rooted = [&](const std::vector<std::string>& l) {
            std::vector<std::string> r;
            for (auto& e : l) r.push_back(join(scope, e));
            return r;
        };
        if (n == "upf_version") {
            if (c.w.size() > 1) version = c.w[1];
            continue;
        }
        if (n == "set_design_top") {
            if (c.w.size() > 1) designTop = c.w[1];
            continue;
        }
        if (n == "set_scope") {
            std::string s = c.w.size() > 1 ? c.w[1] : "/";
            if (s == "/" ) scope.clear();
            else if (s == "..") {
                size_t p = scope.rfind('/');
                scope = p == std::string::npos ? "" : scope.substr(0, p);
            } else if (s != ".") scope = join(scope, s);
            continue;
        }
        if (n == "load_upf") {
            Opts o = opts(c, {});
            need(o, 1, "a file");
            std::string p = o.pos[0];
            if (!p.empty() && p[0] != '/') {
                size_t sl = file.rfind('/');
                if (sl != std::string::npos) p = file.substr(0, sl + 1) + p;
            }
            std::string saved = scope;
            if (o.has("-scope")) scope = join(scope, o.get("-scope"));
            struct Back { std::string& s; std::string v; ~Back() { s = v; } } back{scope, saved};
            rd.include(p);
            scope = saved;
            continue;
        }
        if (n == "create_power_domain") {
            Opts o = opts(c, {"-atomic", "-update", "-include_scope"});
            need(o, 1, "a domain name");
            std::string name = join(scope, o.pos[0]);
            bool fresh = !domains.count(name);
            if (fresh && o.has("-update")) throw Error(c.where() + ": -update of unknown domain " + name);
            if (!fresh && !o.has("-update")) throw Error(c.where() + ": domain " + name + " is already defined");
            Domain& d = domains[name];
            if (fresh) {
                d.name = name;
                d.scope = scope;
                d.where = c.where();
                domainOrder.push_back(name);
            }
            for (auto& e : o.list("-elements")) d.elements.push_back(join(scope, e));
            if (o.has("-include_scope")) d.elements.push_back(scope);
            for (auto& e : o.list("-exclude_elements")) d.exclude.push_back(join(scope, e));
            for (auto& s : o.all("-supply")) {
                auto l = splitList(s);
                if (l.empty()) continue;
                std::string handle = name + "." + l[0];
                d.handles[l[0]] = handle;
                if (l.size() > 1) setAlias[handle] = setOf(join(scope, l[1]));
                else if (!sets.count(handle)) { sets[handle].name = handle; sets[handle].where = c.where(); }
            }
            if (!d.handles.count("primary")) {
                d.handles["primary"] = name + ".primary";
                if (!setAlias.count(name + ".primary") && !sets.count(name + ".primary")) {
                    sets[name + ".primary"].name = name + ".primary";
                    sets[name + ".primary"].where = c.where();
                }
            }
            continue;
        }
        if (n == "create_supply_port") {
            Opts o = opts(c, {"-virtual"});
            need(o, 1, "a port name");
            for (auto& p : splitList(o.pos[0])) {
                std::string base = o.has("-domain") ? domains.count(join(scope, o.get("-domain")))
                                                          ? domains[join(scope, o.get("-domain"))].scope : scope
                                                    : scope;
                std::string name = join(base, p);
                ports.insert(name);
                portDir[name] = o.get("-direction", "in");
            }
            continue;
        }
        if (n == "create_supply_net") {
            Opts o = opts(c, {"-reuse", "-virtual"});
            need(o, 1, "a net name");
            for (auto& p : splitList(o.pos[0])) {
                std::string base = scope;
                if (o.has("-domain")) {
                    auto it = domains.find(join(scope, o.get("-domain")));
                    if (it == domains.end()) throw Error(c.where() + ": no domain " + o.get("-domain"));
                    base = it->second.scope;
                }
                std::string name = join(base, p);
                nets.insert(name);
                if (o.has("-resolve")) resolve[name] = o.get("-resolve");
            }
            continue;
        }
        if (n == "connect_supply_net") {
            Opts o = opts(c, {});
            need(o, 1, "a net name");
            std::string net = join(scope, o.pos[0]);
            std::string v = o.get("-vct");
            if (!v.empty() && !vct(v)) throw Error(c.where() + ": connect_supply_net -vct: no value conversion table '" + v + "'");
            for (auto& p : o.list("-ports")) {
                std::string port = join(scope, p);
                connects[net].push_back(port);
                if (!v.empty()) connectVct[{net, port}] = v;
                connectWhere[{net, port}] = c.where();
            }
            continue;
        }
        if (n == "create_hdl2upf_vct" || n == "create_upf2hdl_vct") {   // 6.18, 6.28
            Opts o = opts(c, {});
            need(o, 1, "a table name");
            Vct v;
            v.name = o.pos[0];
            v.where = c.where();
            v.hdl2upf = n == "create_hdl2upf_vct";
            if (vct(v.name)) throw Error(c.where() + ": value conversion table " + v.name + " is already defined" +
                                         (vcts.count(v.name) ? "" : " (Annex B)"));
            auto ht = o.list("-hdl_type");
            if (ht.empty()) throw Error(c.where() + ": " + n + " needs -hdl_type {vhdl|sv [typename]}");
            v.lang = ht[0];
            for (char& ch : v.lang) ch = (char)std::tolower((unsigned char)ch);
            if (v.lang != "sv" && v.lang != "vhdl") throw Error(c.where() + ": -hdl_type " + ht[0] + ": the language is vhdl or sv");
            if (ht.size() > 1) v.hdlType = ht[1];
            if (!o.has("-table")) throw Error(c.where() + ": " + n + " needs -table {{from_value to_value} ...}");
            std::set<std::string> seen;
            for (auto& row : o.list("-table")) {
                auto l = splitList(row);
                if (l.size() != 2) throw Error(c.where() + ": -table row {" + row + "} is not {from_value to_value}");
                const std::string& hdl = v.hdl2upf ? l[0] : l[1];
                const std::string& st = v.hdl2upf ? l[1] : l[0];
                if (Vct::stateNum(st) < 0)
                    throw Error(c.where() + ": -table row {" + row + "}: '" + st + "' is not OFF, FULL_ON, PARTIAL_ON or UNDETERMINED");
                if (!Vct::hdlChar(hdl, v.lang))
                    throw Error(c.where() + ": -table row {" + row + "}: '" + hdl + "' is not a value of " +
                                (v.lang == "sv" ? "SystemVerilog logic (0 1 X Z)" : "VHDL std_logic (U X 0 1 Z W L H -)"));
                std::string from = v.hdl2upf ? std::string(1, Vct::hdlChar(hdl, v.lang)) : Vct::upper(st);
                if (!seen.insert(from).second) throw Error(c.where() + ": -table has two rows for " + l[0]);
                v.table.push_back({l[0], l[1]});
            }
            vcts[v.name] = v;
            continue;
        }
        if (n == "create_supply_set") {
            Opts o = opts(c, {"-update", "-virtual"});
            need(o, 1, "a supply set name");
            std::string name = join(scope, o.pos[0]);
            if (o.has("-update")) {
                SupplySet& s = setRef(name, c.where());
                for (auto& f : o.all("-function")) {
                    auto l = splitList(f);
                    if (l.size() == 2) s.fn[l[0]] = join(scope, l[1]);
                }
                continue;
            }
            SupplySet& s = sets[name];
            s.name = name;
            s.where = c.where();
            for (auto& f : o.all("-function")) {
                auto l = splitList(f);
                if (l.size() != 2) throw Error(c.where() + ": -function needs {function net}");
                s.fn[l[0]] = join(scope, l[1]);
            }
            continue;
        }
        if (n == "associate_supply_set") {
            Opts o = opts(c, {});
            need(o, 1, "a supply set");
            if (!o.has("-handle")) throw Error(c.where() + ": associate_supply_set needs -handle");
            std::string h = join(scope, o.get("-handle"));
            std::string s = setOf(join(scope, o.pos[0]));
            // a handle that was made as an anonymous set takes the target's place
            if (sets.count(h) && sets[h].fn.empty()) sets.erase(h);
            setAlias[h] = s;
            continue;
        }
        if (n == "set_domain_supply_net") {   // legacy (6.46)
            Opts o = opts(c, {});
            need(o, 1, "a domain");
            std::string dn = join(scope, o.pos[0]);
            if (!domains.count(dn)) throw Error(c.where() + ": no domain " + o.pos[0]);
            SupplySet& s = setRef(primaryOf(dn), c.where());
            if (o.has("-primary_power_net")) s.fn["power"] = join(scope, o.get("-primary_power_net"));
            if (o.has("-primary_ground_net")) s.fn["ground"] = join(scope, o.get("-primary_ground_net"));
            continue;
        }
        if (n == "create_power_switch") {
            Opts o = opts(c, {"-update"});
            need(o, 1, "a switch name");
            std::string name = join(scope, o.pos[0]);
            Switch& sw = switches[name];
            sw.name = name;
            sw.where = c.where();
            auto portNet = [&](const std::string& v, std::string& port, std::string& net) {
                auto l = splitList(v);
                if (l.empty()) throw Error(c.where() + ": empty port");
                port = l[0];
                net = l.size() > 1 ? join(scope, l[1]) : "";
            };
            if (o.has("-output_supply_port")) portNet(o.get("-output_supply_port"), sw.outPort, sw.outNet);
            for (auto& v : o.all("-input_supply_port")) {
                std::string p, nn;
                portNet(v, p, nn);
                sw.ins.push_back({p, nn});
            }
            for (auto& v : o.all("-control_port")) {
                std::string p, nn;
                portNet(v, p, nn);
                sw.ctrls.push_back({p, nn});
            }
            auto states = [&](const char* opt, std::vector<SwitchState>& out, bool withPort) {
                for (auto& v : o.all(opt)) {
                    auto l = splitList(v);
                    SwitchState s;
                    if (l.size() < (withPort ? 3u : 2u)) throw Error(c.where() + ": " + opt + " needs {name" + (withPort ? " input_port" : "") + " {expr}}");
                    s.name = l[0];
                    if (withPort) s.inPort = l[1];
                    s.e = parseExpr(l[withPort ? 2 : 1], c.where());
                    out.push_back(s);
                }
            };
            states("-on_state", sw.on, true);
            states("-on_partial_state", sw.partial, true);
            states("-off_state", sw.off, false);
            states("-error_state", sw.error, false);
            for (auto& v : o.all("-ack_port")) {
                auto l = splitList(v);
                Switch::Ack a;
                if (l.size() < 2) throw Error(c.where() + ": -ack_port needs {port net [expr]}");
                a.port = l[0];
                a.net = l[1].empty() ? "" : join(scope, l[1]);
                a.e = parseExpr(l.size() > 2 ? l[2] : "1", c.where());
                sw.acks.push_back(a);
            }
            for (auto& v : o.all("-ack_delay")) {
                auto l = splitList(v);
                if (l.size() != 2) continue;
                for (auto& a : sw.acks)
                    if (a.port == l[0]) a.delay = std::stoull(l[1]);
            }
            if (sw.outPort.empty() && o.get("-switch_type", "coarse_grain") != "fine_grain")
                throw Error(c.where() + ": create_power_switch needs -output_supply_port");
            continue;
        }
        // A strategy of a composite domain is a strategy of each subdomain,
        // if it is legal in every one (6.17).
        if (n == "set_isolation" || n == "set_level_shifter" || n == "set_repeater" || n == "set_retention") {
            size_t di = 0;
            for (size_t k = 1; k + 1 < c.w.size(); k++) if (c.w[k] == "-domain") di = k + 1;
            if (di && composites.count(join(scope, c.w[di]))) {
                Model saved = *this;
                try {
                    for (auto& sub : domainsOf(join(scope, c.w[di]))) {
                        Cmd c2 = c;
                        c2.w[di] = "/" + sub;
                        run({c2}, scope, rd, file);
                    }
                } catch (Error& e) {
                    *this = saved;
                    warnings.push_back(c.where() + ": " + n + " is not applied to composite domain " + c.w[di] +
                                       ": it is an error in a subdomain (" + e.what() + ")");
                }
                continue;
            }
        }
        if (n == "set_isolation") {
            Opts o = opts(c, {"-no_isolation", "-force_isolation", "-update"},
                          {"-diff_supply_only", "-use_functional_equivalence"});
            need(o, 1, "a strategy name");
            if (!o.has("-domain")) throw Error(c.where() + ": set_isolation needs -domain");
            std::string dn = join(scope, o.get("-domain"));
            if (!domains.count(dn)) throw Error(c.where() + ": no domain " + o.get("-domain"));
            Isolation* is = nullptr;
            if (o.has("-update"))
                for (auto& x : isos) if (x.name == o.pos[0] && x.domain == dn) is = &x;
            if (!is) { isos.push_back({}); is = &isos.back(); is->name = o.pos[0]; is->domain = dn; is->where = c.where(); }
            for (auto& e : rooted(o.list("-elements"))) is->elements.push_back(e);
            for (auto& e : rooted(o.list("-exclude_elements"))) is->exclude.push_back(e);
            if (o.has("-applies_to")) {
                std::string a = o.get("-applies_to");
                is->appliesTo = a == "inputs" ? 0 : a == "outputs" ? 1 : 2;
            }
            if (o.has("-clamp_value")) { is->clamp = o.get("-clamp_value"); is->clampSet = true; }
            if (o.has("-isolation_signal")) {
                is->signals.clear();
                for (auto& s : o.list("-isolation_signal")) is->signals.push_back(join(scope, s));
            }
            if (o.has("-isolation_sense")) {
                is->senseLow.clear();
                for (auto& s : o.list("-isolation_sense")) is->senseLow.push_back(s == "low");
            }
            if (o.has("-isolation_supply")) is->supply = join(scope, splitList(o.get("-isolation_supply")).at(0));
            if (o.has("-isolation_supply_set")) is->supply = join(scope, o.get("-isolation_supply_set"));
            if (o.has("-source")) is->source = join(scope, o.get("-source"));
            if (o.has("-sink")) is->sink = join(scope, o.get("-sink"));
            if (o.has("-diff_supply_only")) is->diffSupplyOnly = o.get("-diff_supply_only") == "TRUE";
            if (o.has("-no_isolation")) is->noIso = true;
            if (o.has("-applies_to_boundary")) is->boundary = boundaryOf(o.get("-applies_to_boundary"), c);
            if (is->clamp != "0" && is->clamp != "1" && is->clamp != "Z" && is->clamp != "z" && is->clamp != "latch")
                throw Error(c.where() + ": -clamp_value " + is->clamp + " is not supported (0, 1, Z, latch)");
            continue;
        }
        if (n == "set_isolation_control") {   // legacy UPF 1.0
            Opts o = opts(c, {});
            need(o, 1, "a strategy name");
            std::string dn = join(scope, o.get("-domain"));
            for (auto& x : isos)
                if (x.name == o.pos[0] && x.domain == dn) {
                    if (o.has("-isolation_signal")) x.signals = {join(scope, o.get("-isolation_signal"))};
                    if (o.has("-isolation_sense")) x.senseLow = {o.get("-isolation_sense") == "low"};
                }
            continue;
        }
        if (n == "set_level_shifter") {
            Opts o = opts(c, {"-no_shift", "-force_shift", "-update"});
            need(o, 1, "a strategy name");
            if (!o.has("-domain")) throw Error(c.where() + ": set_level_shifter needs -domain");
            LevelShifter l;
            l.name = o.pos[0];
            l.domain = join(scope, o.get("-domain"));
            if (!domains.count(l.domain)) throw Error(c.where() + ": no domain " + o.get("-domain"));
            l.where = c.where();
            l.elements = rooted(o.list("-elements"));
            l.exclude = rooted(o.list("-exclude_elements"));
            std::string a = o.get("-applies_to", "both");
            l.appliesTo = a == "inputs" ? 0 : a == "outputs" ? 1 : 2;
            if (o.has("-input_supply")) l.inSupply = join(scope, o.get("-input_supply"));
            if (o.has("-output_supply")) l.outSupply = join(scope, o.get("-output_supply"));
            l.rule = o.get("-rule", "both");
            l.noShift = o.has("-no_shift");
            if (o.has("-applies_to_boundary")) l.boundary = boundaryOf(o.get("-applies_to_boundary"), c);
            if (o.has("-source")) l.source = join(scope, o.get("-source"));
            if (o.has("-sink")) l.sink = join(scope, o.get("-sink"));
            lss.push_back(l);
            continue;
        }
        if (n == "set_retention") {
            Opts o = opts(c, {"-no_retention", "-use_retention_as_primary", "-update"});
            need(o, 1, "a strategy name");
            if (!o.has("-domain")) throw Error(c.where() + ": set_retention needs -domain");
            std::string dn = join(scope, o.get("-domain"));
            if (!domains.count(dn)) throw Error(c.where() + ": no domain " + o.get("-domain"));
            Retention* rt = nullptr;
            if (o.has("-update"))
                for (auto& x : rets) if (x.name == o.pos[0] && x.domain == dn) rt = &x;
            if (!rt) { rets.push_back({}); rt = &rets.back(); rt->name = o.pos[0]; rt->domain = dn; rt->where = c.where(); }
            for (auto& e : rooted(o.list("-elements"))) {
                auto rl = retLists.find(e);   // a named list of set_retention_elements (6.54)
                if (rl == retLists.end()) { rt->elements.push_back(e); continue; }
                for (auto& x : rl->second.elements) rt->elements.push_back(x);
                for (auto& x : rl->second.exclude) rt->exclude.push_back(x);
            }
            for (auto& e : rooted(o.list("-exclude_elements"))) rt->exclude.push_back(e);
            if (o.has("-retention_supply")) rt->supply = join(scope, o.get("-retention_supply"));
            auto ss = [&](const char* opt, SigSense& out) {
                if (!o.has(opt)) return;
                auto l = splitList(o.get(opt));
                if (l.size() != 2 || senseOf(l[1]) < 0)
                    throw Error(c.where() + ": " + opt + " needs {net high|low|posedge|negedge}");
                out.net = join(scope, l[0]);
                out.sense = senseOf(l[1]);
            };
            ss("-save_signal", rt->save);
            ss("-restore_signal", rt->restore);
            if (o.has("-save_event_condition")) rt->saveCond = expr(o.get("-save_event_condition"), c);
            if (o.has("-save_condition")) rt->saveCond = expr(o.get("-save_condition"), c);
            if (o.has("-restore_event_condition")) rt->restoreCond = expr(o.get("-restore_event_condition"), c);
            if (o.has("-restore_condition")) rt->restoreCond = expr(o.get("-restore_condition"), c);
            if (o.has("-restore_period_condition")) rt->restorePeriodCond = expr(o.get("-restore_period_condition"), c);
            if (o.has("-powerdown_period_condition")) rt->powerdownCond = expr(o.get("-powerdown_period_condition"), c);
            if (o.has("-retention_condition")) rt->powerdownCond = expr(o.get("-retention_condition"), c);
            if (o.has("-no_retention")) rt->noRet = true;
            if (o.has("-use_retention_as_primary")) rt->useAsPrimary = true;
            if (o.has("-applies_to")) rt->appliesTo = o.get("-applies_to");
            for (auto& p : o.list("-parameters")) {
                if (p == "NO_RET_SUP_COR") rt->retSupCor = false;
                if (p == "RET_SUP_COR") rt->retSupCor = true;
                if (p == "NO_SAV_RES_COR") rt->savResCor = false;
                if (p == "SAV_RES_COR") rt->savResCor = true;
            }
            if (!rt->noRet && (rt->save.net.empty() || rt->restore.net.empty()) && !o.has("-update"))
                throw Error(c.where() + ": set_retention needs -save_signal and -restore_signal (6.53)");
            if (rt->restoreCond && rt->restorePeriodCond)
                throw Error(c.where() + ": -restore_event_condition and -restore_period_condition together (6.53)");
            continue;
        }
        if (n == "create_composite_domain") {   // 6.17
            Opts o = opts(c, {"-update"});
            need(o, 1, "a composite domain name");
            std::string name = join(scope, o.pos[0]);
            bool fresh = !composites.count(name);
            if (fresh && o.has("-update")) throw Error(c.where() + ": -update of unknown composite domain " + o.pos[0]);
            if (!fresh && !o.has("-update")) throw Error(c.where() + ": composite domain " + o.pos[0] + " is already defined");
            if (fresh && domains.count(name)) throw Error(c.where() + ": " + o.pos[0] + " is already a power domain");
            Composite& cd = composites[name];
            if (fresh) {
                cd.name = name;
                cd.scope = scope;
                cd.where = c.where();
                // its power states are held like those of a group (6.22)
                StateGroup& g = groups[name];
                g.name = name;
                g.scope = scope;
                g.where = c.where();
                groupOrder.push_back(name);
            }
            for (auto& sd : o.list("-subdomains")) {
                std::string sn = join(scope, sd);
                if (!domains.count(sn) && !composites.count(sn)) throw Error(c.where() + ": no domain " + sd);
                if (sn == name) throw Error(c.where() + ": composite domain " + o.pos[0] + " contains itself");
                cd.subs.push_back(sn);
            }
            for (auto& sv : o.all("-supply")) {
                auto l = splitList(sv);
                if (l.empty()) continue;
                if (l[0] != "primary") throw Error(c.where() + ": only the primary supply handle can be given for a composite domain (6.17)");
                if (l.size() > 1) setAlias[name + ".primary"] = setOf(join(scope, l[1]));
            }
            eqBuilt_ = false;
            continue;
        }
        if (n == "set_equivalent") {   // 6.47
            Opts o = opts(c, {"-function_only"}, {"-interchangeable"});
            if (o.has("-nets") == o.has("-sets")) throw Error(c.where() + ": set_equivalent needs -nets or -sets, not both (6.47)");
            if (o.has("-interchangeable") && !o.has("-function_only"))
                throw Error(c.where() + ": -interchangeable without -function_only (6.47)");
            Equiv e;
            e.functionOnly = o.has("-function_only");
            e.interchangeable = o.get("-interchangeable", "TRUE") == "TRUE";
            e.where = c.where();
            bool nets = o.has("-nets");
            for (auto& x : o.list(nets ? "-nets" : "-sets")) {
                std::string full = join(scope, x);
                if (nets) {
                    if (!this->nets.count(full) && !ports.count(full) && fnNet(full).empty())
                        throw Error(c.where() + ": set_equivalent: no supply net or port " + x);
                    if (!fnNet(full).empty()) full = fnNet(full);
                } else full = setRef(full, c.where()).name;
                e.names.push_back(full);
            }
            if (e.names.size() < 2) throw Error(c.where() + ": set_equivalent needs two or more supplies");
            (nets ? eqNets : eqSets).push_back(e);
            eqBuilt_ = false;
            continue;
        }
        if (n == "set_partial_on_translation") {   // 6.50
            if (c.w.size() != 2 || (c.w[1] != "OFF" && c.w[1] != "FULL_ON"))
                throw Error(c.where() + ": set_partial_on_translation needs OFF or FULL_ON");
            if (!partialOn.empty() && partialOn != c.w[1])
                throw Error(c.where() + ": set_partial_on_translation " + c.w[1] + " after " + partialOn + " (6.50)");
            partialOn = c.w[1];
            continue;
        }
        if (n == "connect_supply_set") {   // 6.15
            Opts o = opts(c, {}, {"-transitive"});
            need(o, 1, "a supply set");
            SupplyConn sc;
            sc.set = setRef(join(scope, o.pos[0]), c.where()).name;
            sc.scope = scope;
            sc.where = c.where();
            sc.hasElements = o.has("-elements");
            sc.elements = rooted(o.list("-elements"));
            sc.exclude = rooted(o.list("-exclude_elements"));
            sc.transitive = o.get("-transitive", "TRUE") == "TRUE";
            for (auto& cv : o.all("-connect")) {
                auto l = splitList(cv);
                if (l.size() != 2) throw Error(c.where() + ": -connect needs {function pg_type_list}");
                for (auto& pg : splitList(l[1])) {
                    for (auto& old : supplyConns)
                        for (auto& [f, t] : old.connect)
                            if (old.set != sc.set && t == pg && old.elements == sc.elements && sc.hasElements)
                                throw Error(c.where() + ": pg_type " + pg + " is already connected to " + old.set + " for these elements (6.15)");
                    sc.connect.push_back({l[0], pg});
                }
            }
            // a handle of a domain: its elements are those of the domain
            if (!sc.hasElements) {
                std::string h = join(scope, o.pos[0]);
                size_t dot = h.rfind('.');
                if (dot != std::string::npos && domains.count(h.substr(0, dot))) {
                    sc.elements = domains[h.substr(0, dot)].elements;
                    sc.hasElements = true;
                }
            }
            if (!sc.hasElements) throw Error(c.where() + ": connect_supply_set needs -elements (the supply set is not a handle of a domain)");
            supplyConns.push_back(sc);
            continue;
        }
        if (n == "set_simstate_behavior") {   // 6.56
            Opts o = opts(c, {});
            need(o, 1, "ENABLE, SIMSTATE_ONLY, PORT_CORR_ONLY or DISABLE");
            SimBehavior sb;
            sb.mode = o.pos[0];
            if (sb.mode != "ENABLE" && sb.mode != "SIMSTATE_ONLY" && sb.mode != "PORT_CORR_ONLY" && sb.mode != "DISABLE")
                throw Error(c.where() + ": set_simstate_behavior " + sb.mode + " (ENABLE, SIMSTATE_ONLY, PORT_CORR_ONLY or DISABLE)");
            sb.lib = o.get("-lib");
            sb.models = o.list("-models");
            sb.scope = scope;
            sb.where = c.where();
            bool rel = !sb.models.empty() || !sb.lib.empty();   // element names are relative to the models
            for (auto& e : o.list("-elements")) sb.elements.push_back(rel ? e : join(scope, e));
            for (auto& e : o.list("-exclude_elements")) sb.exclude.push_back(rel ? e : join(scope, e));
            if (!sb.exclude.empty() && sb.models.empty() && sb.elements.empty() && sb.lib.empty())
                throw Error(c.where() + ": -exclude_elements without -models, -elements or -lib (6.56)");
            if (sb.models.empty() && sb.elements.empty() && sb.lib.empty())
                throw Error(c.where() + ": set_simstate_behavior names no model or element (6.56)");
            if (!sb.lib.empty())
                for (auto& mm : sb.models) {
                    size_t dot = mm.find('.');
                    if (dot != std::string::npos && mm.substr(0, dot) != sb.lib)
                        throw Error(c.where() + ": model " + mm + " is not in library " + sb.lib + " (6.56)");
                }
            simBehaviors.push_back(sb);
            continue;
        }
        if (n == "set_retention_elements") {   // 6.54
            Opts o = opts(c, {}, {"-transitive"});
            need(o, 1, "a list name");
            std::string name = join(scope, o.pos[0]);
            if (retLists.count(name)) throw Error(c.where() + ": retention list " + o.pos[0] + " is already defined in this scope (6.54)");
            if (!o.has("-elements")) throw Error(c.where() + ": set_retention_elements needs -elements");
            RetList rl;
            rl.name = name;
            rl.scope = scope;
            rl.where = c.where();
            rl.elements = rooted(o.list("-elements"));
            rl.exclude = rooted(o.list("-exclude_elements"));
            rl.appliesTo = o.get("-applies_to");
            if (!rl.appliesTo.empty() && rl.appliesTo != "required" && rl.appliesTo != "optional" &&
                rl.appliesTo != "not_required" && rl.appliesTo != "not_optional")
                throw Error(c.where() + ": -applies_to " + rl.appliesTo);
            std::string pu = o.get("-retention_purpose", "required");
            if (pu != "required" && pu != "optional") throw Error(c.where() + ": -retention_purpose " + pu);
            rl.required = pu == "required";
            retLists[name] = rl;
            continue;
        }
        if (n == "set_design_attributes") {   // 6.44
            Opts o = opts(c, {}, {"-is_soft_macro", "-is_hard_macro", "-is_refinable_macro", "-is_power_aware_model", "-is_leaf_cell", "-is_macro_cell"});
            DesignAttrRule r;
            r.scope = scope;
            r.where = c.where();
            for (auto& mm : o.list("-models")) r.models.push_back(mm == "." ? std::string(".") : mm);
            r.hasElements = o.has("-elements");
            r.elements = rooted(o.list("-elements"));
            r.exclude = rooted(o.list("-exclude_elements"));
            if (!r.exclude.empty() && !r.hasElements) throw Error(c.where() + ": -exclude_elements without -elements (6.44)");
            for (auto& av : o.all("-attribute")) {
                auto l = splitList(av);
                if (l.size() != 2) throw Error(c.where() + ": -attribute needs {name value}");
                r.attrs[l[0]] = l[1];
            }
            for (const char* f : {"soft_macro", "hard_macro", "refinable_macro", "power_aware_model", "leaf_cell", "macro_cell"})
                if (o.has(std::string("-is_") + f)) r.attrs[std::string("UPF_is_") + f] = o.get(std::string("-is_") + f);
            if (o.has("-switch_cell_type")) {
                std::string t = o.get("-switch_cell_type");
                if (t != "coarse_grain" && t != "fine_grain") throw Error(c.where() + ": -switch_cell_type " + t);
                r.attrs["UPF_switch_cell_type"] = t;
            }
            if (r.attrs.empty()) throw Error(c.where() + ": set_design_attributes gives no attribute (6.44)");
            static const std::map<std::string, std::set<std::string>> legal = {
                {"UPF_is_soft_macro", {"TRUE", "FALSE"}}, {"UPF_is_hard_macro", {"TRUE", "FALSE"}},
                {"UPF_is_refinable_macro", {"TRUE", "FALSE"}}, {"UPF_is_power_aware_model", {"TRUE", "FALSE"}},
                {"UPF_retention", {"required", "optional"}},
                {"UPF_simstate_behavior", {"ENABLE", "SIMSTATE_ONLY", "PORT_CORR_ONLY", "DISABLE"}}};
            for (auto& [an, av] : r.attrs) {
                auto lg = legal.find(an);
                if (lg != legal.end() && !lg->second.count(av))
                    throw Error(c.where() + ": attribute " + an + " cannot be " + av + " (5.6)");
            }
            if (r.models.empty() && !r.hasElements) {   // global attributes
                for (auto& [an, av] : r.attrs) globalAttrs[an] = av;
                continue;
            }
            // -models {.}: the model of the current scope, as an element
            if (r.models.size() == 1 && r.models[0] == "." && !r.hasElements) {
                r.models.clear();
                r.hasElements = true;
                r.elements.push_back(scope);
            }
            designAttrs.push_back(r);
            continue;
        }
        if (n == "set_port_attributes") {   // 6.51
            Opts o = opts(c, {"-unconnected", "-is_analog", "-is_isolated"});
            PortAttrRule r;
            r.scope = scope;
            r.where = c.where();
            r.model = o.get("-model");
            r.hasElements = o.has("-elements");
            r.elements = rooted(o.list("-elements"));
            r.exclude = rooted(o.list("-exclude_elements"));
            r.hasPorts = o.has("-ports");
            for (auto& pn : o.list("-ports")) r.ports.push_back(r.model.empty() ? join(scope, pn) : pn);
            r.excludePorts = o.list("-exclude_ports");
            if (r.model.empty() && !r.hasElements && !r.hasPorts)
                throw Error(c.where() + ": set_port_attributes needs -model, -elements or -ports (6.51)");
            if (o.has("-applies_to")) {
                r.dirs = 0;
                for (auto& d : o.list("-applies_to")) {
                    if (d == "inputs") r.dirs |= 1;
                    else if (d == "outputs") r.dirs |= 2;
                    else if (d == "inouts") r.dirs |= 4;
                    else throw Error(c.where() + ": -applies_to " + d + " (inputs, outputs, inouts)");
                }
            }
            for (auto& av : o.all("-attribute")) {
                auto l = splitList(av);
                if (l.size() != 2) throw Error(c.where() + ": -attribute needs {name value}");
                r.attrs[l[0]] = l[1];
            }
            static const std::pair<const char*, const char*> opt2attr[] = {
                {"-clamp_value", "UPF_clamp_value"}, {"-async_clamp_value", "UPF_async_clamp_value"},
                {"-sink_off_clamp", "UPF_sink_off_clamp_value"}, {"-source_off_clamp", "UPF_source_off_clamp_value"},
                {"-pg_type", "UPF_pg_type"}, {"-related_power_port", "UPF_related_power_port"},
                {"-related_ground_port", "UPF_related_ground_port"}, {"-related_bias_ports", "UPF_related_bias_ports"},
                {"-feedthrough", "UPF_feedthrough"}};
            for (auto& [op, an] : opt2attr) if (o.has(op)) r.attrs[an] = o.get(op);
            for (const char* op : {"-driver_supply", "-receiver_supply", "-literal_supply"})
                if (o.has(op)) r.attrs[std::string("UPF_") + (op + 1)] = setRef(join(scope, o.get(op)), c.where()).name;
            if (o.has("-unconnected")) r.attrs["UPF_unconnected"] = "TRUE";
            if (o.has("-is_analog")) r.attrs["UPF_is_analog"] = "TRUE";
            if (o.has("-is_isolated")) r.attrs["UPF_is_isolated"] = "TRUE";
            if (r.attrs.empty()) throw Error(c.where() + ": set_port_attributes gives no attribute (6.51)");
            auto acv = r.attrs.find("UPF_async_clamp_value");
            if (acv != r.attrs.end() && acv->second != "0" && acv->second != "1")
                throw Error(c.where() + ": -async_clamp_value " + acv->second + " (0 or 1)");
            portAttrs.push_back(r);
            continue;
        }
        if (n == "create_power_state_group") {   // 6.22
            Opts o = opts(c, {});
            need(o, 1, "a group name");
            std::string name = join(scope, o.pos[0]);
            if (groups.count(name)) throw Error(c.where() + ": power state group " + o.pos[0] + " is already defined");
            StateGroup& g = groups[name];
            g.name = name;
            g.scope = scope;
            g.where = c.where();
            groupOrder.push_back(name);
            continue;
        }
        if (n == "add_state_transition" || n == "describe_state_transition") {   // 6.7 (and its UPF 2.x name)
            Opts o = opts(c, {"-supply", "-domain", "-group", "-model", "-instance", "-update", "-complete", "-legal", "-illegal"});
            need(o, 1, "an object");
            bool legacy = n == "describe_state_transition";   // name -object obj -from .. -to .. [-paired ..] [-illegal]
            if (legacy && !o.has("-object")) throw Error(c.where() + ": describe_state_transition needs -object");
            std::string objArg = legacy ? o.get("-object") : o.pos[0];
            std::string obj = join(scope, objArg);
            char kind = o.has("-supply") ? 's' : o.has("-domain") ? 'd' : o.has("-group") ? 'g' : 0;
            if (o.has("-model") || o.has("-instance")) {
                warnings.push_back(c.where() + ": " + n + " -model / -instance: power states of models are not simulated");
                continue;
            }
            if (kind == 's' || (!kind && !domains.count(obj) && !groups.count(obj))) obj = setRef(obj, c.where()).name;
            else if (kind == 'd' && !domains.count(obj) && !composites.count(obj)) throw Error(c.where() + ": no domain " + objArg);
            else if (kind == 'g' && !groups.count(obj)) throw Error(c.where() + ": no power state group " + objArg);
            if (kind == 'd' && composites.count(obj)) kind = 'g';
            if (!kind) kind = groups.count(obj) ? 'g' : domains.count(obj) ? 'd' : 's';
            bool fresh = !transitions.count(obj);
            if (fresh && o.has("-update")) throw Error(c.where() + ": " + n + " -update: no transitions of " + objArg + " to update");
            TransSet& ts = transitions[obj];
            if (fresh) { ts.where = c.where(); ts.kind = kind; }
            if (o.has("-complete")) ts.complete = true;
            auto parse = [&](const std::vector<std::string>& l) {
                if (l.empty()) throw Error(c.where() + ": -transition needs a name");
                Transition t;
                t.name = l[0];
                t.where = c.where();
                bool from = false, to = false;
                for (size_t k = 1; k < l.size(); k++) {
                    auto val = [&]() -> std::string {
                        if (k + 1 >= l.size()) throw Error(c.where() + ": " + l[k] + " needs a value");
                        return l[++k];
                    };
                    if (l[k] == "-from") { t.from = splitList(val()); from = true; }
                    else if (l[k] == "-to") { t.to = splitList(val()); to = true; }
                    else if (l[k] == "-paired") {
                        for (auto& pr : splitList(val())) {
                            auto two = splitList(pr);
                            if (two.size() != 2) throw Error(c.where() + ": -paired needs {{from to} ...}");
                            t.paired.push_back({two[0], two[1]});
                        }
                    } else if (l[k] == "-illegal") t.illegal = true;
                    else if (l[k] == "-legal") t.illegal = false;
                    else throw Error(c.where() + ": unknown transition option " + l[k]);
                }
                if (from != to) throw Error(c.where() + ": transition " + t.name + " needs both -from and -to (6.7)");
                t.fromTo = from;
                if (!t.fromTo && t.paired.empty())
                    throw Error(c.where() + ": transition " + t.name + " needs -from and -to, or -paired (6.7)");
                for (auto& a : t.from)
                    for (auto& b : t.to)
                        if (a == b) throw Error(c.where() + ": transition " + t.name + ": " + a + " is both the -from and the -to state (6.7)");
                for (auto& [a, b] : t.paired)
                    if (a == b) throw Error(c.where() + ": transition " + t.name + ": " + a + " is both the from and the to state (6.7)");
                bool merged = false;
                for (auto& old : ts.tr)
                    if (old.name == t.name) {
                        if (!o.has("-update") && !legacy) throw Error(c.where() + ": transition " + t.name + " is already defined");
                        if (t.fromTo) { old.from = t.from; old.to = t.to; old.fromTo = true; }
                        for (auto& pr : t.paired) old.paired.push_back(pr);
                        old.illegal = t.illegal;
                        merged = true;
                    }
                if (!merged) ts.tr.push_back(t);
            };
            for (auto& tv : o.all("-transition")) parse(splitList(tv));
            if (legacy) {
                std::vector<std::string> l{o.pos[0]};
                if (o.has("-from")) { l.push_back("-from"); l.push_back(o.get("-from")); }
                if (o.has("-to")) { l.push_back("-to"); l.push_back(o.get("-to")); }
                if (o.has("-paired")) { l.push_back("-paired"); l.push_back(o.get("-paired")); }
                if (o.has("-illegal")) l.push_back("-illegal");
                parse(l);
            }
            continue;
        }
        if (n == "set_repeater") {   // 6.52
            Opts o = opts(c, {"-update"}, {"-use_functional_equivalence"});
            need(o, 1, "a strategy name");
            if (!o.has("-domain")) throw Error(c.where() + ": set_repeater needs -domain");
            std::string dn = join(scope, o.get("-domain"));
            if (!domains.count(dn)) throw Error(c.where() + ": no domain " + o.get("-domain"));
            Repeater* rp = nullptr;
            for (auto& x : reps) if (x.name == o.pos[0] && x.domain == dn && x.scope == scope) rp = &x;
            if (o.has("-update") && !rp) throw Error(c.where() + ": set_repeater -update of unknown strategy " + o.pos[0]);
            if (!o.has("-update") && rp) throw Error(c.where() + ": repeater strategy " + o.pos[0] + " is already defined");
            if (!rp) { reps.push_back({}); rp = &reps.back(); rp->name = o.pos[0]; rp->domain = dn; rp->scope = scope; rp->where = c.where(); }
            for (auto& e : rooted(o.list("-elements"))) rp->elements.push_back(e);
            for (auto& e : rooted(o.list("-exclude_elements"))) rp->exclude.push_back(e);
            if (o.has("-repeater_supply")) rp->supply = join(scope, o.get("-repeater_supply"));
            if (o.has("-source")) rp->source = join(scope, o.get("-source"));
            if (o.has("-sink")) rp->sink = join(scope, o.get("-sink"));
            if (o.has("-applies_to")) {
                std::string a = o.get("-applies_to");
                if (a != "inputs" && a != "outputs" && a != "both") throw Error(c.where() + ": -applies_to " + a);
                rp->appliesTo = a == "inputs" ? 0 : a == "outputs" ? 1 : 2;
            }
            if (o.has("-applies_to_boundary")) rp->boundary = boundaryOf(o.get("-applies_to_boundary"), c);
            if (rp->supply.empty()) throw Error(c.where() + ": set_repeater needs -repeater_supply (6.52)");
            continue;
        }
        if (n == "create_logic_port") {   // 6.20
            Opts o = opts(c, {});
            need(o, 1, "a port name");
            auto l = splitList(o.pos[0]);
            if (l.empty()) throw Error(c.where() + ": create_logic_port: the port list is empty (6.20)");
            std::string dir = o.get("-direction", "in");
            if (dir != "in" && dir != "out" && dir != "inout") throw Error(c.where() + ": -direction " + dir);
            for (auto& p : l) logicPorts[join(scope, p)] = dir;
            continue;
        }
        if (n == "create_logic_net") {   // 6.19
            Opts o = opts(c, {});
            need(o, 1, "a net name");
            auto l = splitList(o.pos[0]);
            if (l.empty()) throw Error(c.where() + ": create_logic_net: the net list is empty (6.19)");
            for (auto& p : l) logicNets.insert(join(scope, p));
            continue;
        }
        if (n == "connect_logic_net") {   // 6.13
            Opts o = opts(c, {"-reconnect"});
            need(o, 1, "a net name");
            if (!o.has("-ports")) throw Error(c.where() + ": connect_logic_net needs -ports");
            if (o.pos[0].find('/') != std::string::npos)
                throw Error(c.where() + ": connect_logic_net: " + o.pos[0] + " is not a simple name (6.13)");
            std::string net = join(scope, o.pos[0]);
            for (auto& p : o.list("-ports")) {
                std::string port = join(scope, p);
                for (auto& x : logicConns)
                    if (x.port == port && x.net != net)
                        throw Error(c.where() + ": port " + p + " is already connected to logic net " + x.net + " (6.13 e)");
                logicConns.push_back({net, port, scope, c.where(), o.has("-reconnect")});
            }
            continue;
        }
        if (n == "add_power_state") {
            Opts o = opts(c, {"-supply", "-domain", "-group", "-model", "-instance", "-update", "-complete"});
            need(o, 1, "an object");
            std::string obj = join(scope, o.pos[0]);
            std::vector<PowerState>* states = nullptr;
            bool* complete = nullptr;
            bool isSet = false;
            if (groups.count(obj) && !o.has("-supply") && (!o.has("-domain") || composites.count(obj))) {
                states = &groups[obj].states;
                complete = &groups[obj].complete;
            } else if (o.has("-group")) {
                throw Error(c.where() + ": no power state group " + o.pos[0]);
            } else if (domains.count(obj) && !o.has("-supply")) {
                states = &domains[obj].states;
                complete = &domains[obj].complete;
            } else {
                SupplySet& s = setRef(obj, c.where());
                states = &s.states;
                complete = &s.complete;
                isSet = true;
            }
            if (o.has("-complete")) *complete = true;
            for (auto& st : o.all("-state")) {
                auto l = splitList(st);
                if (l.empty()) continue;
                PowerState ps;
                ps.name = l[0];
                ps.where = c.where();
                for (size_t k = 1; k < l.size(); k++) {
                    auto val = [&]() -> std::string {
                        if (k + 1 >= l.size()) throw Error(c.where() + ": " + l[k] + " needs a value");
                        return l[++k];
                    };
                    if (l[k] == "-supply_expr") ps.supplyExpr = expr(val(), c);
                    else if (l[k] == "-logic_expr") ps.logicExpr = expr(val(), c);
                    else if (l[k] == "-simstate") {
                        std::string v = val();
                        ps.simstate = simstateOf(v);
                        if (ps.simstate == -2) throw Error(c.where() + ": unknown simstate " + v);
                        if (!isSet) throw Error(c.where() + ": a simstate on a power state of a domain (6.5 g)");
                    } else if (l[k] == "-illegal") ps.illegal = true;
                    else if (l[k] == "-legal") ps.illegal = false;
                    else if (l[k] == "-power_expr") val();
                    else throw Error(c.where() + ": unknown power state option " + l[k]);
                }
                bool merged = false;
                for (auto& old : *states)
                    if (old.name == ps.name) {   // -update refines a state
                        if (ps.supplyExpr) old.supplyExpr = ps.supplyExpr;
                        if (ps.logicExpr) old.logicExpr = ps.logicExpr;
                        if (ps.simstate != SS_NONE) old.simstate = ps.simstate;
                        old.illegal = ps.illegal;
                        merged = true;
                    }
                if (!merged) states->push_back(ps);
            }
            continue;
        }
        if (n == "add_port_state" || n == "add_supply_state") {   // legacy PST port states (6.4, 6.8)
            Opts o = opts(c, {});
            need(o, 1, "a supply port");
            std::string p = join(scope, o.pos[0]);
            if (!fnNet(p).empty()) p = fnNet(p);   // a supply set function (6.8)
            else if (!ports.count(p) && !nets.count(p))
                throw Error(c.where() + ": " + n + ": no supply port, net or supply set function " + o.pos[0]);
            for (auto& st : o.all("-state")) {
                auto l = splitList(st);
                if (l.size() < 2) throw Error(c.where() + ": -state needs {name value}");
                PortStateDef d;
                d.name = l[0];
                if (l[1] == "off" || l[1] == "OFF") d.off = true;
                else {
                    // add_supply_state (6.8): a voltage, or {net_state voltage};
                    // add_port_state (6.4): a voltage or {min nom max}
                    auto two = splitList(l[1]);
                    try {
                        if (n == "add_supply_state" && two.size() == 2 && !std::isdigit((unsigned char)two[0][0]) && two[0][0] != '.') {
                            if (two[0] == "OFF") d.off = true;
                            else if (two[0] == "PARTIAL_ON") d.partial = true;
                            else if (two[0] != "FULL_ON") throw Error(c.where() + ": net state " + two[0] + " in -state " + st);
                            d.vmin = d.vmax = std::stod(two[1]);
                        } else if (two.size() > 1) {
                            d.vmin = std::stod(two.front());
                            d.vmax = std::stod(two.back());
                        } else {
                            d.vmin = std::stod(l[1]);
                            d.vmax = std::stod(l.back());
                        }
                    } catch (Error&) { throw; }
                    catch (...) { throw Error(c.where() + ": bad voltage in -state " + st); }
                }
                portStates[p].push_back(d);
            }
            continue;
        }
        if (n == "create_pst") {
            Opts o = opts(c, {});
            need(o, 1, "a table name");
            Pst t;
            t.name = join(scope, o.pos[0]);
            t.where = c.where();
            for (auto& s : o.list("-supplies")) t.supplies.push_back(join(scope, s));
            psts.push_back(t);
            continue;
        }
        if (n == "add_pst_state") {
            Opts o = opts(c, {});
            need(o, 1, "a state name");
            std::string tn = join(scope, o.get("-pst"));
            Pst* t = nullptr;
            for (auto& x : psts) if (x.name == tn) t = &x;
            if (!t) throw Error(c.where() + ": no PST " + o.get("-pst"));
            Pst::Row r;
            r.name = o.pos[0];
            r.st = o.list("-state");
            r.where = c.where();
            if (r.st.size() != t->supplies.size())
                throw Error(c.where() + ": PST " + t->name + " has " + std::to_string(t->supplies.size()) +
                            " supplies, the state lists " + std::to_string(r.st.size()));
            t->rows.push_back(r);
            continue;
        }
        // Accepted and not needed for simulation.
        static const std::set<std::string> ignored = {
            "map_isolation_cell", "map_level_shifter_cell",
            "map_retention_cell", "map_power_switch", "use_interface_cell", "name_format", 
            "set_variation", "set_correlated", "bind_checker", 
            
            "save_upf", "sim_assertion_control", "sim_corruption_control",
            "sim_replay_control", "load_simstate_behavior",
            "set_retention_control", "define_power_model", "end_power_model", "apply_power_model", "create_vcm", "define_isolation_cell", "define_level_shifter_cell",
            "define_retention_cell", "define_power_switch_cell", "define_always_on_cell", "define_diode_clamp",
            "map_retention_clamp_cell", "map_repeater_cell", "create_abstract_power_source", "set_pin_related_supply",
            "set_related_supply_net", "query_upf", "add_parameter"};
        if (ignored.count(n)) {
            warnings.push_back(c.where() + ": " + n + " is accepted and has no effect in simulation");
            continue;
        }
        throw Error(c.where() + ": unknown UPF command " + n);
    }
}

// ------------------------------------------------------ design boundaries
// What UPF needs to know about the elaborated design: instance paths and
// the port connections between a parent and a child instance.
struct PortSite {
    std::string inst;    // the child instance, simulator path ("u_dut.u_core")
    std::string port;    // the child's port name
    int dir = 0;         // 0 input, 1 output, 2 inout
    int width = 1;
    std::string where;   // source location, for messages
};

// Which domain an instance (or a process or signal scope) belongs to: the
// domain with the longest element path that contains it. "" for none.
struct DomainMap {
    std::vector<std::pair<std::string, std::string>> el;   // simulator path -> domain
    std::vector<std::string> excl;
    void build(const Model& m) {
        el.clear();
        for (auto& dn : m.domainOrder) {
            const Domain& d = m.domains.at(dn);
            for (auto& e : d.elements) el.push_back({m.design(e), dn});
            for (auto& e : d.exclude) excl.push_back(m.design(e));
        }
    }
    static bool within(const std::string& path, const std::string& e) {
        if (e.empty()) return true;
        return path == e || (path.size() > e.size() && path.compare(0, e.size(), e) == 0 &&
                             (path[e.size()] == '.' || path[e.size()] == '['));
    }
    std::string of(const std::string& path) const {
        size_t best = 0;
        std::string d;
        bool found = false;
        for (auto& [e, dn] : el)
            if (within(path, e) && (!found || e.size() >= best)) {
                best = e.size();
                d = dn;
                found = true;
            }
        for (auto& x : excl)
            if (within(path, x) && (!found || x.size() > best)) return "";
        return d;
    }
};

// Does a strategy's element list name this port (or its instance)?
inline bool namesPort(const Model& m, const std::vector<std::string>& els, const PortSite& p) {
    for (auto& e : els) {
        std::string d = m.design(e);
        if (d == p.inst + "." + p.port || DomainMap::within(p.inst, d)) return true;
    }
    return false;
}

// A port on the interface of a domain, seen from that domain: an input or
// an output of the domain, on its upper (the domain's own boundary
// instance) or lower boundary (a child instance in another domain).
struct Crossing {
    const PortSite* p;
    std::string from, to;   // driving and receiving domain ("" = no domain: always on)
};
inline std::string parentOf(const std::string& inst) {
    size_t d = inst.rfind('.');
    return d == std::string::npos ? "" : inst.substr(0, d);
}
inline std::vector<Crossing> crossings(const DomainMap& dm, const std::vector<PortSite>& sites) {
    std::vector<Crossing> r;
    for (auto& p : sites) {
        if (p.dir == 2) continue;   // inout ports are never isolated (6.48)
        std::string child = dm.of(p.inst), parent = dm.of(parentOf(p.inst));
        if (child == parent) continue;
        Crossing c{&p, p.dir == 1 ? child : parent, p.dir == 1 ? parent : child};
        r.push_back(c);
    }
    return r;
}

// The filters every interface-cell strategy shares (6.48, 6.49, 6.52):
// -applies_to, -applies_to_boundary, -elements, -exclude_elements, -source
// and -sink, for a strategy of `domain` at crossing c.
inline std::string supplyOfDomain(const Model& m, const std::string& dom, bool functional = true) {
    return dom.empty() ? std::string("<always-on>") : m.eqOf(m.primaryOf(dom), functional);
}
inline bool strategyApplies(const Model& m, const std::string& domain, int appliesTo, int boundary,
                            const std::vector<std::string>& elements, const std::vector<std::string>& exclude,
                            const std::string& source, const std::string& sink, const Crossing& c) {
    bool out = c.from == domain, in = c.to == domain;
    if (!out && !in) return false;
    if (appliesTo == 0 && !in) return false;
    if (appliesTo == 1 && !out) return false;
    // upper boundary: a port of the domain's own instance; lower: the
    // HighConn of a port of a child instance that is in another domain
    bool upper = (c.p->dir == 1) == out;
    if (boundary == 1 && upper) return false;
    if (boundary == 2 && !upper) return false;
    if (!elements.empty() && !namesPort(m, elements, *c.p)) return false;
    if (!exclude.empty() && namesPort(m, exclude, *c.p)) return false;
    auto matches = [&](const std::string& filter, const std::string& dom) {
        if (filter.empty()) return true;
        if (m.domains.count(filter)) return filter == dom || m.eqOf(m.primaryOf(filter)) == supplyOfDomain(m, dom);
        return m.eqOf(filter) == supplyOfDomain(m, dom);
    };
    return matches(source, c.from) && matches(sink, c.to);
}
// Does an isolation strategy apply to crossing c?
inline bool isoApplies(const Model& m, const Isolation& is, const Crossing& c) {
    if (!strategyApplies(m, is.domain, is.appliesTo, is.boundary, is.elements, is.exclude, is.source, is.sink, c)) return false;
    if (is.diffSupplyOnly && supplyOfDomain(m, c.from) == supplyOfDomain(m, c.to)) return false;
    return true;
}
inline bool lsApplies(const Model& m, const LevelShifter& l, const Crossing& c) {
    return strategyApplies(m, l.domain, l.appliesTo, l.boundary, l.elements, l.exclude, l.source, l.sink, c);
}
inline bool repApplies(const Model& m, const Repeater& r, const Crossing& c) {
    return strategyApplies(m, r.domain, r.appliesTo, r.boundary, r.elements, r.exclude, r.source, r.sink, c);
}

// ------------------------------------------------------------ static checks
// Checks for mlint (and msim at load): the model is complete, its names
// exist in the design, and every crossing from a domain that can be off to
// one that can be on is isolated and every crossing between different
// voltages has a level shifter.
struct Finding {
    std::string rule, where, text;
};
// Errors of the power intent that do not need the design's ports: a
// subdomain whose primary supply is not equivalent to its composite
// domain's (6.17 c); an element of a required retention list that is not
// retained while another element of its domain is (6.54). `modelOf` gives
// the model of an instance (for the UPF_retention attribute).
inline std::vector<Finding> intentErrors(const Model& m, const DomainMap& dm,
                                         const std::function<std::string(const std::string&)>& modelOf) {
    std::vector<Finding> out;
    for (auto& [cn, cd] : m.composites) {
        if (!m.setAlias.count(cn + ".primary")) continue;
        std::string want = m.eqOf(cn + ".primary");
        for (auto& sub : m.domainsOf(cn)) {
            if (!m.domains.count(sub)) continue;
            std::string have = m.primaryOf(sub);
            auto st = m.sets.find(have);
            if (st == m.sets.end() || st->second.fn.empty()) continue;   // no supply associated yet
            if (m.eqOf(have) != want)
                out.push_back({"UPF007", cd.where, "composite domain " + cn + ": the primary supply of subdomain " + sub + " (" + have +
                                                   ") is not equivalent to " + m.setOf(cn + ".primary") + " (6.17 c)"});
        }
    }
    auto retained = [&](const std::string& path, const std::string& dom) {
        for (auto& r : m.rets) {
            if (r.noRet || r.domain != dom) continue;
            bool in = r.elements.empty();
            for (auto& e : r.elements) in |= DomainMap::within(path, m.design(e));
            for (auto& e : r.exclude) if (DomainMap::within(path, m.design(e))) in = false;
            if (in) return true;
        }
        return false;
    };
    for (auto& [ln, rl] : m.retLists) {
        if (!rl.required) continue;
        for (auto& e : rl.elements) {
            std::string path = m.design(e);
            bool excluded = false;
            for (auto& x : rl.exclude) if (DomainMap::within(path, m.design(x))) excluded = true;
            if (excluded) continue;
            if (!rl.appliesTo.empty()) {
                std::string a = m.designAttr(path, modelOf ? modelOf(path) : std::string(), "UPF_retention");
                bool keep = rl.appliesTo == "required" ? a == "required" : rl.appliesTo == "optional" ? a == "optional"
                          : rl.appliesTo == "not_required" ? a != "required" : a != "optional";
                if (!keep) continue;
            }
            std::string dom = dm.of(path);
            if (dom.empty()) continue;
            bool any = false;
            for (auto& r : m.rets) if (!r.noRet && r.domain == dom) any = true;
            if (any && !retained(path, dom))
                out.push_back({"UPF007", rl.where, "retention list " + ln + ": " + path + " is not retained, and other elements of " + dom + " are (6.54)"});
        }
    }
    return out;
}
struct DesignView {
    std::map<std::string, std::string> models;   // instance (simulator path) -> its module
    std::set<std::string> instances;   // simulator paths of every instance (and generate block)
    std::set<std::string> signals;     // simulator names of every signal
    std::vector<PortSite> ports;
};

// The supply net a supply set function reaches, followed through
// connect_supply_net to its union with other nets and ports.
struct SupplyGraph {
    std::map<std::string, std::string> up;
    std::string find(std::string n) {
        while (up.count(n) && up[n] != n) n = up[n];
        return n;
    }
    void unite(const std::string& a, const std::string& b) {
        std::string x = find(a), y = find(b);
        if (x != y) up[x] = y;
    }
    void build(const Model& m) {
        for (auto& [net, ps] : m.connects)
            for (auto& p : ps) unite(p, net);
        for (auto& e : m.eqNets)   // electrically equivalent supplies are one subnet (6.47, 9.1)
            if (!e.functionOnly) for (auto& n : e.names) unite(n, e.names[0]);
        for (auto& [n, sw] : m.switches) {
            if (!sw.outNet.empty()) unite(n + "/" + sw.outPort, sw.outNet);
            for (auto& [p, net] : sw.ins)
                if (!net.empty()) unite(n + "/" + p, net);
        }
    }
};

// Can a supply set be off, and which voltages can its power have? From the
// PSTs and power states; a supply driven by a switch can always be off.
struct SupplyInfo {
    bool switchable = false;
    std::set<double> volts;
};
inline std::map<std::string, SupplyInfo> supplyInfo(const Model& m, SupplyGraph& g) {
    std::map<std::string, SupplyInfo> r;   // keyed by supply subnet root
    for (auto& [n, sw] : m.switches)
        if (!sw.outNet.empty()) r[g.find(sw.outNet)].switchable = true;
    for (auto& t : m.psts)
        for (auto& row : t.rows)
            for (size_t k = 0; k < t.supplies.size(); k++) {
                std::string root = g.find(t.supplies[k]);
                auto ps = m.portStates.find(t.supplies[k]);
                bool off = row.st[k] == "OFF" || row.st[k] == "off";
                if (ps != m.portStates.end())
                    for (auto& d : ps->second)
                        if (d.name == row.st[k]) {
                            if (d.off) off = true;
                            else r[root].volts.insert(d.vmin);
                        }
                if (off) r[root].switchable = true;
            }
    for (auto& [sn, s] : m.sets) {
        auto pw = s.fn.find("power");
        if (pw == s.fn.end()) continue;
        std::string root = g.find(pw->second);
        for (auto& ps : s.states) {
            if (ps.name == "OFF" || ps.simstate == SS_CORRUPT) r[root].switchable = true;
            std::function<void(const ExP&)> walk = [&](const ExP& e) {
                if (!e) return;
                if (e->k == Ex::Eq && e->kids.size() == 2 && e->kids[1]->k == Ex::Tuple &&
                    e->kids[0]->k == Ex::Id && e->kids[0]->s == "power") {
                    auto& tu = e->kids[1]->kids;
                    if (!tu.empty() && tu[0]->k == Ex::Id && tu[0]->s == "OFF") r[root].switchable = true;
                    if (tu.size() > 1) r[root].volts.insert(tu[1]->real);
                }
                if (e->k == Ex::Eq && e->kids.size() == 2 && e->kids[1]->k == Ex::Id && e->kids[1]->s == "OFF" &&
                    e->kids[0]->k == Ex::Id && e->kids[0]->s == "power")
                    r[root].switchable = true;
                for (auto& k : e->kids) walk(k);
            };
            walk(ps.supplyExpr);
        }
    }
    return r;
}

inline std::vector<Finding> check(const Model& m, const DesignView& dv) {
    std::vector<Finding> f;
    auto add = [&](const char* rule, const std::string& where, const std::string& text) {
        f.push_back({rule, where, text});
    };
    DomainMap dm;
    dm.build(m);
    auto modelOf = [&](const std::string& inst) {
        auto it = dv.models.find(inst);
        return it == dv.models.end() ? std::string() : it->second;
    };
    for (auto& e : intentErrors(m, dm, modelOf)) f.push_back(e);
    // UPF001: names that are not in the design
    for (auto& dn : m.domainOrder) {
        const Domain& d = m.domains.at(dn);
        if (d.elements.empty()) add("UPF001", d.where, "power domain " + dn + " has no -elements");
        for (auto& e : d.elements) {
            std::string p = m.design(e);
            if (!p.empty() && !dv.instances.count(p))
                add("UPF001", d.where, "power domain " + dn + ": no instance '" + p + "' in the design");
        }
    }
    auto sigExists = [&](const std::string& upfName) {
        return dv.signals.count(m.design(upfName)) != 0 || m.logicNets.count(upfName) || m.logicPorts.count(upfName);
    };
    for (auto& lc : m.logicConns) {
        if (!sigExists(lc.net))
            add("UPF001", lc.where, "connect_logic_net: no logic net '" + m.design(lc.net) + "' in the design or made by create_logic_net");
        if (!sigExists(lc.port))
            add("UPF001", lc.where, "connect_logic_net: no port '" + m.design(lc.port) + "' in the design or made by create_logic_port");
    }
    for (auto& rp : m.reps)
        if (!m.domains.count(rp.supply) && !m.sets.count(m.setOf(rp.supply)))
            add("UPF001", rp.where, "repeater " + rp.name + ": no supply set '" + rp.supply + "'");
    for (auto& is : m.isos) {
        if (is.noIso) continue;
        for (auto& s : is.signals)
            if (!sigExists(s)) add("UPF001", is.where, "isolation " + is.name + ": no signal '" + m.design(s) + "' in the design");
    }
    for (auto& rt : m.rets) {
        if (!rt.save.net.empty() && !sigExists(rt.save.net))
            add("UPF001", rt.where, "retention " + rt.name + ": no signal '" + m.design(rt.save.net) + "' in the design");
        if (!rt.restore.net.empty() && rt.restore.net != rt.save.net && !sigExists(rt.restore.net))
            add("UPF001", rt.where, "retention " + rt.name + ": no signal '" + m.design(rt.restore.net) + "' in the design");
    }
    for (auto& [n, sw] : m.switches)
        for (auto& [p, net] : sw.ctrls)
            if (!net.empty() && !sigExists(net))
                add("UPF001", sw.where, "power switch " + n + ": no control signal '" + m.design(net) + "' in the design");
    // UPF002: a domain whose primary supply has no power function
    SupplyGraph g;
    g.build(m);
    std::set<std::string> known;
    for (auto& p : m.ports) known.insert(p);
    for (auto& n : m.nets) known.insert(n);
    for (auto& [n, sw] : m.switches) {
        if (!sw.outPort.empty()) known.insert(n + "/" + sw.outPort);
        for (auto& [p, net] : sw.ins) known.insert(n + "/" + p);
    }
    for (auto& dn : m.domainOrder) {
        std::string ss = m.primaryOf(dn);
        auto it = m.sets.find(ss);
        if (it == m.sets.end() || !it->second.fn.count("power")) {
            add("UPF002", m.domains.at(dn).where, "power domain " + dn + ": primary supply " + ss + " has no power function");
            continue;
        }
        for (auto& [fn, net] : it->second.fn)
            if (!known.count(net)) add("UPF002", it->second.where, "supply set " + ss + ": function " + fn + " names '" + net + "', which is not a supply net or port");
    }
    for (auto& [n, sw] : m.switches)
        for (auto& [p, net] : sw.ins)
            if (!net.empty() && !known.count(net))
                add("UPF002", sw.where, "power switch " + n + ": input " + p + " names '" + net + "', which is not a supply net or port");
    // UPF003 isolation / UPF004 level shifting at every domain crossing
    auto info = supplyInfo(m, g);
    auto powerRoot = [&](const std::string& dom) -> std::string {
        if (dom.empty()) return "";
        auto it = m.sets.find(m.primaryOf(dom));
        if (it == m.sets.end()) return "";
        auto pw = it->second.fn.find("power");
        return pw == it->second.fn.end() ? "" : g.find(pw->second);
    };
    auto canBeOff = [&](const std::string& dom) {
        std::string r = powerRoot(dom);
        auto it = info.find(r);
        return !r.empty() && it != info.end() && it->second.switchable;
    };
    for (const Crossing& c : crossings(dm, dv.ports)) {
        std::string what = c.p->inst + "." + c.p->port + " (" + (c.from.empty() ? "always-on" : c.from) + " -> " +
                           (c.to.empty() ? "always-on" : c.to) + ")";
        bool iso = false, noIso = false;
        for (auto& is : m.isos)
            if (isoApplies(m, is, c)) (is.noIso ? noIso : iso) = true;
        if (m.portAttr(c.p->inst, modelOf(c.p->inst), c.p->port, c.p->dir, "UPF_is_isolated") == "TRUE" ||
            m.portAttr(c.p->inst, modelOf(c.p->inst), c.p->port, c.p->dir, "UPF_is_analog") == "TRUE")
            noIso = true;
        if (canBeOff(c.from) && !iso && !noIso && powerRoot(c.from) != powerRoot(c.to))
            add("UPF003", c.p->where, "no isolation on " + what + ": " + c.from +
                                          " can be off while the receiver is on, so it sees corrupted values");
        if (iso && !c.from.empty() && !canBeOff(c.from))
            add("UPF005", c.p->where, "isolation on " + what + ", but " + c.from + " is never off in any power state");
        std::string ra = powerRoot(c.from), rb = powerRoot(c.to);
        if (ra.empty() || rb.empty() || ra == rb) continue;
        auto va = info[ra].volts, vb = info[rb].volts;
        bool differ = false;
        for (double x : va)
            for (double y : vb)
                if (std::fabs(x - y) > 1e-6) differ = true;
        bool ls = false;
        for (auto& l : m.lss)
            if (lsApplies(m, l, c) && !l.noShift) ls = true;
        if (differ && !ls)
            add("UPF004", c.p->where, "no level shifter on " + what + ": the supplies can be at different voltages");
    }
    // UPF006: a PST state that names an undefined port state
    for (auto& t : m.psts)
        for (auto& row : t.rows)
            for (size_t k = 0; k < t.supplies.size(); k++) {
                const std::string& s = row.st[k];
                if (s == "OFF" || s == "off" || s == "*") continue;
                auto ps = m.portStates.find(t.supplies[k]);
                bool ok = false;
                if (ps != m.portStates.end())
                    for (auto& d : ps->second) ok |= d.name == s;
                if (!ok) add("UPF006", row.where, "PST " + t.name + " state " + row.name + ": " + t.supplies[k] + " has no port state " + s);
            }
    return f;
}

// Rules, for mlint -list.
inline const std::vector<std::pair<const char*, const char*>>& rules() {
    static const std::vector<std::pair<const char*, const char*>> r = {
        {"UPF001", "a UPF name (domain element, isolation, retention or switch control signal) is not in the design"},
        {"UPF002", "a power domain's primary supply set has no power function, or a supply function names no supply net"},
        {"UPF003", "a port crosses from a domain that can be off to one that can be on, with no isolation strategy"},
        {"UPF004", "a port crosses between supplies that can have different voltages, with no level shifter"},
        {"UPF005", "an isolation strategy on a domain whose supply is never off (redundant isolation)"},
        {"UPF006", "a PST state names a supply port state that was not defined with add_port_state"},
        {"UPF007", "the power intent contradicts itself: a composite domain's subdomain on another supply, a required retention list that is not retained"},
    };
    return r;
}

// The SystemVerilog package UPF (11.2.4) that a testbench imports to drive
// supplies: msim adds it when a UPF file is given. supply_on, supply_off,
// supply_partial_on, set_supply_state, set_power_state and get_supply_value
// are run by the simulator; the others are ordinary functions of these
// types.
inline const char* svPackage() {
    return R"SV(
package UPF;
  typedef string upfStringT;
  typedef int upfIntegerT;
  typedef bit upfBooleanT;
  typedef logic upfLogicT;
  typedef real upfRealT;
  typedef int upfHandleT;
  typedef enum {OFF, FULL_ON, PARTIAL_ON, UNDETERMINED} upfSupplyStateE;
  typedef struct packed { upfSupplyStateE state; int voltage; } upfSupplyTypeT;
  typedef upfSupplyStateE state;
  typedef upfSupplyTypeT supply_net_type;
  function automatic upfBooleanT supply_on(string supply_name, real value = 1.0); return 1; endfunction
  function automatic upfBooleanT supply_off(string supply_name); return 1; endfunction
  function automatic upfBooleanT supply_partial_on(string supply_name, real value = 1.0); return 1; endfunction
  function automatic upfBooleanT set_supply_state(string object_name, upfSupplyStateE supply_state); return 1; endfunction
  function automatic upfBooleanT set_power_state(string object, string power_state); return 1; endfunction
  function automatic upfSupplyTypeT get_supply_value(string name); return '0; endfunction
  function automatic real get_supply_voltage(upfSupplyTypeT arg); return arg.voltage / 1.0e6; endfunction
  function automatic upfBooleanT get_supply_on_state(upfSupplyTypeT arg); return arg.state == FULL_ON; endfunction
  function automatic upfSupplyStateE get_supply_state(upfSupplyTypeT arg); return arg.state; endfunction
endpackage
)SV";
}

}  // namespace upf
