// liberty.hpp - a reader for Liberty (.lib) cell libraries, shared by msta
// (static timing), msynth (synthesis) and mequiv (equivalence checking).
//
// Two layers:
//   * parse(): the generic syntax, groups / simple attributes / complex
//     attributes, into a lib::Group tree. Nothing is lost here, so a tool
//     that needs an attribute the model below does not keep can still read it.
//   * Library::load(): the parts the tools use. Units are normalized: times
//     in ns, capacitances in pF, areas as written.
//
// Kept: library units, lu_table_template, default_max_transition,
// default_input_pin_cap, cells (area, dont_use, ff, latch, statetable kept as
// the raw group), pins and buses (direction, capacitance, function,
// three_state, clock, max_transition, max_capacitance), timing groups
// (related_pin, timing_sense, timing_type, when, the NLDM tables cell_rise,
// cell_fall, rise_transition, fall_transition, rise_constraint,
// fall_constraint), internal_power and leakage_power groups, wire_load,
// test_cell pins (signal_type). Pin functions parse to an expression tree
// (parseFunction, Expr::eval, Expr::truthTable). CCS and noise groups are
// parsed (they stay in the Group tree) but not modeled.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace liberty {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ------------------------------------------------------------------ syntax

struct Attr {
    std::string name;
    std::vector<std::string> vals;  // simple attribute: one value
    bool complex = false;           // name ( v, v ) ;
    int line = 0;
};

struct Group {
    std::string type;                // "cell"
    std::vector<std::string> names;  // cell ( INV_X1 ) -> {"INV_X1"}
    std::vector<Attr> attrs;
    std::vector<Group> groups;
    int line = 0;

    std::string name() const { return names.empty() ? "" : names[0]; }
    const Attr* attr(const std::string& n) const {
        for (auto& a : attrs) if (a.name == n) return &a;
        return nullptr;
    }
    std::string str(const std::string& n, const std::string& dflt = "") const {
        auto* a = attr(n);
        return a && !a->vals.empty() ? a->vals[0] : dflt;
    }
    bool has(const std::string& n) const { return attr(n) != nullptr; }
    double num(const std::string& n, double dflt) const {
        auto* a = attr(n);
        if (!a || a->vals.empty()) return dflt;
        return std::atof(a->vals[0].c_str());
    }
    std::vector<const Group*> all(const std::string& t) const {
        std::vector<const Group*> r;
        for (auto& g : groups) if (g.type == t) r.push_back(&g);
        return r;
    }
    const Group* first(const std::string& t) const {
        for (auto& g : groups) if (g.type == t) return &g;
        return nullptr;
    }
};

namespace detail {

struct Lexer {
    const std::string& s;
    std::string file;
    size_t p = 0;
    int line = 1;
    // Token kinds: 'w' word or number, 's' quoted string, or the punctuation
    // character itself ( ) { } : ; , and 0 at end of input.
    char kind = 0;
    std::string tok;
    int tokLine = 1;

    Lexer(const std::string& src, std::string f) : s(src), file(std::move(f)) { next(); }

    [[noreturn]] void fail(const std::string& m) const {
        throw Error(file + ":" + std::to_string(tokLine) + ": " + m);
    }
    void skip() {
        for (;;) {
            while (p < s.size() && (isspace((unsigned char)s[p]) || s[p] == '\\')) {
                if (s[p] == '\n') line++;
                p++;  // a backslash here is a line continuation
            }
            if (p + 1 < s.size() && s[p] == '/' && s[p + 1] == '*') {
                p += 2;
                while (p + 1 < s.size() && !(s[p] == '*' && s[p + 1] == '/')) {
                    if (s[p] == '\n') line++;
                    p++;
                }
                p += 2;
                continue;
            }
            if (p + 1 < s.size() && s[p] == '/' && s[p + 1] == '/') {
                while (p < s.size() && s[p] != '\n') p++;
                continue;
            }
            break;
        }
    }
    void next() {
        skip();
        tokLine = line;
        tok.clear();
        if (p >= s.size()) { kind = 0; return; }
        char c = s[p];
        if (c == '"') {
            p++;
            while (p < s.size() && s[p] != '"') {
                if (s[p] == '\\' && p + 1 < s.size() && s[p + 1] == '\n') { p += 2; line++; continue; }
                if (s[p] == '\n') line++;
                tok += s[p++];
            }
            p++;
            kind = 's';
            return;
        }
        if (strchr("(){}:;,", c)) { kind = c; tok = c; p++; return; }
        while (p < s.size()) {
            char d = s[p];
            if (isspace((unsigned char)d) || strchr("(){}:;,\"", d)) break;
            if (d == '\\' && p + 1 < s.size() && (s[p + 1] == '\n' || s[p + 1] == '\r')) break;
            tok += d;
            p++;
        }
        kind = 'w';
    }
};

inline void parseBody(Lexer& L, Group& g);

// After a name: either ": value ;" (simple), or "( args )" followed by "{"
// (group) or ";"/newline (complex attribute).
inline void parseStatement(Lexer& L, Group& parent) {
    std::string name = L.tok;
    int line = L.tokLine;
    L.next();
    if (L.kind == ':') {
        L.next();
        Attr a;
        a.name = name;
        a.line = line;
        std::string v;
        // A value may be an expression of several words (1.2 * VDD).
        while (L.kind == 'w' || L.kind == 's') {
            if (!v.empty()) v += ' ';
            v += L.tok;
            int l = L.tokLine;
            L.next();
            if (L.tokLine != l && L.kind != ';') break;  // missing ';' at end of line
        }
        a.vals.push_back(v);
        parent.attrs.push_back(std::move(a));
        if (L.kind == ';') L.next();
        return;
    }
    if (L.kind != '(') L.fail("expected ':' or '(' after '" + name + "'");
    L.next();
    std::vector<std::string> args;
    std::string cur;
    bool any = false;
    while (L.kind != ')') {
        if (L.kind == 0) L.fail("unterminated '(' after '" + name + "'");
        if (L.kind == ',') {
            args.push_back(cur);
            cur.clear();
            any = false;
        } else {
            if (any) cur += ' ';
            cur += L.tok;
            any = true;
        }
        L.next();
    }
    if (any || !args.empty()) args.push_back(cur);
    L.next();
    if (L.kind == '{') {
        L.next();
        Group g;
        g.type = name;
        g.names = std::move(args);
        g.line = line;
        parseBody(L, g);
        parent.groups.push_back(std::move(g));
        return;
    }
    Attr a;
    a.name = name;
    a.vals = std::move(args);
    a.complex = true;
    a.line = line;
    parent.attrs.push_back(std::move(a));
    if (L.kind == ';') L.next();
}

inline void parseBody(Lexer& L, Group& g) {
    for (;;) {
        if (L.kind == '}') { L.next(); return; }
        if (L.kind == 0) L.fail("missing '}' for " + g.type + " starting at line " + std::to_string(g.line));
        if (L.kind == ';') { L.next(); continue; }
        if (L.kind != 'w') L.fail("unexpected '" + L.tok + "'");
        parseStatement(L, g);
    }
}

}  // namespace detail

// Parses Liberty source into its top-level group ("library").
inline Group parse(const std::string& text, const std::string& file = "<lib>") {
    detail::Lexer L(text, file);
    Group root;
    root.type = "<root>";
    while (L.kind != 0) {
        if (L.kind == ';') { L.next(); continue; }
        if (L.kind != 'w') L.fail("unexpected '" + L.tok + "'");
        detail::parseStatement(L, root);
    }
    for (auto& g : root.groups)
        if (g.type == "library") return g;
    throw Error(file + ": no library group");
}

// ------------------------------------------------------------------ model

// The variables a table index can stand for.
enum class Var { None, InSlew, Load, RelSlew, ConSlew, OutSlew };

inline Var varOf(const std::string& v) {
    if (v == "input_net_transition" || v == "input_transition_time") return Var::InSlew;
    if (v == "total_output_net_capacitance") return Var::Load;
    if (v == "related_pin_transition") return Var::RelSlew;
    if (v == "constrained_pin_transition") return Var::ConSlew;
    if (v == "output_net_transition") return Var::OutSlew;
    return Var::None;
}

inline std::vector<double> numbers(const std::string& s) {
    std::vector<double> r;
    const char* p = s.c_str();
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == '"')) p++;
        if (!*p) break;
        char* e;
        double v = std::strtod(p, &e);
        if (e == p) { p++; continue; }
        r.push_back(v);
        p = e;
    }
    return r;
}

// A lookup table of up to two indexes, values row-major [i1][i2], with
// linear interpolation and extrapolation (as the NLDM model specifies).
struct Table {
    Var var1 = Var::None, var2 = Var::None;
    std::vector<double> idx1, idx2, v;

    bool empty() const { return v.empty(); }
    static double interp1(const std::vector<double>& x, double q, size_t& i, double& t) {
        if (x.size() < 2) { i = 0; t = 0; return 0; }
        size_t k = 0;
        while (k + 2 < x.size() && q > x[k + 1]) k++;
        i = k;
        double d = x[k + 1] - x[k];
        t = d == 0 ? 0 : (q - x[k]) / d;
        return t;
    }
    // Looks the table up with the value of each variable; a and b are the
    // values for the two variables the caller has, matched by kind.
    double at(double q1, double q2) const {
        if (v.empty()) return 0;
        if (v.size() == 1) return v[0];
        size_t n2 = idx2.empty() ? 1 : idx2.size();
        if (idx2.empty() || idx2.size() == 1) {
            if (idx1.size() < 2) return v[0];
            size_t i;
            double t;
            interp1(idx1, q1, i, t);
            return v[i * n2] + t * (v[(i + 1) * n2] - v[i * n2]);
        }
        if (idx1.size() < 2) {
            size_t j;
            double u;
            interp1(idx2, q2, j, u);
            return v[j] + u * (v[j + 1] - v[j]);
        }
        size_t i, j;
        double t, u;
        interp1(idx1, q1, i, t);
        interp1(idx2, q2, j, u);
        double a = v[i * n2 + j], b = v[i * n2 + j + 1];
        double c = v[(i + 1) * n2 + j], d = v[(i + 1) * n2 + j + 1];
        return (1 - t) * (1 - u) * a + (1 - t) * u * b + t * (1 - u) * c + t * u * d;
    }
    // Delay/transition tables: input slew and output load.
    double lookup(double inSlew, double load) const {
        auto val = [&](Var x) { return x == Var::Load ? load : inSlew; };
        return at(val(var1), val(var2));
    }
    // Constraint tables: constrained (data) pin slew and related (clock) pin slew.
    double constraint(double dataSlew, double clkSlew) const {
        auto val = [&](Var x) { return x == Var::RelSlew ? clkSlew : dataSlew; };
        return at(val(var1), val(var2));
    }
};

// ------------------------------------------------------- pin functions

// A Liberty boolean expression: function, three_state, next_state,
// clocked_on, when ... Operators by precedence: ' and ! (not), ^ (xor),
// & * and juxtaposition (and), | + (or). Constants 0 and 1.
struct Expr {
    enum Kind { Const, Var, Not, And, Or, Xor } kind = Const;
    bool value = false;      // Const
    std::string var;         // Var
    std::vector<Expr> kids;  // Not: 1, the others: 2

    bool eval(const std::function<bool(const std::string&)>& get) const {
        switch (kind) {
            case Const: return value;
            case Var: return get(var);
            case Not: return !kids[0].eval(get);
            case And: return kids[0].eval(get) && kids[1].eval(get);
            case Or: return kids[0].eval(get) || kids[1].eval(get);
            case Xor: return kids[0].eval(get) != kids[1].eval(get);
        }
        return false;
    }
    void vars(std::vector<std::string>& out) const {
        if (kind == Var) {
            if (std::find(out.begin(), out.end(), var) == out.end()) out.push_back(var);
        }
        for (auto& k : kids) k.vars(out);
    }
    std::vector<std::string> vars() const {
        std::vector<std::string> r;
        vars(r);
        return r;
    }
    // Bit i of the result is the value when input k has bit k of i (up to 6 inputs).
    uint64_t truthTable(const std::vector<std::string>& inputs) const {
        uint64_t t = 0;
        size_t n = inputs.size();
        if (n > 6) throw Error("truthTable: more than 6 inputs");
        for (uint64_t i = 0; i < (1ull << n); i++) {
            bool v = eval([&](const std::string& name) {
                for (size_t k = 0; k < n; k++)
                    if (inputs[k] == name) return ((i >> k) & 1) != 0;
                return false;
            });
            if (v) t |= 1ull << i;
        }
        return t;
    }
    std::string str() const {
        switch (kind) {
            case Const: return value ? "1" : "0";
            case Var: return var;
            case Not: return "!" + kids[0].str();
            case And: return "(" + kids[0].str() + " & " + kids[1].str() + ")";
            case Or: return "(" + kids[0].str() + " | " + kids[1].str() + ")";
            case Xor: return "(" + kids[0].str() + " ^ " + kids[1].str() + ")";
        }
        return "";
    }
};

namespace detail {
struct FnParser {
    const std::string& s;
    size_t p = 0;
    void ws() { while (p < s.size() && isspace((unsigned char)s[p])) p++; }
    static Expr un(Expr a) {
        Expr e;
        e.kind = Expr::Not;
        e.kids.push_back(std::move(a));
        return e;
    }
    static Expr bin(Expr::Kind k, Expr a, Expr b) {
        Expr e;
        e.kind = k;
        e.kids.push_back(std::move(a));
        e.kids.push_back(std::move(b));
        return e;
    }
    Expr postfix(Expr e) {
        for (;;) {
            ws();
            if (p < s.size() && s[p] == '\'') { p++; e = un(std::move(e)); }
            else return e;
        }
    }
    Expr primary() {
        ws();
        if (p >= s.size()) throw Error("function '" + s + "': unexpected end");
        char c = s[p];
        if (c == '!') { p++; return un(primary()); }
        if (c == '(') {
            p++;
            Expr e = orExpr();
            ws();
            if (p >= s.size() || s[p] != ')') throw Error("function '" + s + "': missing ')'");
            p++;
            return postfix(std::move(e));
        }
        Expr e;
        std::string name;
        if (c == '\\' && p + 1 < s.size() && s[p + 1] == '"') {  // \"name\"
            p += 2;
            while (p < s.size() && !(s[p] == '\\' && p + 1 < s.size() && s[p + 1] == '"')) name += s[p++];
            p += 2;
        } else {
            while (p < s.size() && (isalnum((unsigned char)s[p]) || s[p] == '_' || s[p] == '[' || s[p] == ']' ||
                                    s[p] == '.' || s[p] == '$'))
                name += s[p++];
        }
        if (name.empty()) throw Error("function '" + s + "': unexpected '" + std::string(1, c) + "'");
        if (name == "0" || name == "1") {
            e.kind = Expr::Const;
            e.value = name == "1";
        } else {
            e.kind = Expr::Var;
            e.var = name;
        }
        return postfix(std::move(e));
    }
    Expr xorExpr() {
        Expr e = primary();
        for (;;) {
            ws();
            if (p < s.size() && s[p] == '^') { p++; e = bin(Expr::Xor, std::move(e), primary()); }
            else return e;
        }
    }
    Expr andExpr() {
        Expr e = xorExpr();
        for (;;) {
            ws();
            if (p >= s.size()) return e;
            char c = s[p];
            if (c == '&' || c == '*') { p++; e = bin(Expr::And, std::move(e), xorExpr()); }
            else if (c == '!' || c == '(' || c == '\\' || isalnum((unsigned char)c) || c == '_')
                e = bin(Expr::And, std::move(e), xorExpr());  // juxtaposition
            else return e;
        }
    }
    Expr orExpr() {
        Expr e = andExpr();
        for (;;) {
            ws();
            if (p < s.size() && (s[p] == '|' || s[p] == '+')) { p++; e = bin(Expr::Or, std::move(e), andExpr()); }
            else return e;
        }
    }
};
}  // namespace detail

inline Expr parseFunction(const std::string& text) {
    detail::FnParser fp{text};
    Expr e = fp.orExpr();
    fp.ws();
    if (fp.p != text.size()) throw Error("function '" + text + "': unexpected '" + text.substr(fp.p) + "'");
    return e;
}

enum class Sense { Positive, Negative, Non };

enum class ArcType {
    Comb,          // combinational, combinational_rise/fall, three_state_*, preset, clear
    RisingEdge,    // clock -> output
    FallingEdge,
    SetupRising, SetupFalling, HoldRising, HoldFalling,
    RecoveryRising, RecoveryFalling, RemovalRising, RemovalFalling,
    Other          // min_pulse_width, minimum_period, skew, nochange ...: kept, not timed
};

struct Arc {
    std::string related;   // one related pin (a timing group with several is split)
    std::string to;        // the pin the timing group is in
    Sense sense = Sense::Non;
    ArcType type = ArcType::Comb;
    std::string typeName;  // as written
    bool riseOnly = false, fallOnly = false;  // combinational_rise/_fall, preset/clear
    std::string when;
    Table cellRise, cellFall, riseTrans, fallTrans, riseCons, fallCons;

    bool isCheck() const {
        return type >= ArcType::SetupRising && type <= ArcType::RemovalFalling;
    }
    bool isSetup() const {
        return type == ArcType::SetupRising || type == ArcType::SetupFalling ||
               type == ArcType::RecoveryRising || type == ArcType::RecoveryFalling;
    }
    bool isEdge() const { return type == ArcType::RisingEdge || type == ArcType::FallingEdge; }
    // The clock edge a check or an edge arc uses: true for rising.
    bool rising() const {
        return type == ArcType::RisingEdge || type == ArcType::SetupRising || type == ArcType::HoldRising ||
               type == ArcType::RecoveryRising || type == ArcType::RemovalRising;
    }
};

enum class Dir { In, Out, InOut, Internal };

// One internal_power group of a pin: energy per transition, in the
// library's own power units (not scaled); indexes are in ns and pF.
struct PowerArc {
    std::string related, when;
    Table risePower, fallPower;
};

struct Pin {
    std::string name;
    Dir dir = Dir::In;
    double cap = 0, riseCap = -1, fallCap = -1;
    std::string function, threeState;
    bool clock = false;
    double maxTransition = -1, maxCapacitance = -1;
    std::string bus;  // the bus it is a bit of, or ""
    int busBit = -1;
    std::string signalType;  // test_cell pins: test_scan_in, test_scan_enable ...
    std::vector<PowerArc> power;  // internal_power groups

    bool hasFunction() const { return !function.empty(); }
    Expr functionExpr() const { return parseFunction(function); }
};

struct Leakage {
    std::string when;  // "" for the unconditional value
    double value = 0;
};

struct WireLoad {
    std::string name;
    double resistance = 0, capacitance = 0, area = 0, slope = 0;  // per unit length, library units
    std::vector<std::pair<int, double>> fanoutLength;
    // The estimated wire length for a net with this many sinks.
    double length(int fanout) const {
        if (fanoutLength.empty()) return 0;
        for (auto& [f, l] : fanoutLength)
            if (f == fanout) return l;
        auto& last = fanoutLength.back();
        if (fanout > last.first) return last.second + slope * (fanout - last.first);
        for (size_t i = 0; i + 1 < fanoutLength.size(); i++)
            if (fanout > fanoutLength[i].first && fanout < fanoutLength[i + 1].first) {
                double t = double(fanout - fanoutLength[i].first) / (fanoutLength[i + 1].first - fanoutLength[i].first);
                return fanoutLength[i].second + t * (fanoutLength[i + 1].second - fanoutLength[i].second);
            }
        return fanoutLength.front().second;
    }
};

struct FF {
    std::string iq, iqn, clockedOn, nextState, clear, preset, cpv1, cpv2;
    std::string clockedOnAlso;  // master-slave
};

struct Latch {
    std::string iq, iqn, enable, dataIn, clear, preset, cpv1, cpv2;
};

struct Cell {
    std::string name;
    double area = 0;
    bool dontUse = false, dontTouch = false;
    std::vector<Pin> pins;
    std::vector<Arc> arcs;
    std::vector<FF> ffs;
    std::vector<Latch> latches;
    double leakage = 0;              // cell_leakage_power
    std::vector<Leakage> leakages;   // leakage_power groups
    std::vector<Pin> testPins;       // the pins of a test_cell group (signal_type)
    const Group* group = nullptr;  // the parsed group, for anything not modeled

    const Pin* pin(const std::string& n) const {
        for (auto& p : pins) if (p.name == n) return &p;
        return nullptr;
    }
    bool sequential() const { return !ffs.empty() || !latches.empty(); }
    std::vector<const Arc*> arcsTo(const std::string& to) const {
        std::vector<const Arc*> r;
        for (auto& a : arcs) if (a.to == to) r.push_back(&a);
        return r;
    }
};

struct Library {
    std::string name, file;
    double timeScale = 1;  // multiply a library time by this to get ns
    double capScale = 1;   // multiply a library cap by this to get pF
    double defaultMaxTransition = -1;
    double nomVoltage = 0, nomTemperature = 0;
    std::string leakagePowerUnit, defaultWireLoad;
    std::vector<WireLoad> wireLoads;
    std::vector<Cell> cells;
    std::map<std::string, size_t> byName;
    std::shared_ptr<Group> root;

    const Cell* cell(const std::string& n) const {
        auto it = byName.find(n);
        return it == byName.end() ? nullptr : &cells[it->second];
    }
    const WireLoad* wireLoad(const std::string& n) const {
        for (auto& w : wireLoads) if (w.name == n) return &w;
        return nullptr;
    }

    // "1ns", "10ps" -> ns
    static double timeUnit(const std::string& s) {
        double v = std::atof(s.c_str());
        if (v == 0) v = 1;
        std::string u;
        for (char c : s) if (isalpha((unsigned char)c)) u += (char)tolower((unsigned char)c);
        if (u == "ps") return v * 1e-3;
        if (u == "us") return v * 1e3;
        if (u == "fs") return v * 1e-6;
        return v;  // ns
    }

    static Library load(const Group& g, const std::string& file = "") {
        Library L;
        L.file = file;
        L.name = g.name();
        L.root = std::make_shared<Group>(g);
        const Group& r = *L.root;
        if (auto* a = r.attr("time_unit")) L.timeScale = timeUnit(a->vals[0]);
        if (auto* a = r.attr("capacitive_load_unit"); a && a->vals.size() >= 2) {
            double v = std::atof(a->vals[0].c_str());
            std::string u = a->vals[1];
            for (auto& c : u) c = (char)tolower((unsigned char)c);
            L.capScale = v * (u == "ff" ? 1e-3 : u == "nf" ? 1e3 : 1.0);
        }
        L.nomVoltage = r.num("nom_voltage", 0);
        L.nomTemperature = r.num("nom_temperature", 0);
        if (r.has("default_max_transition"))
            L.defaultMaxTransition = r.num("default_max_transition", -1) * L.timeScale;

        L.leakagePowerUnit = r.str("leakage_power_unit");
        L.defaultWireLoad = r.str("default_wire_load");
        for (auto* wg : r.all("wire_load")) {
            WireLoad w;
            w.name = wg->name();
            w.resistance = wg->num("resistance", 0);
            w.capacitance = wg->num("capacitance", 0);
            w.area = wg->num("area", 0);
            w.slope = wg->num("slope", 0);
            for (auto& a : wg->attrs)
                if (a.name == "fanout_length" && a.vals.size() >= 2)
                    w.fanoutLength.push_back({std::atoi(a.vals[0].c_str()), std::atof(a.vals[1].c_str())});
            std::sort(w.fanoutLength.begin(), w.fanoutLength.end());
            L.wireLoads.push_back(std::move(w));
        }
        // Templates: name -> (var1, var2, idx1, idx2).
        std::map<std::string, Table> tmpl;
        for (auto& t : r.groups) {
            if (t.type != "lu_table_template" && t.type != "power_lut_template") continue;
            Table x;
            x.var1 = varOf(t.str("variable_1"));
            x.var2 = varOf(t.str("variable_2"));
            if (auto* a = t.attr("index_1")) x.idx1 = numbers(a->vals.empty() ? "" : a->vals[0]);
            if (auto* a = t.attr("index_2")) x.idx2 = numbers(a->vals.empty() ? "" : a->vals[0]);
            tmpl[t.name()] = x;
        }
        // Bus types: name -> (from, to).
        std::map<std::string, std::pair<int, int>> busTypes;
        auto collectTypes = [&](const Group& scope) {
            for (auto& t : scope.groups)
                if (t.type == "type")
                    busTypes[t.name()] = {(int)t.num("bit_from", 0), (int)t.num("bit_to", 0)};
        };
        collectTypes(r);

        auto readTable = [&](const Group& tg, bool isTime) {
            Table x;
            auto it = tmpl.find(tg.name());
            if (it != tmpl.end()) x = it->second;
            if (auto* a = tg.attr("index_1")) x.idx1 = numbers(a->vals.empty() ? "" : a->vals[0]);
            if (auto* a = tg.attr("index_2")) x.idx2 = numbers(a->vals.empty() ? "" : a->vals[0]);
            if (auto* a = tg.attr("values"))
                for (auto& row : a->vals)
                    for (double d : numbers(row)) x.v.push_back(d);
            if (x.var1 == Var::None && x.var2 == Var::None) {
                // No template (e.g. "scalar"): assume the usual order.
                x.var1 = Var::InSlew;
                x.var2 = Var::Load;
            }
            // Normalize units: slews are times, loads are caps.
            auto scaleIdx = [&](std::vector<double>& idx, Var var) {
                double f = var == Var::Load ? L.capScale : L.timeScale;
                for (auto& d : idx) d *= f;
            };
            scaleIdx(x.idx1, x.var1);
            scaleIdx(x.idx2, x.var2);
            if (isTime)
                for (auto& d : x.v) d *= L.timeScale;
            size_t want = std::max<size_t>(1, x.idx1.size()) * std::max<size_t>(1, x.idx2.size());
            if (x.v.size() != 1 && x.v.size() != want)
                throw Error(file + ":" + std::to_string(tg.line) + ": table has " + std::to_string(x.v.size()) +
                            " values, its indexes need " + std::to_string(want));
            return x;
        };

        auto readArcs = [&](Cell& c, const Group& pg, const std::string& pinName) {
            for (auto* tg : pg.all("timing")) {
                std::string rel = tg->str("related_pin");
                std::string tt = tg->str("timing_type", "combinational");
                std::string ts = tg->str("timing_sense", "");
                Arc proto;
                proto.to = pinName;
                proto.typeName = tt;
                proto.when = tg->str("when");
                if (ts == "positive_unate") proto.sense = Sense::Positive;
                else if (ts == "negative_unate") proto.sense = Sense::Negative;
                else proto.sense = Sense::Non;
                if (tt == "combinational" || tt == "three_state_enable" || tt == "three_state_disable" ||
                    tt == "three_state_enable_rise" || tt == "three_state_enable_fall" ||
                    tt == "three_state_disable_rise" || tt == "three_state_disable_fall")
                    proto.type = ArcType::Comb;
                else if (tt == "combinational_rise") { proto.type = ArcType::Comb; proto.riseOnly = true; }
                else if (tt == "combinational_fall") { proto.type = ArcType::Comb; proto.fallOnly = true; }
                else if (tt == "preset") { proto.type = ArcType::Comb; proto.riseOnly = true; }
                else if (tt == "clear") { proto.type = ArcType::Comb; proto.fallOnly = true; }
                else if (tt == "rising_edge") proto.type = ArcType::RisingEdge;
                else if (tt == "falling_edge") proto.type = ArcType::FallingEdge;
                else if (tt == "setup_rising") proto.type = ArcType::SetupRising;
                else if (tt == "setup_falling") proto.type = ArcType::SetupFalling;
                else if (tt == "hold_rising") proto.type = ArcType::HoldRising;
                else if (tt == "hold_falling") proto.type = ArcType::HoldFalling;
                else if (tt == "recovery_rising") proto.type = ArcType::RecoveryRising;
                else if (tt == "recovery_falling") proto.type = ArcType::RecoveryFalling;
                else if (tt == "removal_rising") proto.type = ArcType::RemovalRising;
                else if (tt == "removal_falling") proto.type = ArcType::RemovalFalling;
                else proto.type = ArcType::Other;
                for (auto& sub : tg->groups) {
                    if (sub.type == "cell_rise") proto.cellRise = readTable(sub, true);
                    else if (sub.type == "cell_fall") proto.cellFall = readTable(sub, true);
                    else if (sub.type == "rise_transition") proto.riseTrans = readTable(sub, true);
                    else if (sub.type == "fall_transition") proto.fallTrans = readTable(sub, true);
                    else if (sub.type == "rise_constraint") proto.riseCons = readTable(sub, true);
                    else if (sub.type == "fall_constraint") proto.fallCons = readTable(sub, true);
                }
                std::istringstream is(rel);
                std::string one;
                bool any = false;
                while (is >> one) {
                    Arc a = proto;
                    a.related = one;
                    c.arcs.push_back(std::move(a));
                    any = true;
                }
                if (!any && proto.type != ArcType::Other)
                    throw Error(file + ":" + std::to_string(tg->line) + ": timing group without related_pin");
            }
        };

        auto readPin = [&](Cell& c, const Group& pg, const std::string& name, const std::string& bus, int bit,
                           const Group* busGroup) {
            Pin p;
            p.name = name;
            p.bus = bus;
            p.busBit = bit;
            auto get = [&](const std::string& a) -> const Attr* {
                if (auto* x = pg.attr(a)) return x;
                return busGroup ? busGroup->attr(a) : nullptr;
            };
            auto gs = [&](const std::string& a, const std::string& d = "") {
                auto* x = get(a);
                return x && !x->vals.empty() ? x->vals[0] : d;
            };
            std::string d = gs("direction", "input");
            p.dir = d == "output" ? Dir::Out : d == "inout" ? Dir::InOut : d == "internal" ? Dir::Internal : Dir::In;
            p.cap = std::atof(gs("capacitance", "0").c_str()) * L.capScale;
            if (get("rise_capacitance")) p.riseCap = std::atof(gs("rise_capacitance").c_str()) * L.capScale;
            if (get("fall_capacitance")) p.fallCap = std::atof(gs("fall_capacitance").c_str()) * L.capScale;
            if (!get("capacitance") && p.riseCap >= 0) p.cap = std::max(p.riseCap, std::max(p.fallCap, 0.0));
            p.function = gs("function");
            p.threeState = gs("three_state");
            p.clock = gs("clock") == "true";
            if (get("max_transition")) p.maxTransition = std::atof(gs("max_transition").c_str()) * L.timeScale;
            if (get("max_capacitance")) p.maxCapacitance = std::atof(gs("max_capacitance").c_str()) * L.capScale;
            p.signalType = gs("signal_type");
            for (auto* ig : pg.all("internal_power")) {
                PowerArc pa;
                pa.related = ig->str("related_pin");
                pa.when = ig->str("when");
                for (auto& sub : ig->groups) {
                    if (sub.type == "rise_power") pa.risePower = readTable(sub, false);
                    else if (sub.type == "fall_power") pa.fallPower = readTable(sub, false);
                    else if (sub.type == "power") pa.risePower = pa.fallPower = readTable(sub, false);
                }
                p.power.push_back(std::move(pa));
            }
            c.pins.push_back(p);
            readArcs(c, pg, name);
            if (busGroup && &pg != busGroup) readArcs(c, *busGroup, name);  // timing written on the bus
        };

        for (auto* cg : r.all("cell")) {
            Cell c;
            c.name = cg->name();
            c.area = cg->num("area", 0);
            c.dontUse = cg->str("dont_use") == "true";
            c.dontTouch = cg->str("dont_touch") == "true";
            c.leakage = cg->num("cell_leakage_power", 0);
            for (auto* lg : cg->all("leakage_power")) c.leakages.push_back({lg->str("when"), lg->num("value", 0)});
            if (auto* tc = cg->first("test_cell"))
                for (auto* pg : tc->all("pin"))
                    for (auto& n : pg->names) {
                        Pin tp;
                        tp.name = n;
                        std::string d = pg->str("direction", "input");
                        tp.dir = d == "output" ? Dir::Out : d == "inout" ? Dir::InOut : Dir::In;
                        tp.signalType = pg->str("signal_type");
                        tp.function = pg->str("function");
                        c.testPins.push_back(tp);
                    }
            collectTypes(*cg);
            for (auto& sub : cg->groups) {
                if (sub.type == "pin") {
                    for (auto& n : sub.names) readPin(c, sub, n, "", -1, nullptr);
                } else if (sub.type == "bus" || sub.type == "bundle") {
                    std::string bname = sub.name();
                    auto bt = busTypes.find(sub.str("bus_type"));
                    std::vector<std::string> bitNames;
                    std::vector<int> bits;
                    if (bt != busTypes.end()) {
                        int f = bt->second.first, t = bt->second.second, s = f <= t ? 1 : -1;
                        for (int i = f;; i += s) {
                            bitNames.push_back(bname + "[" + std::to_string(i) + "]");
                            bits.push_back(i);
                            if (i == t) break;
                        }
                    }
                    // Bits with their own pin group, otherwise the bus attributes.
                    std::map<std::string, const Group*> own;
                    for (auto* pg : sub.all("pin"))
                        for (auto& n : pg->names) own[n] = pg;
                    for (size_t i = 0; i < bitNames.size(); i++) {
                        auto it = own.find(bitNames[i]);
                        readPin(c, it != own.end() ? *it->second : sub, bitNames[i], bname, bits[i], &sub);
                    }
                    if (bitNames.empty())
                        for (auto& [n, pg] : own) readPin(c, *pg, n, bname, -1, &sub);
                } else if (sub.type == "ff" || sub.type == "ff_bank") {
                    FF f;
                    f.iq = sub.names.size() > 0 ? sub.names[0] : "IQ";
                    f.iqn = sub.names.size() > 1 ? sub.names[1] : "IQN";
                    f.clockedOn = sub.str("clocked_on");
                    f.clockedOnAlso = sub.str("clocked_on_also");
                    f.nextState = sub.str("next_state");
                    f.clear = sub.str("clear");
                    f.preset = sub.str("preset");
                    f.cpv1 = sub.str("clear_preset_var1");
                    f.cpv2 = sub.str("clear_preset_var2");
                    c.ffs.push_back(f);
                } else if (sub.type == "latch" || sub.type == "latch_bank") {
                    Latch l;
                    l.iq = sub.names.size() > 0 ? sub.names[0] : "IQ";
                    l.iqn = sub.names.size() > 1 ? sub.names[1] : "IQN";
                    l.enable = sub.str("enable");
                    l.dataIn = sub.str("data_in");
                    l.clear = sub.str("clear");
                    l.preset = sub.str("preset");
                    l.cpv1 = sub.str("clear_preset_var1");
                    l.cpv2 = sub.str("clear_preset_var2");
                    c.latches.push_back(l);
                }
            }
            L.byName[c.name] = L.cells.size();
            L.cells.push_back(std::move(c));
        }
        // Group pointers into the shared root (stable now).
        size_t k = 0;
        for (auto& sub : L.root->groups)
            if (sub.type == "cell" && k < L.cells.size()) L.cells[k++].group = &sub;
        return L;
    }

    // Reads a .lib file; throws liberty::Error with file:line.
    static Library read(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw Error("cannot open " + path);
        std::stringstream ss;
        ss << in.rdbuf();
        return load(parse(ss.str(), path), path);
    }
};

}  // namespace liberty

namespace liberty {
inline Library load(const std::string& path) { return Library::read(path); }
}  // namespace liberty
