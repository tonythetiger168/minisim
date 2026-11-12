// saif_fwd.hpp - forward SAIF files (IEEE 1801-2024 Annex J.4, J.5) and the
// backward SAIF they ask for.
//
// A library forward SAIF file (J.4) names, for each library cell, the state
// conditions (COND), edges (RISE_FALL) and input pins (IOPATH) its pins'
// toggles are to be split by, and the states its leakage time is to be split
// by. backward() walks the waveform of every instance of such a cell and
// writes the state- and path-dependent attributes of J.3.6 to J.3.10.
//
// An RTL forward SAIF file (J.5) lists the synthesis invariant points of an
// RTL design and the gate-level names they map to. rtlBackward() writes the
// activity of just those points, under the mapped names, with a
// VIRTUAL_INSTANCE (J.3.11) for a port mapping that carries a type.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "saif.hpp"

namespace saif {

// ------------------------------------------------------------ conditions

// cond_expr (J.3.6): pin names, ! * ^ |, parentheses; ! binds tightest, then
// *, then ^, then |.
struct Cond {
    struct Node {
        char op = 0;   // 'p' pin, '!', '*', '^', '|', '1' / '0' constant
        int a = -1, b = -1;
        std::string pin;
    };
    std::vector<Node> n;
    int root = -1;
    std::string text;   // as written, spaces folded

    // 0, 1, or 2 (unknown), given each pin's value '0' '1' or other.
    template<typename V>
    int eval(int i, const V& val) const {
        const Node& x = n[(size_t)i];
        switch (x.op) {
            case 'p': {
                char c = val(x.pin);
                return c == '0' ? 0 : c == '1' ? 1 : 2;
            }
            case '1': return 1;
            case '0': return 0;
            case '!': {
                int v = eval(x.a, val);
                return v == 2 ? 2 : !v;
            }
            case '*': {
                int l = eval(x.a, val), r = eval(x.b, val);
                if (l == 0 || r == 0) return 0;
                return l == 1 && r == 1 ? 1 : 2;
            }
            case '|': {
                int l = eval(x.a, val), r = eval(x.b, val);
                if (l == 1 || r == 1) return 1;
                return l == 0 && r == 0 ? 0 : 2;
            }
            default: {   // '^'
                int l = eval(x.a, val), r = eval(x.b, val);
                return l == 2 || r == 2 ? 2 : l ^ r;
            }
        }
    }
    template<typename V>
    int eval(const V& val) const { return eval(root, val); }
    void pins(std::vector<std::string>& out) const {
        for (auto& x : n)
            if (x.op == 'p' && std::find(out.begin(), out.end(), x.pin) == out.end()) out.push_back(x.pin);
    }
};

// Reads a cond_expr from the lexer's text, at its current position.
struct CondParser {
    SaifLex& lx;
    Cond& c;
    void ws() {
        while (lx.p < lx.text.size() && std::isspace((unsigned char)lx.text[lx.p])) {
            if (lx.text[lx.p] == '\n') lx.line++;
            lx.p++;
        }
    }
    char peek() {
        ws();
        return lx.p < lx.text.size() ? lx.text[lx.p] : 0;
    }
    int add(Cond::Node x) {
        c.n.push_back(std::move(x));
        return (int)c.n.size() - 1;
    }
    int primary() {
        char ch = peek();
        if (ch == '(') {
            lx.p++;
            int e = expr();
            if (peek() != ')') lx.fail("expected ) in a COND expression");
            lx.p++;
            return e;
        }
        if (ch == '!') {
            lx.p++;
            return add({'!', primary(), -1, {}});
        }
        std::string id;
        while (lx.p < lx.text.size()) {
            char k = lx.text[lx.p];
            if (k == '\\' && lx.p + 1 < lx.text.size()) {
                id += lx.text[lx.p + 1];
                lx.p += 2;
                continue;
            }
            if (!(std::isalnum((unsigned char)k) || k == '_' || k == '[' || k == ']' || k == '.' || k == '/' ||
                  k == '$'))
                break;
            id += k;
            lx.p++;
        }
        if (id.empty()) lx.fail("expected a pin name in a COND expression");
        if (id == "1" || id == "0" || id == "1'b1" || id == "1'b0") return add({id.back(), -1, -1, {}});
        return add({'p', -1, -1, id});
    }
    int level(int l) {
        static const char ops[] = {'|', '^', '*'};
        if (l == 3) return primary();
        int a = level(l + 1);
        while (peek() == ops[l]) {
            lx.p++;
            int b = level(l + 1);
            a = add({ops[l], a, b, {}});
        }
        return a;
    }
    int expr() { return level(0); }
    void run() {
        ws();
        size_t from = lx.p;
        c.root = expr();
        std::string t;
        bool sp = false;
        for (size_t i = from; i < lx.p; i++) {
            char k = lx.text[i];
            if (std::isspace((unsigned char)k)) {
                sp = !t.empty();
                continue;
            }
            if (sp) t += ' ';
            sp = false;
            t += k;
        }
        c.text = t;
    }
};

// ------------------------------------------------------- library forward

// path_dep_toggle_directive (J.4.6): input pin lists, and IOPATH_DEFAULT.
struct PathDir {
    std::vector<std::vector<std::string>> pins;
    bool dflt = false;
    // The item a toggle caused by `pin` counts in: an index, pins.size() for
    // the default, -1 for none.
    int of(const std::string& pin) const {
        for (size_t i = 0; i < pins.size(); i++)
            if (std::find(pins[i].begin(), pins[i].end(), pin) != pins[i].end()) return (int)i;
        return dflt ? (int)pins.size() : -1;
    }
};

// One COND (or COND_DEFAULT) of a state-dependent or SDPD toggle directive
// (J.4.5, J.4.7), or of a timing directive (J.4.4).
struct CondItem {
    std::optional<Cond> cond;   // none: COND_DEFAULT
    bool riseFall = false;
    std::optional<PathDir> path;
};

// A port directive (J.4.8): conditions (with or without paths), or paths only.
struct Directive {
    std::vector<CondItem> items;
    std::optional<PathDir> pathOnly;
};

struct CellDecl {
    std::string name;
    std::vector<std::pair<std::string, std::vector<Directive>>> ports;   // in file order
    std::vector<Directive> leakage;                                      // timing directives
};

struct Forward {
    std::string path;
    bool lib = false;
    std::string library;                                // LIBRARY name
    std::map<std::string, CellDecl> cells;              // by cell (module) name
    // J.5: the instance tree
    struct Inst {
        std::string design, name;
        struct Port { std::string rtl, mapped, type; };
        std::vector<Port> ports;
        std::vector<std::pair<std::string, std::string>> nets;
        std::vector<Inst> kids;
    };
    std::vector<Inst> insts;
    char divider = '.';
};

struct ForwardParser {
    SaifLex lx;
    Forward& f;

    // The next token without consuming it.
    int peek(std::string& t) {
        size_t p = lx.p;
        int line = lx.line;
        int k = lx.next(t);
        lx.p = p;
        lx.line = line;
        return k;
    }
    std::string name(const std::string& raw) const {
        if (f.divider == '.') return raw;
        std::string s = raw;
        for (char& c : s)
            if (c == f.divider) c = '.';
        return s;
    }
    static bool keyword(const std::string& t) {
        return t == "COND" || t == "COND_DEFAULT" || t == "IOPATH" || t == "IOPATH_DEFAULT" || t == "RISE_FALL";
    }
    // After "(IOPATH" or "(IOPATH_DEFAULT" has been peeked at: the list, to its ')'.
    PathDir pathDir() {
        PathDir d;
        std::string t;
        for (;;) {
            int k = lx.next(t);
            if (k == ')') break;
            if (k != 'w') lx.fail("expected IOPATH");
            if (t == "IOPATH_DEFAULT") {
                d.dflt = true;
                continue;
            }
            if (t != "IOPATH") lx.fail("expected IOPATH, not " + t);
            std::vector<std::string> pins;
            for (;;) {
                k = peek(t);
                if (k != 'w' || keyword(t)) break;
                lx.next(t);
                pins.push_back(t);
            }
            if (pins.empty()) lx.fail("IOPATH needs a port name (J.4.6)");
            d.pins.push_back(pins);
        }
        return d;
    }
    // After '(' of a directive: COND items, or IOPATH items.
    Directive directive(bool timing) {
        Directive d;
        std::string t;
        if (peek(t) == 'w' && (t == "IOPATH" || t == "IOPATH_DEFAULT")) {
            if (timing) lx.fail("a LEAKAGE directive takes COND items (J.4.4)");
            d.pathOnly = pathDir();
            return d;
        }
        for (;;) {
            int k = lx.next(t);
            if (k == ')') break;
            if (k != 'w' || (t != "COND" && t != "COND_DEFAULT")) lx.fail("expected COND or COND_DEFAULT");
            CondItem it;
            if (t == "COND") {
                it.cond.emplace();
                CondParser cp{lx, *it.cond};
                cp.run();
            }
            if (peek(t) == 'w' && t == "RISE_FALL") {
                if (timing) lx.fail("RISE_FALL is for toggle directives, not LEAKAGE (J.4.4)");
                lx.next(t);
                it.riseFall = true;
            }
            if (peek(t) == '(') {
                if (timing) lx.fail("IOPATH is for toggle directives, not LEAKAGE (J.4.4)");
                lx.next(t);
                it.path = pathDir();
            }
            d.items.push_back(std::move(it));
        }
        if (d.items.empty()) lx.fail("a directive needs a COND item");
        for (size_t i = 0; i + 1 < d.items.size(); i++)
            if (!d.items[i].cond) lx.fail("COND_DEFAULT must be the last item (J.4.4, J.4.5)");
        return d;
    }
    void module() {
        std::string t;
        if (lx.next(t) != 'w') lx.fail("expected a cell name after MODULE");
        CellDecl c;
        c.name = t;
        for (;;) {
            int k = lx.next(t);
            if (k == ')') break;
            if (k != '(' || lx.next(t) != 'w') lx.fail("expected (PORT or (LEAKAGE in MODULE " + c.name);
            if (t == "PORT") {
                for (;;) {
                    k = lx.next(t);
                    if (k == ')') break;
                    if (k != '(' || lx.next(t) != 'w') lx.fail("expected (pin in PORT of " + c.name);
                    std::string pin = t;
                    std::vector<Directive> ds;
                    for (;;) {
                        k = lx.next(t);
                        if (k == ')') break;
                        if (k != '(') lx.fail("expected a directive for pin " + pin);
                        ds.push_back(directive(false));
                    }
                    c.ports.push_back({pin, std::move(ds)});
                }
            } else if (t == "LEAKAGE") {
                for (;;) {
                    k = lx.next(t);
                    if (k == ')') break;
                    if (k != '(') lx.fail("expected a COND list in LEAKAGE of " + c.name);
                    c.leakage.push_back(directive(true));
                }
            } else {
                lx.fail("MODULE takes PORT and LEAKAGE, not " + t + " (J.4.8)");
            }
        }
        f.cells[c.name] = std::move(c);
    }
    void rtlInst(std::vector<Forward::Inst>& into) {
        Forward::Inst in;
        std::string t;
        if (lx.next(t) != 'w') lx.fail("expected an instance name");
        if (lx.quoted) {
            in.design = t;
            if (lx.next(t) != 'w') lx.fail("expected an instance name");
        }
        in.name = name(t);
        for (;;) {
            int k = lx.next(t);
            if (k == ')') break;
            if (k != '(' || lx.next(t) != 'w') lx.fail("expected (PORT, (NET or (INSTANCE in " + in.name);
            if (t == "INSTANCE") {
                rtlInst(in.kids);
                continue;
            }
            if (t != "PORT" && t != "NET") lx.fail("an RTL forward instance takes PORT, NET and INSTANCE (J.5.4)");
            bool port = t == "PORT";
            for (;;) {
                k = lx.next(t);
                if (k == ')') break;
                if (k != '(') lx.fail("expected (rtl_name mapped_name)");
                std::vector<std::string> w = SaifParser::words(lx);
                if (w.size() < 2 || w.size() > (port ? 3u : 2u))
                    lx.fail(port ? "a port mapping is (rtl_name mapped_name [type]) (J.5.3)"
                                 : "a net mapping is (rtl_name mapped_name) (J.5.3)");
                if (port) in.ports.push_back({name(w[0]), name(w[1]), w.size() > 2 ? w[2] : ""});
                else in.nets.push_back({name(w[0]), name(w[1])});
            }
        }
        into.push_back(std::move(in));
    }
    void run() {
        std::ifstream in(f.path);
        if (!in) throw Error("cannot read " + f.path);
        std::stringstream ss;
        ss << in.rdbuf();
        lx.text = ss.str();
        lx.path = f.path;
        std::string t;
        if (lx.next(t) != '(' || lx.next(t) != 'w' || t != "SAIFILE") lx.fail("not a SAIF file");
        bool forward = false;
        for (;;) {
            int k = lx.next(t);
            if (k == ')' || k == 0) break;
            if (k != '(') lx.fail("expected (");
            lx.next(t);
            if (t == "SAIFVERSION") {
                auto w = SaifParser::words(lx);
                if (w.size() > 1 && (w[1] == "lib" || w[1] == "LIB")) f.lib = true;
            } else if (t == "DIRECTION") {
                auto w = SaifParser::words(lx);
                forward = !w.empty() && (w[0] == "forward" || w[0] == "FORWARD");
                if (!forward) lx.fail("not a forward SAIF file: DIRECTION is not \"forward\" (J.4.3.2, J.5.2.4)");
            } else if (t == "DIVIDER") {
                auto w = SaifParser::words(lx);
                if (!w.empty() && !w[0].empty()) {
                    if (w[0] != "/" && w[0] != ".") lx.fail("the divider is / or . (J.4.3.8)");
                    f.divider = w[0][0];
                }
            } else if (t == "LIBRARY") {
                f.lib = true;
                if (lx.next(t) != 'w') lx.fail("expected the library name");
                f.library = t;
                for (;;) {
                    k = lx.next(t);
                    if (k == ')') break;
                    if (k == 'w') continue;   // the directory string
                    if (k != '(' || lx.next(t) != 'w' || t != "MODULE") lx.fail("LIBRARY takes MODULE entries (J.4.9)");
                    module();
                }
            } else if (t == "INSTANCE") {
                if (f.lib) lx.fail("a library forward SAIF file has no INSTANCE (J.4.9)");
                rtlInst(f.insts);
            } else {
                SaifParser::skip(lx);
            }
        }
        if (!forward) lx.fail("no (DIRECTION \"forward\") (J.4.3.2, J.5.2.4)");
    }
};

inline Forward readForward(const std::string& path) {
    Forward f;
    f.path = path;
    ForwardParser p{{}, f};
    p.run();
    return f;
}

// ------------------------------------------------- backward, from a library

// What the backward file needs of one cell instance.
struct CellInst {
    std::string path;     // top.u1 (as the waveform names it)
    const CellDecl* cell = nullptr;
    std::map<std::string, bool> output;   // pin -> is an output (from the design)
};

// The state-dependent data of one cell instance, written into its INSTANCE.
struct CellActivity {
    // per port: per directive: per item: per edge (0 any, 1 rise, 2 fall):
    // per path (index, pins.size() default) or 0 when the item has none
    using Counts = std::vector<std::vector<std::vector<std::vector<double>>>>;
    std::map<std::string, Counts> ports;
    // per timing directive: per item: (T1, T0)
    std::vector<std::vector<std::pair<double, double>>> leakage;
};

inline CellActivity cellActivity(const CellInst& ci, const wave::Reader& r, uint64_t start, uint64_t end) {
    const CellDecl& cd = *ci.cell;
    CellActivity act;
    // every pin the cell's directives name
    std::vector<std::string> pins;
    auto addPin = [&](const std::string& p) {
        if (std::find(pins.begin(), pins.end(), p) == pins.end()) pins.push_back(p);
    };
    auto dirPins = [&](const Directive& d) {
        for (auto& it : d.items) {
            if (it.cond) it.cond->pins(pins);
            if (it.path)
                for (auto& l : it.path->pins)
                    for (auto& p : l) addPin(p);
        }
        if (d.pathOnly)
            for (auto& l : d.pathOnly->pins)
                for (auto& p : l) addPin(p);
    };
    for (auto& [p, ds] : cd.ports) {
        addPin(p);
        for (auto& d : ds) dirPins(d);
    }
    for (auto& d : cd.leakage) dirPins(d);
    for (auto& [p, out] : ci.output) addPin(p);   // a toggle on any input pin is a cause
    std::map<std::string, int> idx;
    for (size_t i = 0; i < pins.size(); i++) idx[pins[i]] = (int)i;
    std::vector<const wave::Wave*> wv(pins.size(), nullptr);
    for (size_t i = 0; i < pins.size(); i++) {
        auto it = r.byName.find(ci.path + "." + pins[i]);
        if (it != r.byName.end() && r.waves[(size_t)it->second].width == 1) wv[i] = &r.waves[(size_t)it->second];
    }
    auto isOut = [&](const std::string& p) {
        auto it = ci.output.find(p);
        return it != ci.output.end() && it->second;
    };
    // sizes
    for (auto& [p, ds] : cd.ports) {
        auto& c = act.ports[p];
        for (auto& d : ds) {
            std::vector<std::vector<std::vector<double>>> di;
            if (d.pathOnly) di.push_back({std::vector<double>(d.pathOnly->pins.size() + 1, 0.0)});
            for (auto& it : d.items) {
                size_t np = it.path ? it.path->pins.size() + 1 : 1;
                di.push_back(std::vector<std::vector<double>>(3, std::vector<double>(np, 0.0)));
            }
            c.push_back(std::move(di));
        }
    }
    for (auto& d : cd.leakage) act.leakage.push_back(std::vector<std::pair<double, double>>(d.items.size(), {0.0, 0.0}));

    struct Ev { uint64_t t; int pin; char v; };
    std::vector<Ev> evs;
    for (size_t i = 0; i < pins.size(); i++) {
        if (!wv[i]) continue;
        for (auto& ch : wv[i]->ch) {
            char v = ch.len ? wv[i]->base[ch.off + ch.len - 1] : 'x';
            if (v == 'h' || v == 'H') v = '1';
            else if (v == 'l' || v == 'L') v = '0';
            else if (v != '0' && v != '1') v = 'x';
            evs.push_back({ch.t, (int)i, v});
        }
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    std::vector<char> cur(pins.size(), 'x'), known(pins.size(), 'x');
    auto valOf = [&](const std::vector<char>& st) {
        return [&st, &idx](const std::string& p) {
            auto it = idx.find(p);
            return it == idx.end() ? 'x' : st[(size_t)it->second];
        };
    };
    // the first item whose condition holds; -1 unknown
    auto select = [&](const Directive& d, const std::vector<char>& st) {
        auto val = valOf(st);
        for (size_t i = 0; i < d.items.size(); i++) {
            if (!d.items[i].cond) return (int)i;
            int v = d.items[i].cond->eval(val);
            if (v == 2) return -1;
            if (v == 1) return (int)i;
        }
        return (int)d.items.size();   // none holds and there is no default
    };
    auto accrue = [&](uint64_t from, uint64_t to) {
        if (to <= from) return;
        double dt = (double)(to - from);
        for (size_t k = 0; k < cd.leakage.size(); k++) {
            int s = select(cd.leakage[k], cur);
            if (s < 0) continue;
            for (size_t i = 0; i < cd.leakage[k].items.size(); i++)
                (i == (size_t)s ? act.leakage[k][i].first : act.leakage[k][i].second) += dt;
        }
    };
    // the input event that output toggles are put down to
    std::vector<char> causeState = cur;
    std::vector<int> causePins;
    uint64_t last = start;
    size_t e = 0;
    while (e < evs.size()) {
        uint64_t t = evs[e].t;
        if (t > end) break;
        accrue(std::max(last, start), t);
        last = std::max(last, t);
        std::vector<char> before = cur;
        std::vector<std::pair<int, bool>> tog;   // pin, rise
        size_t g = e;
        for (; g < evs.size() && evs[g].t == t; g++) cur[(size_t)evs[g].pin] = evs[g].v;
        for (size_t i = 0; i < pins.size(); i++) {
            char v = cur[i];
            if (v == 'x' || v == before[i]) continue;
            if (known[i] != 'x' && known[i] != v) tog.push_back({(int)i, v == '1'});
            known[i] = v;
        }
        e = g;
        // inputs first: their state is the one just before them (J.3.7)
        std::vector<int> inNow;
        for (auto& [pi, rise] : tog)
            if (!isOut(pins[(size_t)pi])) inNow.push_back(pi);
        if (!inNow.empty()) {
            causeState = before;
            causePins = inNow;
        }
        for (auto& [pi, rise] : tog) {
            const std::string& p = pins[(size_t)pi];
            auto pit = act.ports.find(p);
            if (pit == act.ports.end()) continue;
            bool out = isOut(p);
            const std::vector<char>& st = out ? causeState : before;
            const auto& ds = std::find_if(cd.ports.begin(), cd.ports.end(), [&](auto& x) { return x.first == p; })->second;
            for (size_t k = 0; k < ds.size(); k++) {
                const Directive& d = ds[k];
                auto pathIdx = [&](const PathDir& pd) {
                    if (!out) return -1;
                    for (int c : causePins) {
                        int i = pd.of(pins[(size_t)c]);
                        if (i >= 0) return i;
                    }
                    return pd.dflt ? (int)pd.pins.size() : -1;
                };
                if (d.pathOnly) {
                    int i = pathIdx(*d.pathOnly);
                    if (i >= 0) pit->second[k][0][0][(size_t)i] += 1;
                    continue;
                }
                int s = select(d, st);
                if (s < 0 || s >= (int)d.items.size()) continue;
                const CondItem& it = d.items[(size_t)s];
                int edge = it.riseFall ? (rise ? 1 : 2) : 0;
                int pi2 = 0;
                if (it.path) {
                    pi2 = pathIdx(*it.path);
                    if (pi2 < 0) continue;
                }
                pit->second[k][(size_t)s][(size_t)edge][(size_t)pi2] += 1;
            }
        }
    }
    accrue(std::max(last, start), std::max(end, last));
    return act;
}

inline std::string fmtNum(double v) {
    char b[64];
    if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%.0f", v);
    else std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// Writes a port's state- and path-dependent attributes (J.3.7 to J.3.9).
inline void writePortDirectives(std::ostream& os, const std::vector<Directive>& ds, const CellActivity::Counts& c,
                                const std::string& ind) {
    for (size_t k = 0; k < ds.size(); k++) {
        const Directive& d = ds[k];
        auto paths = [&](const PathDir& pd, const std::vector<double>& n) {
            std::string s = "(";
            for (size_t i = 0; i < pd.pins.size(); i++) {
                if (i) s += " ";
                s += "IOPATH";
                for (auto& p : pd.pins[i]) s += " " + p;
                s += " (TC " + fmtNum(n[i]) + ")";
            }
            if (pd.dflt) s += std::string(pd.pins.empty() ? "" : " ") + "IOPATH_DEFAULT (TC " + fmtNum(n[pd.pins.size()]) + ")";
            return s + ")";
        };
        if (d.pathOnly) {
            os << "\n" << ind << paths(*d.pathOnly, c[k][0][0]);
            continue;
        }
        os << "\n" << ind << "(";
        bool first = true;
        for (size_t i = 0; i < d.items.size(); i++) {
            const CondItem& it = d.items[i];
            for (int edge : it.riseFall ? std::vector<int>{1, 2} : std::vector<int>{0}) {
                if (!first) os << "\n" << ind << " ";
                first = false;
                os << (it.cond ? "COND " + it.cond->text : std::string("COND_DEFAULT"));
                if (edge) os << (edge == 1 ? " (RISE)" : " (FALL)");
                const auto& n = c[k][i][(size_t)edge];
                if (it.path) os << " " << paths(*it.path, n);
                else os << " (TC " << fmtNum(n[0]) << ")";
            }
        }
        os << ")";
    }
}

inline void writeLeakage(std::ostream& os, const CellDecl& cd, const CellActivity& a, const std::string& ind) {
    if (cd.leakage.empty()) return;
    os << ind << "(LEAKAGE\n";
    for (size_t k = 0; k < cd.leakage.size(); k++) {
        os << ind << " (";
        for (size_t i = 0; i < cd.leakage[k].items.size(); i++) {
            const CondItem& it = cd.leakage[k].items[i];
            if (i) os << "\n" << ind << "  ";
            os << (it.cond ? "COND " + it.cond->text : std::string("COND_DEFAULT")) << " (T1 "
               << fmtNum(a.leakage[k][i].first) << ") (T0 " << fmtNum(a.leakage[k][i].second) << ")";
        }
        os << ")\n";
    }
    os << ind << ")\n";
}

// ----------------------------------------------------- backward, from RTL

// A backward file holding only the points an RTL forward file lists, under
// their mapped names (J.5). `top` is the waveform's top scope, tried as a
// prefix when an instance name is not found as given.
inline void rtlBackward(const Db& db, const Forward& fw, std::ostream& os, const std::string& top,
                        const std::string& program, const std::string& version, std::vector<std::string>& warnings) {
    // a dotted name, written with the / divider
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '.') { o += '/'; continue; }
            if (c == '[' || c == ']' || c == '/' || c == '(' || c == ')' || c == '\\' || c == '"') o += '\\';
            o += c;
        }
        return o;
    };
    auto unitOf = [&](std::string& unit) {
        static const std::pair<const char*, double> us[] = {{"s", 1}, {"ms", 1e-3}, {"us", 1e-6},
                                                            {"ns", 1e-9}, {"ps", 1e-12}, {"fs", 1e-15}};
        for (auto& [u, m] : us) {
            double r = db.tick / m;
            if (r >= 0.999 && r < 999.5) {
                unit = u;
                return std::round(r);
            }
        }
        unit = "ns";
        return 1.0;
    };
    auto hasScope = [&](const std::string& p) {
        std::string pre = p + ".";
        for (auto& [n, s] : db.sigs)
            if (n.compare(0, pre.size(), pre) == 0) return true;
        return false;
    };
    auto attrs = [&](const Bit& b) {
        return "(T0 " + fmtNum(b.t0) + ") (T1 " + fmtNum(b.t1) + ") (TX " + fmtNum(b.tx) + ") (TC " + fmtNum(b.tc) +
               ") (IG 0)";
    };
    // rtl -> (mapped, bit) pairs: one bit, or every bit of a vector
    auto bitsOf = [&](const std::string& scope, const std::string& rtl, const std::string& mapped) {
        std::vector<std::pair<std::string, const Bit*>> out;
        std::string full = scope + "." + rtl;
        auto it = db.sigs.find(full);
        if (it != db.sigs.end() && it->second.width > 1) {
            bool idx = !mapped.empty() && mapped.back() == ']';
            for (int j = 0; j < it->second.width; j++) {
                std::string m = idx ? mapped : mapped + "[" + std::to_string(it->second.index(j)) + "]";
                out.push_back({m, &db.bits[it->second.base + (uint32_t)j]});
                if (idx) break;
            }
            return out;
        }
        if (const Bit* b = db.find(full)) out.push_back({mapped, b});
        else warnings.push_back(fw.path + ": no signal " + full + " in the waveform");
        return out;
    };
    std::string unit;
    double scale = unitOf(unit);
    std::string design = fw.insts.size() == 1 && !fw.insts[0].design.empty() ? fw.insts[0].design : top;
    os << "(SAIFILE\n(SAIFVERSION \"2.0\")\n(DIRECTION \"backward\")\n(DESIGN \"" << design << "\")\n"
       << "(VENDOR \"minisim\")\n(PROGRAM_NAME \"" << program << "\")\n(VERSION \"" << version << "\")\n"
       << "(DIVIDER / )\n(TIMESCALE " << fmtNum(scale) << " " << unit << ")\n(DURATION " << fmtNum(db.duration)
       << ")\n";
    std::function<void(const Forward::Inst&, const std::string&, int)> emit = [&](const Forward::Inst& in,
                                                                                    const std::string& parent,
                                                                                    int depth) {
        std::string scope = parent.empty() ? in.name : parent + "." + in.name;
        if (parent.empty() && !hasScope(scope) && !top.empty() && hasScope(top + "." + scope)) scope = top + "." + scope;
        if (!hasScope(scope)) warnings.push_back(fw.path + ": no instance " + scope + " in the waveform");
        std::string ind((size_t)depth, ' ');
        os << ind << "(INSTANCE ";
        if (!in.design.empty()) os << "\"" << in.design << "\" ";
        os << esc(in.name) << "\n";
        // a typed port mapping is a virtual instance (J.3.11, J.5.3)
        std::vector<std::pair<const Forward::Inst::Port*, std::vector<std::pair<std::string, const Bit*>>>> virt;
        std::vector<std::pair<std::string, const Bit*>> ports, nets;
        for (auto& p : in.ports) {
            auto b = bitsOf(scope, p.rtl, p.mapped);
            if (p.type.empty()) ports.insert(ports.end(), b.begin(), b.end());
            else virt.push_back({&p, b});
        }
        for (auto& [rtl, mapped] : in.nets) {
            auto b = bitsOf(scope, rtl, mapped);
            nets.insert(nets.end(), b.begin(), b.end());
        }
        if (!ports.empty()) {
            os << ind << " (PORT\n";
            for (auto& [n, b] : ports) os << ind << "  (" << esc(n) << " " << attrs(*b) << ")\n";
            os << ind << " )\n";
        }
        if (!nets.empty()) {
            os << ind << " (NET\n";
            for (auto& [n, b] : nets) os << ind << "  (" << esc(n) << " " << attrs(*b) << ")\n";
            os << ind << " )\n";
        }
        for (auto& [p, bs] : virt)
            for (auto& [n, b] : bs) {
                // "A_reg/QN" names the pin; otherwise the virtual instance's output, Q
                std::string inst = n, pin = "Q";
                size_t d = n.rfind('.');
                if (d != std::string::npos && n.find('[', d) == std::string::npos) {
                    inst = n.substr(0, d);
                    pin = n.substr(d + 1);
                }
                os << ind << " (VIRTUAL_INSTANCE \"" << p->type << "\" " << esc(inst) << "\n" << ind << "  (PORT\n"
                   << ind << "   (" << esc(pin) << " " << attrs(*b) << ")\n" << ind << "  )\n" << ind << " )\n";
            }
        for (auto& k : in.kids) emit(k, scope, depth + 1);
        os << ind << ")\n";
    };
    for (auto& in : fw.insts) emit(in, "", 0);
    os << ")\n";
}

}  // namespace saif
