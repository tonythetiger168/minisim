// SDF back-annotation (IEEE 1800-2023 clause 32; the file format is IEEE
// 1497 / OVI SDF 3.0). sdf::read() turns an SDF file into an ordered list
// of annotations (Op); sdf::annotate() (sdf_annotate.inc) applies them to
// the lowered design: specify path delays, interconnect delays, timing
// check limits and specparams.
#pragma once
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace sdf {

// One s-expression: an atom or a parenthesized list.
struct Node {
    bool list = false;
    bool quoted = false;   // a "string" atom
    std::string atom;
    std::vector<Node> kids;
    int line = 0;
    // The keyword of a list: its first atom, upper case ("IOPATH").
    std::string head() const {
        if (!list || kids.empty() || kids[0].list) return "";
        std::string h = kids[0].atom;
        for (auto& c : h) c = (char)toupper((unsigned char)c);
        return h;
    }
};

struct Error {
    std::string msg;
    int line = 0;
};

// Tokenizes and parses the whole file. Comments are // and /* */. A
// backslash escapes the next character of an identifier (\[ \/ \.).
// The escape is kept in the atom so that an escaped divider is not split
// as a hierarchy; names are unescaped when they are split (splitPath).
inline bool parse(const std::string& s, Node& root, Error& err) {
    size_t p = 0;
    int line = 1;
    std::vector<Node*> stack{&root};
    root.list = true;
    auto skip = [&] {
        for (;;) {
            while (p < s.size() && isspace((unsigned char)s[p])) { if (s[p] == '\n') line++; p++; }
            if (p + 1 < s.size() && s[p] == '/' && s[p + 1] == '/') {
                while (p < s.size() && s[p] != '\n') p++;
                continue;
            }
            if (p + 1 < s.size() && s[p] == '/' && s[p + 1] == '*') {
                p += 2;
                while (p + 1 < s.size() && !(s[p] == '*' && s[p + 1] == '/')) { if (s[p] == '\n') line++; p++; }
                p += 2;
                continue;
            }
            break;
        }
    };
    for (;;) {
        skip();
        if (p >= s.size()) break;
        char c = s[p];
        if (c == '(') {
            Node n; n.list = true; n.line = line;
            stack.back()->kids.push_back(std::move(n));
            stack.push_back(&stack.back()->kids.back());
            p++;
            continue;
        }
        if (c == ')') {
            if (stack.size() == 1) { err = {"unbalanced ')'", line}; return false; }
            stack.pop_back();
            p++;
            continue;
        }
        Node a; a.line = line;
        if (c == '"') {
            a.quoted = true;
            p++;
            while (p < s.size() && s[p] != '"') {
                if (s[p] == '\\' && p + 1 < s.size()) p++;
                if (s[p] == '\n') line++;
                a.atom += s[p++];
            }
            if (p >= s.size()) { err = {"unterminated string", a.line}; return false; }
            p++;
        } else {
            while (p < s.size() && !isspace((unsigned char)s[p]) && s[p] != '(' && s[p] != ')' && s[p] != '"') {
                if (s[p] == '\\' && p + 1 < s.size()) { a.atom += s[p++]; a.atom += s[p++]; continue; }
                a.atom += s[p++];
            }
        }
        stack.back()->kids.push_back(std::move(a));
    }
    if (stack.size() != 1) { err = {"missing ')'", stack.back()->line}; return false; }
    return true;
}

// A min:typ:max value; each part may be missing ("(::3)", "()").
struct Triple {
    bool has[3] = {false, false, false};
    double v[3] = {0, 0, 0};
    bool empty() const { return !has[0] && !has[1] && !has[2]; }
};
// One delay value of an IOPATH / INTERCONNECT ...: the delay, and the
// reject and error pulse limits of the "((d) (r) (e))" form.
struct DelVal {
    Triple d, r, e;
    bool pulse = false;   // written as ((d) (r) (e)) or ((d) (r))
    bool hasE = false;
};

// A port: a name (maybe hierarchical, maybe a bit or part select), with an
// edge and a COND for timing checks.
struct Port {
    std::string name;       // as written, dividers turned into '.'
    std::string edge;       // "", posedge, negedge, 01, 10, 0z, z1, 1z, z0
    std::string cond;       // the condition text, spaces removed
    bool hasCond = false;
};

enum Kind {
    IOPATH, PORT, INTERCONNECT, NETDELAY, DEVICE, PATHPULSE, PATHPULSEPERCENT,
    TCHK, LABEL
};

struct Op {
    Kind kind = IOPATH;
    bool incr = false;        // INCREMENT (else ABSOLUTE)
    std::string cellType;     // CELLTYPE
    std::string inst;         // INSTANCE, relative to the annotation scope; "*" = every instance of the cell type
    bool wild = false;
    Port a, b;                // IOPATH in / out, INTERCONNECT source / load, PORT / NETDELAY / DEVICE port, timing check first / second
    std::string cond;         // COND of an IOPATH, spaces removed
    bool hasCond = false, condElse = false;
    std::vector<DelVal> vals; // delays, or the timing check's values
    std::string tchk;         // SETUP, HOLD, ...
    std::string scond, ccond; // SETUPHOLD / RECREM SCOND / CCOND
    std::string label;        // LABEL: the specparam
    int line = 0;
};

struct File {
    std::string version, design, divider = ".";
    double timescale = 1e-9;   // seconds per unit
    std::vector<Op> ops;
    std::vector<std::string> warnings;
};

inline std::string upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}
// The text of a node as written, for COND expressions: atoms joined, lists
// in parentheses, all white space dropped so that "a == 1" matches "a==1".
inline std::string text(const Node& n) {
    if (!n.list) return n.quoted ? "\"" + n.atom + "\"" : n.atom;
    std::string r = "(";
    for (auto& k : n.kids) r += text(k);
    return r + ")";
}
inline std::string squeeze(const std::string& s) {
    std::string r;
    for (char c : s) if (!isspace((unsigned char)c)) r += c;
    // Outer parentheses that wrap the whole text change nothing.
    while (r.size() >= 2 && r.front() == '(' && r.back() == ')') {
        int d = 0;
        bool whole = true;
        for (size_t i = 0; i < r.size(); i++) {
            if (r[i] == '(') d++;
            else if (r[i] == ')') d--;
            if (d == 0 && i + 1 < r.size()) { whole = false; break; }
        }
        if (!whole) break;
        r = r.substr(1, r.size() - 2);
    }
    return r;
}

// A value list "( 1.2:1.3:1.4 )", "(1)", "()", "(::2)". Atoms are joined,
// so "( 1 : 2 : 3 )" reads the same.
inline bool triple(const Node& n, Triple& t) {
    if (!n.list) return false;
    std::string s;
    for (auto& k : n.kids) {
        if (k.list) return false;
        s += k.atom;
    }
    if (s.empty()) return true;
    std::vector<std::string> parts;
    size_t a = 0;
    for (;;) {
        size_t c = s.find(':', a);
        parts.push_back(s.substr(a, c == std::string::npos ? std::string::npos : c - a));
        if (c == std::string::npos) break;
        a = c + 1;
    }
    if (parts.size() != 1 && parts.size() != 3) return false;
    for (size_t i = 0; i < parts.size(); i++) {
        if (parts[i].empty()) continue;
        char* end = nullptr;
        double v = strtod(parts[i].c_str(), &end);
        if (!end || *end) return false;
        if (parts.size() == 1) {
            for (int k = 0; k < 3; k++) { t.has[k] = true; t.v[k] = v; }
        } else {
            t.has[i] = true;
            t.v[i] = v;
        }
    }
    return true;
}
// One delay value: "(v)" or "((d) (r) (e))".
inline bool delval(const Node& n, DelVal& dv) {
    if (!n.list) return false;
    bool anyList = false;
    for (auto& k : n.kids) anyList |= k.list;
    if (!anyList) return triple(n, dv.d);
    if (n.kids.size() < 1 || n.kids.size() > 3) return false;
    dv.pulse = true;
    if (!triple(n.kids[0], dv.d)) return false;
    if (n.kids.size() >= 2 && !triple(n.kids[1], dv.r)) return false;
    if (n.kids.size() == 3) {
        if (!triple(n.kids[2], dv.e)) return false;
        dv.hasE = true;
    }
    return true;
}

// TIMESCALE: "1ns", "100 ps", "1.0 us".
inline bool timescale(const Node& n, double& sec) {
    std::string s;
    for (size_t i = 1; i < n.kids.size(); i++) s += n.kids[i].atom;
    size_t u = 0;
    while (u < s.size() && (isdigit((unsigned char)s[u]) || s[u] == '.')) u++;
    double m = u ? atof(s.substr(0, u).c_str()) : 1.0;
    std::string unit = s.substr(u);
    for (auto& c : unit) c = (char)tolower((unsigned char)c);
    static const std::pair<const char*, double> units[] = {
        {"s", 1}, {"ms", 1e-3}, {"us", 1e-6}, {"ns", 1e-9}, {"ps", 1e-12}, {"fs", 1e-15}};
    for (auto [k, v] : units)
        if (unit == k) { sec = m * v; return true; }
    return false;
}

struct Reader {
    File& f;
    Error& err;
    bool fail(const Node& n, const std::string& m) {
        err = {m, n.line};
        return false;
    }
    void warn(const Node& n, const std::string& m) {
        f.warnings.push_back("line " + std::to_string(n.line) + ": " + m);
    }
    // A hierarchical name with the file's divider turned into '.'.
    std::string path(const std::string& s) {
        if (f.divider == ".") return s;
        std::string r;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '\\' && i + 1 < s.size()) { r += s[i]; r += s[++i]; continue; }
            r += s[i] == f.divider[0] ? '.' : s[i];
        }
        return r;
    }
    // port_spec: a name, or (edge name). A name may be followed by a
    // separate [n] atom in some writers ("a [3]"): joined here.
    bool port(const Node& n, Port& p) {
        if (!n.list) { p.name = path(n.atom); return true; }
        std::string h = n.kids.empty() || n.kids[0].list ? "" : n.kids[0].atom;
        std::string lh = h;
        for (auto& c : lh) c = (char)tolower((unsigned char)c);
        static const char* edges[] = {"posedge", "negedge", "01", "10", "0z", "z1", "1z", "z0"};
        for (auto e : edges)
            if (lh == e) {
                if (n.kids.size() < 2 || n.kids[1].list) return fail(n, "bad edge port");
                p.edge = e;
                p.name = path(n.kids[1].atom);
                for (size_t i = 2; i < n.kids.size(); i++) p.name += n.kids[i].atom;
                return true;
            }
        return fail(n, "bad port " + text(n));
    }
    // port_tchk: port_spec, or (COND [name] cond port_spec).
    bool portTchk(const Node& n, Port& p) {
        if (n.list && n.head() == "COND") {
            size_t i = 1;
            if (i < n.kids.size() && n.kids[i].quoted) i++;
            if (n.kids.size() < i + 2) return fail(n, "bad COND");
            std::string c;
            for (size_t k = i; k + 1 < n.kids.size(); k++) c += text(n.kids[k]);
            p.cond = squeeze(c);
            p.hasCond = true;
            return port(n.kids.back(), p);
        }
        return port(n, p);
    }
    bool values(const Node& n, size_t from, std::vector<DelVal>& out) {
        for (size_t i = from; i < n.kids.size(); i++) {
            DelVal dv;
            if (!delval(n.kids[i], dv)) return fail(n.kids[i], "bad delay value " + text(n.kids[i]));
            out.push_back(dv);
        }
        return true;
    }
    // One entry of ABSOLUTE / INCREMENT.
    bool delEntry(const Node& n, Op base) {
        std::string h = n.head();
        Op op = base;
        op.line = n.line;
        if (h == "COND" || h == "CONDELSE") {
            if (h == "CONDELSE") {
                if (n.kids.size() != 2) return fail(n, "bad CONDELSE");
                op.condElse = true;
            } else {
                size_t i = 1;
                if (i < n.kids.size() && n.kids[i].quoted) i++;
                if (n.kids.size() < i + 2) return fail(n, "bad COND");
                std::string c;
                for (size_t k = i; k + 1 < n.kids.size(); k++) c += text(n.kids[k]);
                op.cond = squeeze(c);
                op.hasCond = true;
            }
            const Node& io = n.kids.back();
            if (io.head() != "IOPATH") return fail(io, "COND without IOPATH");
            return iopath(io, op);
        }
        if (h == "IOPATH") return iopath(n, op);
        if (h == "PORT" || h == "NETDELAY" || h == "DEVICE") {
            op.kind = h == "PORT" ? PORT : h == "NETDELAY" ? NETDELAY : DEVICE;
            size_t i = 1;
            if (op.kind != DEVICE || (n.kids.size() > 1 && !n.kids[1].list)) {
                if (n.kids.size() < 2) return fail(n, "bad " + h);
                if (!port(n.kids[1], op.a)) return false;
                i = 2;
            }
            if (!values(n, i, op.vals)) return false;
            f.ops.push_back(op);
            return true;
        }
        if (h == "INTERCONNECT") {
            op.kind = INTERCONNECT;
            if (n.kids.size() < 3 || !port(n.kids[1], op.a) || !port(n.kids[2], op.b)) return fail(n, "bad INTERCONNECT");
            if (!values(n, 3, op.vals)) return false;
            f.ops.push_back(op);
            return true;
        }
        if (h == "PATHPULSE" || h == "PATHPULSEPERCENT") {
            op.kind = h == "PATHPULSE" ? PATHPULSE : PATHPULSEPERCENT;
            size_t i = 1;
            // (PATHPULSE [in out] (r) [(e)])
            if (i < n.kids.size() && !n.kids[i].list) {
                if (!port(n.kids[i], op.a)) return false;
                i++;
                if (i >= n.kids.size() || n.kids[i].list) return fail(n, "bad " + h);
                if (!port(n.kids[i], op.b)) return false;
                i++;
            }
            if (!values(n, i, op.vals)) return false;
            f.ops.push_back(op);
            return true;
        }
        warn(n, "ignoring " + h);
        return true;
    }
    bool iopath(const Node& n, Op& op) {
        op.kind = IOPATH;
        if (n.kids.size() < 3) return fail(n, "bad IOPATH");
        if (!port(n.kids[1], op.a) || !port(n.kids[2], op.b)) return false;
        size_t i = 3;
        if (i < n.kids.size() && n.kids[i].head() == "RETAIN") i++;   // RETAIN may be ignored (Table 32-1)
        if (!values(n, i, op.vals)) return false;
        f.ops.push_back(op);
        return true;
    }
    bool tchk(const Node& n, Op base) {
        Op op = base;
        op.kind = TCHK;
        op.line = n.line;
        op.tchk = n.head();
        static const char* known[] = {"SETUP", "HOLD", "SETUPHOLD", "RECOVERY", "REMOVAL", "RECREM",
                                      "SKEW", "BIDIRECTSKEW", "WIDTH", "PERIOD", "NOCHANGE"};
        bool ok = false;
        for (auto k : known) ok |= op.tchk == k;
        if (!ok) { warn(n, "ignoring timing check " + op.tchk); return true; }
        bool one = op.tchk == "WIDTH" || op.tchk == "PERIOD";
        size_t i = 1;
        if (n.kids.size() < (one ? 3u : 4u)) return fail(n, "bad " + op.tchk);
        if (!portTchk(n.kids[i++], op.a)) return false;
        if (!one && !portTchk(n.kids[i++], op.b)) return false;
        for (; i < n.kids.size(); i++) {
            const Node& k = n.kids[i];
            std::string h = k.head();
            if (h == "SCOND" || h == "CCOND") {
                std::string c;
                size_t j = 1;
                if (j < k.kids.size() && k.kids[j].quoted) j++;
                for (; j < k.kids.size(); j++) c += text(k.kids[j]);
                (h == "SCOND" ? op.scond : op.ccond) = squeeze(c);
                continue;
            }
            DelVal dv;
            if (!triple(k, dv.d)) return fail(k, "bad timing check value " + text(k));
            op.vals.push_back(dv);
        }
        f.ops.push_back(op);
        return true;
    }
    bool cell(const Node& n) {
        Op base;
        for (size_t i = 1; i < n.kids.size(); i++) {
            const Node& k = n.kids[i];
            std::string h = k.head();
            if (h == "CELLTYPE") {
                if (k.kids.size() >= 2) base.cellType = k.kids[1].atom;
            } else if (h == "INSTANCE") {
                std::string s;
                for (size_t j = 1; j < k.kids.size(); j++) s += k.kids[j].atom;
                if (s == "*") { base.wild = true; s.clear(); }
                base.inst = path(s);
            } else if (h == "DELAY") {
                for (size_t j = 1; j < k.kids.size(); j++) {
                    const Node& d = k.kids[j];
                    std::string dh = d.head();
                    if (dh == "ABSOLUTE" || dh == "INCREMENT") {
                        Op b = base;
                        b.incr = dh == "INCREMENT";
                        for (size_t m = 1; m < d.kids.size(); m++)
                            if (!delEntry(d.kids[m], b)) return false;
                    } else if (dh == "PATHPULSE" || dh == "PATHPULSEPERCENT") {
                        if (!delEntry(d, base)) return false;
                    } else warn(d, "ignoring DELAY " + dh);
                }
            } else if (h == "TIMINGCHECK") {
                for (size_t j = 1; j < k.kids.size(); j++) {
                    const Node& t = k.kids[j];
                    if (t.head() == "COND") {   // (COND [name] cond timing_check)
                        warn(t, "ignoring a COND around a whole timing check");
                        continue;
                    }
                    if (!tchk(t, base)) return false;
                }
            } else if (h == "LABEL") {
                for (size_t j = 1; j < k.kids.size(); j++) {
                    const Node& d = k.kids[j];
                    std::string dh = d.head();
                    if (dh != "ABSOLUTE" && dh != "INCREMENT") { warn(d, "ignoring LABEL " + dh); continue; }
                    for (size_t m = 1; m < d.kids.size(); m++) {
                        const Node& l = d.kids[m];
                        if (!l.list || l.kids.size() < 2 || l.kids[0].list) return fail(l, "bad LABEL entry");
                        Op op = base;
                        op.kind = LABEL;
                        op.incr = dh == "INCREMENT";
                        op.line = l.line;
                        op.label = l.kids[0].atom;
                        if (l.kids.size() >= 2 && !l.kids[1].list) {   // (dhigh 60), as 32.4.3 writes it
                            Node v;
                            v.list = true;
                            for (size_t q = 1; q < l.kids.size(); q++) v.kids.push_back(l.kids[q]);
                            DelVal dv;
                            if (!triple(v, dv.d)) return fail(l, "bad LABEL value " + text(l));
                            op.vals.push_back(dv);
                        } else if (!values(l, 1, op.vals)) return false;
                        f.ops.push_back(op);
                    }
                }
            } else if (h == "TIMINGENV") {
                // Not SystemVerilog timing: ignored without a warning (32.3).
            } else return fail(k, "unexpected " + (h.empty() ? text(k) : h) + " in CELL");
        }
        return true;
    }
    bool file(const Node& root) {
        if (root.kids.size() != 1 || root.kids[0].head() != "DELAYFILE")
            return fail(root.kids.empty() ? root : root.kids[0], "not an SDF file (no DELAYFILE)");
        const Node& d = root.kids[0];
        for (size_t i = 1; i < d.kids.size(); i++) {
            const Node& k = d.kids[i];
            std::string h = k.head();
            auto arg = [&] { return k.kids.size() >= 2 ? k.kids[1].atom : std::string(); };
            if (h == "SDFVERSION") f.version = arg();
            else if (h == "DESIGN") f.design = arg();
            else if (h == "DIVIDER") {
                f.divider = arg();
                if (f.divider != "." && f.divider != "/") return fail(k, "DIVIDER must be . or /");
            } else if (h == "TIMESCALE") {
                if (!timescale(k, f.timescale)) return fail(k, "bad TIMESCALE");
            } else if (h == "CELL") {
                if (!cell(k)) return false;
            } else if (h == "DATE" || h == "VENDOR" || h == "PROGRAM" || h == "VERSION" || h == "VOLTAGE" ||
                       h == "PROCESS" || h == "TEMPERATURE") {
            } else return fail(k, "unexpected " + (h.empty() ? text(k) : h) + " in DELAYFILE");
        }
        return true;
    }
};

inline bool readText(const std::string& text, File& f, Error& err) {
    Node root;
    if (!parse(text, root, err)) return false;
    Reader r{f, err};
    return r.file(root);
}

inline bool read(const std::string& fileName, File& f, Error& err) {
    std::ifstream in(fileName, std::ios::binary);
    if (!in) { err = {"cannot open " + fileName, 0}; return false; }
    std::stringstream ss;
    ss << in.rdbuf();
    return readText(ss.str(), f, err);
}

// Splits a.b\.c.d into its names, dropping the escapes.
inline std::vector<std::string> splitPath(const std::string& s) {
    std::vector<std::string> r(1);
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { r.back() += s[++i]; continue; }
        if (s[i] == '.') { r.emplace_back(); continue; }
        r.back() += s[i];
    }
    if (r.size() == 1 && r[0].empty()) r.clear();
    return r;
}
inline std::string unescape(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) i++;
        r += s[i];
    }
    return r;
}

// Which member of a min:typ:max triple to annotate and how to scale it
// ($sdf_annotate's mtm_spec, scale_factors and scale_type, 32.9).
struct Sel {
    int mtm = 1;                      // 0 min, 1 typ, 2 max
    double scale[3] = {1, 1, 1};
    int from = -1;                    // scale_type: -1 FROM_MTM, else the member scaled
};
inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
// TOOL_CONTROL: the simulator's choice (msim +mindelays / +maxdelays).
inline bool setMtm(Sel& c, const std::string& v, int tool) {
    std::string u = upper(trim(v));
    if (u == "MINIMUM") c.mtm = 0;
    else if (u == "TYPICAL") c.mtm = 1;
    else if (u == "MAXIMUM") c.mtm = 2;
    else if (u == "TOOL_CONTROL") c.mtm = tool;
    else return false;
    return true;
}
inline bool setScale(Sel& c, const std::string& v) {
    double f[3];
    char tail;
    if (std::sscanf(trim(v).c_str(), "%lf:%lf:%lf%c", &f[0], &f[1], &f[2], &tail) != 3) return false;
    for (int k = 0; k < 3; k++) c.scale[k] = f[k];
    return true;
}
inline bool setScaleType(Sel& c, const std::string& v) {
    std::string u = upper(trim(v));
    if (u == "FROM_MTM") c.from = -1;
    else if (u == "FROM_MINIMUM") c.from = 0;
    else if (u == "FROM_TYPICAL") c.from = 1;
    else if (u == "FROM_MAXIMUM") c.from = 2;
    else return false;
    return true;
}
// The configuration file (32.9) is tool-specific. msim reads
// "KEYWORD = value;" lines (or "KEYWORD value") for MTM_SPEC, SCALE_FACTORS
// and SCALE_TYPE; // comments are skipped. Problems go to warn.
inline bool readConfig(Sel& c, const std::string& name, int tool, std::vector<std::string>& warn) {
    std::ifstream in(name);
    if (!in) { warn.push_back("cannot open configuration file " + name); return false; }
    std::string line;
    while (std::getline(in, line)) {
        if (auto k = line.find("//"); k != std::string::npos) line.resize(k);
        for (auto& ch : line) if (ch == '=' || ch == ';') ch = ' ';
        std::istringstream ls(line);
        std::string key, val;
        if (!(ls >> key)) continue;
        ls >> val;
        key = upper(key);
        bool ok = key == "MTM_SPEC" ? setMtm(c, val, tool) : key == "SCALE_FACTORS" ? setScale(c, val)
                : key == "SCALE_TYPE" ? setScaleType(c, val) : false;
        if (!ok) warn.push_back("configuration file " + name + ": ignoring '" + trim(line) + "'");
    }
    return true;
}
// The selected, scaled member of a triple in the file's units; missing:
// nothing to annotate.
inline std::optional<double> pick(const Triple& t, const Sel& c) {
    int src = c.from < 0 ? c.mtm : c.from;
    if (!t.has[src]) return std::nullopt;
    return t.v[src] * c.scale[c.mtm];
}

}   // namespace sdf
