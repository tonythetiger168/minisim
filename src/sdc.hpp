#pragma once
// sdc.hpp - a reader for Synopsys Design Constraints (SDC 2.1) files.
//
// One reader for every tool that takes constraints: msdc (constraint checks),
// static timing and synthesis. It has no front-end dependency: the design is
// seen through sdc::Design, which a tool implements for its own netlist (RTL
// or gates). With no design at all the reader still parses, and keeps each
// object query as an unresolved pattern.
//
//   sdc::Reader r;
//   r.design = &myDesign;            // optional
//   r.load("top.sdc");
//   for (auto& c : r.sdc.clocks) ...
//   for (auto& d : r.diags) ...      // syntax errors, unknown commands, no match
//
// The file is Tcl, so the reader is a tcl::Interp (src/tcl.hpp) with the SDC
// commands added: everything Interp offers (host commands, a prompt, puts
// sink, diagnostics with file and line) is there. Collections
// ([get_ports ...]) are passed between commands as handles, as the tools do.
//
// Every command is kept in Constraints::commands with its options parsed and
// its objects resolved; the common ones are also filed into typed lists
// (clocks, ioDelays, exceptions, ...).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tcl.hpp"

namespace sdc {

using tcl::Diag;   // codes: syntax, unknown-command, unknown-option, missing-value,
                   // bad-value, no-match, no-clock, unsupported
using tcl::Error;
using tcl::Loc;
using tcl::fmtNum;
using tcl::globMatch;
using tcl::hasWild;
using tcl::joinList;
using tcl::listElem;
using tcl::splitList;

// ---------------------------------------------------------------- objects

enum class Kind { Port, Pin, Cell, Net, Clock, LibCell };

inline const char* kindName(Kind k) {
    switch (k) {
        case Kind::Port: return "port";
        case Kind::Pin: return "pin";
        case Kind::Cell: return "cell";
        case Kind::Net: return "net";
        case Kind::Clock: return "clock";
        case Kind::LibCell: return "lib_cell";
    }
    return "?";
}

struct Obj {
    Kind kind = Kind::Port;
    std::string name;      // hierarchy separated by '/', bus bits as name[3]
    bool resolved = true;  // false: no design was given, `name` is the pattern as written
    bool operator==(const Obj& o) const { return kind == o.kind && name == o.name; }
    bool operator<(const Obj& o) const { return kind != o.kind ? kind < o.kind : name < o.name; }
};
using ObjList = std::vector<Obj>;

// What all_registers is asked for.
struct RegQuery {
    bool cells = true, clockPins = false, dataPins = false, outputPins = false;
    bool edgeTriggered = true, levelSensitive = true;
    std::vector<std::string> clocks;  // empty: any clock
};

// The design, as the reader needs it. Names use '/' between hierarchy levels.
struct Design {
    virtual ~Design() = default;
    // Every object of a kind (Port, Pin, Cell, Net), by full name.
    virtual const std::vector<std::string>& names(Kind k) const = 0;
    // 0 input, 1 output, 2 inout, -1 unknown.
    virtual int portDir(const std::string&) const { return -1; }
    virtual ObjList registers(const RegQuery&) const { return {}; }
    // get_pins -of_objects [get_cells x], get_nets -of_objects pin, ...
    virtual ObjList related(Kind /*want*/, const Obj& /*of*/) const { return {}; }
    // For -filter: is_sequential, direction, ref_name, ... Nothing by default.
    virtual std::optional<std::string> attribute(const Obj&, const std::string&) const { return std::nullopt; }
};

// ------------------------------------------------------------- the commands

struct Arg {
    std::string opt;    // "-period", or the positional's name ("objects", "delay")
    std::string s;      // the text as written (after substitution)
    double f = 0;       // for a number
    bool isNum = false;
    ObjList objs;       // for an object list
};

struct Command {
    std::string name;
    Loc loc;
    std::vector<Arg> args;

    const Arg* get(std::string_view opt) const {
        for (auto& a : args)
            if (a.opt == opt) return &a;
        return nullptr;
    }
    bool has(std::string_view opt) const { return get(opt) != nullptr; }
    double num(std::string_view opt, double def = 0) const {
        const Arg* a = get(opt);
        return a && a->isNum ? a->f : def;
    }
    std::string str(std::string_view opt) const {
        const Arg* a = get(opt);
        return a ? a->s : std::string();
    }
    ObjList objs(std::string_view opt) const {
        ObjList o;
        for (auto& a : args)
            if (a.opt == opt) o.insert(o.end(), a.objs.begin(), a.objs.end());
        return o;
    }
    std::vector<const Arg*> all(std::string_view opt) const {
        std::vector<const Arg*> o;
        for (auto& a : args)
            if (a.opt == opt) o.push_back(&a);
        return o;
    }
};

// -------------------------------------------------------------- typed model

struct Clock {
    std::string name;
    Loc loc;
    ObjList sources;  // empty: a virtual clock
    bool add = false;
    bool generated = false;
    // create_clock
    double period = 0;
    std::vector<double> waveform;  // rise, fall, ... ; empty: {0 period/2}
    bool hasWaveform = false;
    // create_generated_clock
    ObjList masterPins;         // -source
    std::string masterClock;    // -master_clock, or empty
    int divideBy = 0, multiplyBy = 0;
    double dutyCycle = 0;
    std::vector<double> edges, edgeShift;
    bool invert = false, combinational = false;
    bool isVirtual() const { return sources.empty(); }
};

struct IoDelay {
    bool input = true;
    Loc loc;
    double value = 0;
    std::string clock;  // empty: no -clock
    bool clockFall = false, levelSensitive = false, add = false;
    bool min = false, max = false, rise = false, fall = false;  // none of min/max: both
    ObjList ports;
    ObjList refPin;
};

struct ClockGroups {
    enum Type { Asynchronous, LogicallyExclusive, PhysicallyExclusive, None };
    Type type = None;
    std::string name;
    Loc loc;
    bool allowPaths = false;
    std::vector<std::vector<std::string>> groups;  // clock names
};

struct PathSpec {
    ObjList from, to;
    std::vector<ObjList> through;
    bool riseFrom = false, fallFrom = false, riseTo = false, fallTo = false;
    bool empty() const { return from.empty() && to.empty() && through.empty(); }
};

struct Exception {
    enum Type { FalsePath, Multicycle, MaxDelay, MinDelay, GroupPath };
    Type type = FalsePath;
    Loc loc;
    PathSpec path;
    double value = 0;  // multiplier or delay
    bool setup = false, hold = false, rise = false, fall = false;
    bool start = false, end = false;   // multicycle -start / -end
    bool datapathOnly = false;         // -ignore_clock_latency / -datapath_only
    std::string name;                  // group_path -name
    // Arguments that named nothing: -from [get_ports nope]. The path spec is
    // then not "everything", it is nothing.
    bool emptyEndpoint = false;
};

struct Uncertainty {
    Loc loc;
    double value = 0;
    bool setup = false, hold = false;
    ObjList objects;                  // clocks, ports or pins
    std::vector<std::string> from, to;  // inter-clock form
};

struct Latency {
    Loc loc;
    double value = 0;
    bool source = false, min = false, max = false, rise = false, fall = false, early = false, late = false;
    ObjList objects;
    std::vector<std::string> clocks;  // -clock
};

struct CaseAnalysis {
    Loc loc;
    std::string value;  // 0, 1, rising, falling
    ObjList objects;
};

struct Constraints {
    std::vector<Command> commands;  // everything, in file order
    std::vector<Clock> clocks;      // in file order; a later one of the same name replaces an earlier
    std::vector<IoDelay> ioDelays;
    std::vector<ClockGroups> clockGroups;
    std::vector<Exception> exceptions;
    std::vector<Uncertainty> uncertainties;
    std::vector<Latency> latencies;
    std::vector<CaseAnalysis> caseAnalysis;
    ObjList propagated;             // set_propagated_clock
    std::map<std::string, std::string> units;  // set_units: time -> "ns"
    std::string hierSep = "/";

    const Clock* clock(const std::string& n) const {
        for (size_t i = clocks.size(); i-- > 0;)
            if (clocks[i].name == n) return &clocks[i];
        return nullptr;
    }
    // Every command of one name: of("set_load").
    std::vector<const Command*> of(std::string_view name) const {
        std::vector<const Command*> o;
        for (auto& c : commands)
            if (c.name == name) o.push_back(&c);
        return o;
    }
};

// ------------------------------------------------------------------- reader

class Reader : public tcl::Interp {
public:
    const Design* design = nullptr;
    Constraints sdc;
    // Commands outside SDC that a tool also accepts, in the spec syntax below:
    // extra["set_dont_touch"] = "| objects=o:cn value=s?".
    std::map<std::string, std::string> extra;

    // Threads for the name indexes of a large design (every logical core by
    // default; the result does not depend on it).
    int threads = std::max(1u, std::thread::hardware_concurrency());
    Reader() { busBrackets = true; }  // get_ports data[3] needs no braces

    // The objects behind a word: collection handles and bare names.
    ObjList objects(const std::string& word, const std::string& kinds = "kpicn") {
        return toObjs(word, kinds, here(), false);
    }

protected:
    bool command(const std::vector<std::string>& w, std::string& result) override {
        bool handled = true;
        result = sdcCommand(w, handled);
        return handled;
    }

private:
    std::vector<ObjList> cols_;
    std::set<std::string> warnedFilter_;

    // -------------------------------------------------------- collections
    static constexpr const char* kColPrefix = "_sdc_sel";
    std::string newCol(ObjList o) {
        cols_.push_back(std::move(o));
        return kColPrefix + std::to_string(cols_.size() - 1);
    }
    const ObjList* colOf(const std::string& w) const {
        if (w.rfind(kColPrefix, 0) != 0) return nullptr;
        char* e = nullptr;
        unsigned long i = strtoul(w.c_str() + strlen(kColPrefix), &e, 10);
        if (*e || i >= cols_.size()) return nullptr;
        return &cols_[i];
    }

    struct Clause { std::string attr, want; int op = 0; };  // op: 0 ==, 1 !=, 2 =~, 3 !~
    using Filter = std::vector<std::vector<Clause>>;         // any of (all of)
    // Name indexes per kind, built on first use: sorted by full name and by
    // leaf name (the part after the last '/'), so a pattern with a literal
    // start is a range lookup and not a scan of the design.
    struct Index { std::vector<uint32_t> byName, byLeaf; };
    std::map<Kind, Index> index_;
    static std::string_view leafOf(const std::string& s) {
        size_t p = s.rfind('/');
        return p == std::string::npos ? std::string_view(s) : std::string_view(s).substr(p + 1);
    }
    // Sorts on `threads` threads: each sorts a slice, then the slices are
    // merged pairwise. The result is the same for any thread count.
    template <typename Less>
    void sortIndex(std::vector<uint32_t>& v, Less less) {
        size_t n = v.size();
        int t = std::max(1, std::min(threads, 64));
        if (t == 1 || n < 50000) { std::sort(v.begin(), v.end(), less); return; }
        std::vector<size_t> cut(t + 1);
        for (int i = 0; i <= t; i++) cut[i] = n * i / t;
        std::vector<std::thread> pool;
        for (int i = 1; i < t; i++) pool.emplace_back([&, i] { std::sort(v.begin() + cut[i], v.begin() + cut[i + 1], less); });
        std::sort(v.begin(), v.begin() + cut[1], less);
        for (auto& th : pool) th.join();
        for (int step = 1; step < t; step *= 2) {
            pool.clear();
            for (int i = 0; i + step < t; i += 2 * step)
                pool.emplace_back([&, i, step] {
                    std::inplace_merge(v.begin() + cut[i], v.begin() + cut[i + step], v.begin() + cut[std::min(t, i + 2 * step)], less);
                });
            for (auto& th : pool) th.join();
        }
    }
    const std::vector<uint32_t>& indexOf(Kind k, bool leaf) {
        const auto& names = design->names(k);
        Index& ix = index_[k];
        auto& v = leaf ? ix.byLeaf : ix.byName;
        if (v.size() == names.size()) return v;
        v.resize(names.size());
        for (uint32_t i = 0; i < names.size(); i++) v[i] = i;
        if (leaf)
            sortIndex(v, [&](uint32_t a, uint32_t b) {
                auto la = leafOf(names[a]), lb = leafOf(names[b]);
                return la != lb ? la < lb : a < b;
            });
        else
            sortIndex(v, [&](uint32_t a, uint32_t b) { return names[a] != names[b] ? names[a] < names[b] : a < b; });
        return v;
    }

    struct Query {
        bool hier = false, regexp = false, nocase = false;
    };
    // Design objects of a kind matching one pattern.
    void findIn(Kind k, const std::string& pat, const Query& q, ObjList& out, const Filter* flt = nullptr) {
        auto keep = [&](const std::string& n) { return !flt || filterOne(k, n, *flt); };
        if (k == Kind::Clock) {
            std::set<std::string> seen;
            for (auto& c : sdc.clocks)
                if (matchName(pat, c.name, q) && seen.insert(c.name).second && keep(c.name)) out.push_back({Kind::Clock, c.name, true});
            return;
        }
        if (k == Kind::LibCell || !design) return;
        const auto& names = design->names(k);
        if (q.regexp || q.nocase) {
            for (auto& n : names)
                if (matchName(pat, n, q) && keep(n)) out.push_back({k, n, true});
            return;
        }
        std::string p = normalized(pat);
        bool leaf = q.hier && p.find('/') == std::string::npos;
        if (q.hier && !leaf) {
            for (auto& n : names)
                if (globName(p, n.c_str(), true) && keep(n)) out.push_back({k, n, true});
            return;
        }
        // The names (or leaves) that start with the literal start of the pattern.
        std::string lit = p.substr(0, p.find_first_of("*?\\"));
        const auto& order = indexOf(k, leaf);
        auto key = [&](uint32_t i) { return leaf ? leafOf(names[i]) : std::string_view(names[i]); };
        auto lo = std::lower_bound(order.begin(), order.end(), std::string_view(lit),
                                   [&](uint32_t i, std::string_view v) { return key(i).substr(0, v.size()) < v; });
        std::vector<uint32_t> hits;
        for (auto it = lo; it != order.end() && key(*it).substr(0, lit.size()) == lit; ++it)
            if (globName(p, leaf ? key(*it).data() : names[*it].c_str(), false) && keep(names[*it])) hits.push_back(*it);
        std::sort(hits.begin(), hits.end());  // the design's own order
        for (uint32_t i : hits) out.push_back({k, names[i], true});
    }
    // The pattern with the file's hierarchy separator turned into '/'.
    std::string normalized(const std::string& pat) const {
        std::string p = pat;
        char sep = sdc.hierSep.empty() ? '/' : sdc.hierSep[0];
        if (sep != '/')
            for (char& c : p) if (c == sep) c = '/';
        return p;
    }
    std::string busBase_;
    bool globName(const std::string& p, const char* n, bool hier) {
        auto one = [&](const char* s) {
            if (globMatch(p.c_str(), s, false)) return true;
            // "d" names every bit of the bus d[7:0].
            if (p.find('[') == std::string::npos) {
                const char* b = strrchr(s, '[');
                if (b && s[strlen(s) - 1] == ']' && !strchr(b, '/')) {
                    busBase_.assign(s, b - s);
                    return globMatch(p.c_str(), busBase_.c_str(), false);
                }
            }
            return false;
        };
        if (one(n)) return true;
        if (hier)  // the pattern names the last levels, at any depth
            for (const char* c = n; *c; c++)
                if (*c == '/' && one(c + 1)) return true;
        return false;
    }
    std::map<std::pair<std::string, bool>, std::regex> regexes_;
    bool matchName(const std::string& pat, const std::string& name, const Query& q) {
        if (q.regexp) {
            auto it = regexes_.find({pat, q.nocase});
            if (it == regexes_.end()) {
                try {
                    it = regexes_.emplace(std::make_pair(pat, q.nocase),
                                          std::regex(pat, q.nocase ? std::regex::icase | std::regex::ECMAScript : std::regex::ECMAScript)).first;
                } catch (const std::regex_error&) {
                    fail("bad regular expression \"" + pat + "\"");
                }
            }
            return std::regex_match(name, it->second);
        }
        std::string p = normalized(pat), n = name;
        if (q.nocase) {
            for (char& c : p) c = (char)tolower((unsigned char)c);
            for (char& c : n) c = (char)tolower((unsigned char)c);
        }
        return globName(p, n.c_str(), q.hier);
    }

    // A word given where objects are expected: handles, and bare names looked
    // up as the first kind in `kinds` that has them (k clock, p port, i pin,
    // c cell, n net, l lib cell).
    static Kind kindOf(char c) {
        switch (c) {
            case 'k': return Kind::Clock;
            case 'p': return Kind::Port;
            case 'i': return Kind::Pin;
            case 'c': return Kind::Cell;
            case 'n': return Kind::Net;
            default: return Kind::LibCell;
        }
    }
    ObjList toObjs(const std::string& word, const std::string& kinds, const Loc& loc, bool quiet) {
        ObjList out;
        if (const ObjList* c = colOf(word)) return *c;
        for (auto& e : splitList(word)) {
            if (const ObjList* c = colOf(e)) {
                out.insert(out.end(), c->begin(), c->end());
                continue;
            }
            size_t before = out.size();
            Kind firstDesign = Kind::Clock;
            bool anyDesign = false;
            for (char kc : kinds) {
                Kind k = kindOf(kc);
                if (k == Kind::LibCell) { out.push_back({k, e, false}); break; }
                if (k != Kind::Clock && !anyDesign) { firstDesign = k; anyDesign = true; }
                findIn(k, e, Query{}, out);
                if (out.size() > before) break;
            }
            if (out.size() > before) continue;
            if (!design && anyDesign) {
                out.push_back({firstDesign, e, false});
            } else if (!quiet) {
                std::string what;
                for (char kc : kinds) {
                    if (!what.empty()) what += ", ";
                    what += kindName(kindOf(kc));
                }
                diag(Diag::Warning, kinds == "k" ? "no-clock" : "no-match", loc, "nothing matches '" + e + "' (looked for: " + what + ")");
            }
        }
        return out;
    }

    // -filter {a == b && c =~ d*}: parsed once, then run on each object.
    std::map<std::string, Filter> filters_;
    const Filter& filterOf(const std::string& expr) {
        auto it = filters_.find(expr);
        if (it != filters_.end()) return it->second;
        auto splitOn = [](const std::string& s, const char* op) {
            std::vector<std::string> out;
            size_t i = 0;
            for (;;) {
                size_t j = s.find(op, i);
                out.push_back(s.substr(i, j == std::string::npos ? j : j - i));
                if (j == std::string::npos) break;
                i = j + 2;
            }
            return out;
        };
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t\n\"{}"), b = s.find_last_not_of(" \t\n\"{}");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        Filter f;
        for (auto& orTerm : splitOn(expr, "||")) {
            std::vector<Clause> all;
            for (auto& term : splitOn(orTerm, "&&")) {
                std::string t = trim(term);
                while (!t.empty() && t[0] == '(') t = trim(t.substr(1));
                while (!t.empty() && t.back() == ')') { t.pop_back(); t = trim(t); }
                if (!t.empty() && t[0] == '@') t.erase(0, 1);
                static const char* ops[] = {"==", "!=", "=~", "!~"};
                size_t at = std::string::npos;
                int which = -1;
                for (int k = 0; k < 4; k++) {
                    size_t j = t.find(ops[k]);
                    if (j != std::string::npos && j < at) { at = j; which = k; }
                }
                Clause c;
                c.attr = trim(which < 0 ? t : t.substr(0, at));
                c.want = which < 0 ? "true" : trim(t.substr(at + 2));
                c.op = which < 0 ? 0 : which;
                all.push_back(std::move(c));
            }
            f.push_back(std::move(all));
        }
        return filters_[expr] = std::move(f);
    }
    bool filterOne(const Obj& o, const std::string& expr) { return filterOne(o.kind, o.name, filterOf(expr)); }
    bool filterOne(Kind kind, const std::string& name, const Filter& f) {
        for (auto& all : f) {
            bool ok = true;
            for (auto& c : all) {
                std::optional<std::string> have;
                const char* h = nullptr;
                if (c.attr == "full_name") h = name.c_str();
                else if (c.attr == "name") { size_t sl = name.rfind('/'); h = name.c_str() + (sl == std::string::npos ? 0 : sl + 1); }
                else if (c.attr == "object_class") h = kindName(kind);
                else if (c.attr == "direction" && kind == Kind::Port && design) {
                    int d = design->portDir(name);
                    if (d >= 0) h = d == 0 ? "in" : d == 1 ? "out" : "inout";
                }
                if (!h && design) {
                    have = design->attribute(Obj{kind, name, true}, c.attr);
                    if (have) h = have->c_str();
                }
                if (!h) {
                    if (warnedFilter_.insert(c.attr).second)
                        diag(Diag::Warning, "unsupported", here(), "-filter: the attribute '" + c.attr + "' is not known for this design; the filter keeps every object");
                    continue;
                }
                bool m = c.op >= 2 ? globMatch(c.want.c_str(), h, true) : c.want == h;
                if (c.op == 1 || c.op == 3) m = !m;
                if (!m) { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    }

    std::string cmdGet(Kind k, const std::vector<std::string>& w) {
        Query q;
        bool quiet = false;
        std::string filter;
        std::vector<std::string> pats;
        ObjList of;
        bool hasOf = false;
        for (size_t i = 1; i < w.size(); i++) {
            const std::string& a = w[i];
            auto is = [&](const char* full, size_t min) { return a.size() >= min && std::string(full).rfind(a, 0) == 0; };
            if (a.size() > 1 && a[0] == '-') {
                if (is("-hierarchical", 2)) q.hier = true;
                else if (is("-regexp", 2)) q.regexp = true;
                else if (is("-nocase", 2)) q.nocase = true;
                else if (is("-quiet", 2)) quiet = true;
                else if (a == "-exact" || a == "-leaf" || a == "-top_net_of_hierarchical_group" || a == "-segments" || a == "-physical_context") {}
                else if (is("-filter", 2)) { if (++i >= w.size()) fail(w[0] + ": -filter needs an expression"); filter = w[i]; }
                else if (is("-hsc", 3)) { if (++i >= w.size()) fail(w[0] + ": -hsc needs a character"); }
                else if (is("-of_objects", 2)) {
                    if (++i >= w.size()) fail(w[0] + ": -of_objects needs objects");
                    hasOf = true;
                    ObjList o = toObjs(w[i], "icnp", here(), quiet);
                    of.insert(of.end(), o.begin(), o.end());
                } else {
                    diag(Diag::ErrorSev, "unknown-option", here(), w[0] + ": unknown option " + a);
                }
                continue;
            }
            for (auto& e : splitList(a)) pats.push_back(e);
        }
        ObjList out;
        if (hasOf) {
            std::set<Obj> seen;
            for (auto& o : of) {
                if (!design) { out.push_back({k, o.name, false}); continue; }
                for (auto& r : design->related(k, o))
                    if (seen.insert(r).second) out.push_back(r);
            }
            if (!pats.empty()) {
                ObjList keep;
                for (auto& o : out)
                    for (auto& p : pats)
                        if (matchName(p, o.name, Query{true, q.regexp, q.nocase})) { keep.push_back(o); break; }
                out = keep;
            }
        } else {
            if (pats.empty()) pats.push_back("*");
            const Filter* flt = filter.empty() ? nullptr : &filterOf(filter);
            bool single = pats.size() == 1;  // one pattern cannot name an object twice
            std::set<Obj> seen;
            for (auto& p : pats) {
                if (const ObjList* c = colOf(p)) {  // get_ports [get_ports x]
                    for (auto& o : *c)
                        if (o.kind == k && seen.insert(o).second) out.push_back(o);
                    continue;
                }
                ObjList found;
                bool any = false;
                if (flt) {  // an empty result after the filter is not "no match"
                    ObjList probe;
                    findIn(k, p, q, found, flt);
                    any = !found.empty();
                    if (!any) { findIn(k, p, q, probe); any = !probe.empty(); }
                } else {
                    findIn(k, p, q, found);
                    any = !found.empty();
                }
                if (single && any) { out = std::move(found); break; }
                if (!any) {
                    if (!design && k != Kind::Clock) found.push_back({k, p, false});
                    else if (k == Kind::LibCell) found.push_back({k, p, false});
                    else if (!quiet)
                        diag(Diag::Warning, k == Kind::Clock ? "no-clock" : "no-match", here(),
                             std::string("no ") + kindName(k) + " matches '" + p + "'");
                }
                for (auto& o : found)
                    if (seen.insert(o).second) out.push_back(o);
            }
        }
        if (!filter.empty() && hasOf) {
            ObjList keep;
            for (auto& o : out)
                if (!o.resolved || filterOne(o, filter)) keep.push_back(o);
            out = keep;
        }
        return newCol(std::move(out));
    }

    // ----------------------------------------------------------- SDC specs
    // "options | positionals". An option is -name (a flag) or -name=T; a
    // positional is name=T, with ? when it may be left out. T is f (number),
    // i (integer), s (string), o:KINDS (objects). A trailing * on an option
    // means it may be given more than once.
    struct OptSpec {
        std::string name;
        char type = 0;  // 0 flag, f, i, s, o
        std::string kinds;
        bool optional = false;
    };
    struct CmdSpec {
        std::vector<OptSpec> opts, pos;
    };
    std::map<std::string, CmdSpec> specs_;

    static CmdSpec parseSpec(const std::string& text) {
        CmdSpec cs;
        bool pos = false;
        std::istringstream in(text);
        std::string t;
        while (in >> t) {
            if (t == "|") { pos = true; continue; }
            OptSpec o;
            if (!t.empty() && t.back() == '*') t.pop_back();
            if (!t.empty() && t.back() == '?') { o.optional = true; t.pop_back(); }
            size_t eq = t.find('=');
            o.name = t.substr(0, eq);
            if (eq != std::string::npos) {
                o.type = t[eq + 1];
                if (o.type == 'o') o.kinds = t.substr(eq + 3);
            }
            (pos ? cs.pos : cs.opts).push_back(o);
        }
        return cs;
    }

    void initSpecs() {
        if (!specs_.empty()) return;
        const std::string PATH =
            " -from=o:kpic -to=o:kpic -through=o:picn* -rise_from=o:kpic -fall_from=o:kpic -rise_to=o:kpic"
            " -fall_to=o:kpic -rise_through=o:picn* -fall_through=o:picn* ";
        const std::pair<const char*, std::string> table[] = {
            {"create_clock", "-period=f -name=s -waveform=s -add -comment=s | objects=o:pin?"},
            {"create_generated_clock",
             "-name=s -source=o:ipn -edges=s -divide_by=i -multiply_by=i -duty_cycle=f -invert -edge_shift=s -add "
             "-master_clock=o:k -combinational -comment=s | objects=o:pin?"},
            {"group_path", "-name=s -default -weight=f -comment=s" + PATH + "|"},
            {"set_clock_gating_check", "-setup=f -hold=f -rise -fall -high -low | objects=o:kci?"},
            {"set_clock_groups",
             "-name=s -physically_exclusive -logically_exclusive -asynchronous -allow_paths -group=o:k* -comment=s |"},
            {"set_clock_latency", "-rise -fall -min -max -source -dynamic -late -early -clock=o:k | delay=f objects=o:kip"},
            {"set_clock_sense", "-positive -negative -stop_propagation -pulse=s -clocks=o:k -clock=o:k | pins=o:ip"},
            {"set_sense", "-type=s -non_unate -positive -negative -clock_leaf -stop_propagation -pulse=s -clocks=o:k -clock=o:k | pins=o:ip"},
            {"set_clock_transition", "-rise -fall -min -max | transition=f clocks=o:k"},
            {"set_clock_uncertainty",
             "-from=o:k -rise_from=o:k -fall_from=o:k -to=o:k -rise_to=o:k -fall_to=o:k -rise -fall -setup -hold | "
             "uncertainty=f objects=o:kip?"},
            {"set_data_check", "-from=o:ip -to=o:ip -rise_from=o:ip -fall_from=o:ip -rise_to=o:ip -fall_to=o:ip -setup -hold -clock=o:k | value=f"},
            {"set_disable_timing", "-from=s -to=s | objects=o:cipl"},
            {"set_false_path", "-setup -hold -rise -fall -comment=s" + PATH + "|"},
            {"set_ideal_latency", "-rise -fall -min -max | delay=f objects=o:ipc"},
            {"set_ideal_network", "-no_propagate | objects=o:ipnc"},
            {"set_ideal_transition", "-rise -fall -min -max | transition=f objects=o:ipc"},
            {"set_input_delay",
             "-clock=o:k -reference_pin=o:ip -clock_fall -level_sensitive -rise -fall -max -min -add_delay "
             "-network_latency_included -source_latency_included | delay=f objects=o:pi"},
            {"set_output_delay",
             "-clock=o:k -reference_pin=o:ip -clock_fall -level_sensitive -rise -fall -max -min -add_delay "
             "-network_latency_included -source_latency_included | delay=f objects=o:pi"},
            {"set_max_delay", "-rise -fall -ignore_clock_latency -datapath_only -comment=s" + PATH + "| delay=f"},
            {"set_min_delay", "-rise -fall -ignore_clock_latency -comment=s" + PATH + "| delay=f"},
            {"set_max_time_borrow", "| delay=f objects=o:kic"},
            {"set_min_pulse_width", "-low -high | value=f objects=o:kicp?"},
            {"set_multicycle_path", "-setup -hold -rise -fall -start -end -comment=s" + PATH + "| multiplier=i"},
            {"set_propagated_clock", "| objects=o:kip"},
            {"set_case_analysis", "| value=s objects=o:pi"},
            {"set_drive", "-rise -fall -min -max | resistance=f objects=o:p"},
            {"set_driving_cell",
             "-lib_cell=s -rise -fall -min -max -library=s -pin=s -from_pin=s -multiply_by=f -dont_scale "
             "-no_design_rule -input_transition_rise=f -input_transition_fall=f -clock=o:k -clock_fall | objects=o:p"},
            {"set_fanout_load", "| value=f objects=o:p"},
            {"set_input_transition", "-rise -fall -min -max -clock=o:k -clock_fall | transition=f objects=o:p"},
            {"set_load", "-min -max -subtract_pin_load -pin_load -wire_load | value=f objects=o:pn"},
            {"set_logic_dc", "| objects=o:pi"},
            {"set_logic_one", "| objects=o:pi"},
            {"set_logic_zero", "| objects=o:pi"},
            {"set_max_area", "| value=f"},
            {"set_max_capacitance", "-clock_path -data_path -rise -fall | value=f objects=o:pkic?"},
            {"set_min_capacitance", "| value=f objects=o:pic?"},
            {"set_max_fanout", "| value=f objects=o:pic?"},
            {"set_max_transition", "-clock_path -data_path -rise -fall | value=f objects=o:pkic?"},
            {"set_port_fanout_number", "| value=i objects=o:p"},
            {"set_resistance", "-min -max | value=f objects=o:n"},
            {"set_timing_derate",
             "-cell_delay -cell_check -net_delay -data -clock -early -late -rise -fall -static -dynamic -increment | "
             "value=f objects=o:cnl?"},
            {"set_voltage", "-min=f -object_list=o:nip | value=f"},
            {"set_wire_load_min_block_size", "| size=f"},
            {"set_wire_load_mode", "| mode=s"},
            {"set_wire_load_model", "-name=s -library=s -min -max | objects=o:cp?"},
            {"set_wire_load_selection_group", "-library=s -min -max | group=s objects=o:c?"},
            {"set_operating_conditions",
             "-library=s -analysis_type=s -max=s -min=s -max_library=s -min_library=s -object_list=o:cp | condition=s?"},
            {"set_units", "-time=s -capacitance=s -resistance=s -voltage=s -current=s -power=s |"},
            {"set_max_dynamic_power", "| value=f unit=s?"},
            {"set_max_leakage_power", "| value=f unit=s?"},
            {"create_voltage_area", "-name=s -coordinate=s -guard_band_x=f -guard_band_y=f | cells=o:c"},
            {"set_level_shifter_strategy", "-rule=s |"},
            {"set_level_shifter_threshold", "-voltage=f -percent=f |"},
        };
        for (auto& [n, t] : table) specs_[n] = parseSpec(t);
        for (auto& [n, t] : extra) specs_[n] = parseSpec(t);
    }

    static bool looksNumber(const std::string& s) {
        if (s.empty()) return false;
        char* e = nullptr;
        strtod(s.c_str(), &e);
        return e != s.c_str() && !*e;
    }

    bool runSdc(const std::vector<std::string>& w) {
        initSpecs();
        auto it = specs_.find(w[0]);
        if (it == specs_.end()) return false;
        const CmdSpec& cs = it->second;
        Command c;
        c.name = w[0];
        c.loc = here();
        size_t posIdx = 0;
        auto fill = [&](const OptSpec& o, const std::string& val) {
            Arg a;
            a.opt = o.name;
            a.s = val;
            if (o.type == 'f' || o.type == 'i') {
                char* e = nullptr;
                a.f = strtod(val.c_str(), &e);
                a.isNum = e != val.c_str() && !*e;
                if (!a.isNum)
                    diag(Diag::ErrorSev, "bad-value", c.loc, c.name + ": " + o.name + " needs a number, not '" + val + "'");
                else if (o.type == 'i' && a.f != std::floor(a.f))
                    diag(Diag::ErrorSev, "bad-value", c.loc, c.name + ": " + o.name + " needs an integer, not '" + val + "'");
            } else if (o.type == 'o') {
                a.objs = toObjs(val, o.kinds, c.loc, false);
            }
            c.args.push_back(std::move(a));
        };
        for (size_t i = 1; i < w.size(); i++) {
            const std::string& a = w[i];
            if (a.size() > 1 && a[0] == '-' && !looksNumber(a)) {
                // An option, or a unique prefix of one.
                const OptSpec* hit = nullptr;
                int n = 0;
                for (auto& o : cs.opts) {
                    if (o.name == a) { hit = &o; n = 1; break; }
                    if (o.name.rfind(a, 0) == 0) { hit = &o; n++; }
                }
                if (n != 1) {
                    diag(Diag::ErrorSev, "unknown-option", c.loc, c.name + (n ? ": ambiguous option " : ": unknown option ") + a);
                    continue;
                }
                if (hit->type == 0) {
                    Arg f;
                    f.opt = hit->name;
                    c.args.push_back(f);
                } else if (i + 1 >= w.size()) {
                    diag(Diag::ErrorSev, "missing-value", c.loc, c.name + ": " + hit->name + " needs a value");
                } else {
                    fill(*hit, w[++i]);
                }
                continue;
            }
            if (posIdx >= cs.pos.size()) {
                diag(Diag::ErrorSev, "bad-value", c.loc, c.name + ": unexpected argument '" + a + "'");
                continue;
            }
            fill(cs.pos[posIdx++], a);
        }
        for (; posIdx < cs.pos.size(); posIdx++)
            if (!cs.pos[posIdx].optional)
                diag(Diag::ErrorSev, "missing-value", c.loc, c.name + ": missing " + cs.pos[posIdx].name);
        file(c);
        sdc.commands.push_back(std::move(c));
        return true;
    }

    static std::vector<double> numList(const std::string& s) {
        std::vector<double> o;
        for (auto& e : splitList(s)) o.push_back(strtod(e.c_str(), nullptr));
        return o;
    }
    static std::vector<std::string> clockNames(const ObjList& o) {
        std::vector<std::string> n;
        for (auto& x : o)
            if (x.kind == Kind::Clock) n.push_back(x.name);
        return n;
    }

    PathSpec pathOf(const Command& c, bool& emptyEndpoint) {
        PathSpec p;
        for (auto& a : c.args) {
            bool isPath = true;
            if (a.opt == "-from" || a.opt == "-rise_from" || a.opt == "-fall_from") {
                p.from.insert(p.from.end(), a.objs.begin(), a.objs.end());
                p.riseFrom |= a.opt == "-rise_from";
                p.fallFrom |= a.opt == "-fall_from";
            } else if (a.opt == "-to" || a.opt == "-rise_to" || a.opt == "-fall_to") {
                p.to.insert(p.to.end(), a.objs.begin(), a.objs.end());
                p.riseTo |= a.opt == "-rise_to";
                p.fallTo |= a.opt == "-fall_to";
            } else if (a.opt == "-through" || a.opt == "-rise_through" || a.opt == "-fall_through") {
                p.through.push_back(a.objs);
            } else {
                isPath = false;
            }
            if (isPath && a.objs.empty()) emptyEndpoint = true;
        }
        return p;
    }

    // Files a parsed command into the typed lists.
    void file(const Command& c) {
        const std::string& n = c.name;
        if (n == "create_clock" || n == "create_generated_clock") {
            Clock k;
            k.loc = c.loc;
            k.sources = c.objs("objects");
            k.add = c.has("-add");
            k.name = c.str("-name");
            if (k.name.empty() && !k.sources.empty()) k.name = k.sources[0].name;
            if (n == "create_clock") {
                k.period = c.num("-period");
                if (c.has("-waveform")) { k.waveform = numList(c.str("-waveform")); k.hasWaveform = true; }
                else k.waveform = {0, k.period / 2};
            } else {
                k.generated = true;
                k.masterPins = c.objs("-source");
                auto mc = clockNames(c.objs("-master_clock"));
                if (!mc.empty()) k.masterClock = mc[0];
                k.divideBy = (int)c.num("-divide_by");
                k.multiplyBy = (int)c.num("-multiply_by");
                k.dutyCycle = c.num("-duty_cycle");
                k.edges = numList(c.str("-edges"));
                k.edgeShift = numList(c.str("-edge_shift"));
                k.invert = c.has("-invert");
                k.combinational = c.has("-combinational");
            }
            sdc.clocks.push_back(std::move(k));
        } else if (n == "set_input_delay" || n == "set_output_delay") {
            IoDelay d;
            d.input = n == "set_input_delay";
            d.loc = c.loc;
            d.value = c.num("delay");
            auto ck = clockNames(c.objs("-clock"));
            if (!ck.empty()) d.clock = ck[0];
            else if (auto* a = c.get("-clock"); a && a->objs.empty()) d.clock = a->s;  // named, but not defined
            d.clockFall = c.has("-clock_fall");
            d.levelSensitive = c.has("-level_sensitive");
            d.add = c.has("-add_delay");
            d.min = c.has("-min"); d.max = c.has("-max"); d.rise = c.has("-rise"); d.fall = c.has("-fall");
            d.ports = c.objs("objects");
            d.refPin = c.objs("-reference_pin");
            sdc.ioDelays.push_back(std::move(d));
        } else if (n == "set_clock_groups") {
            ClockGroups g;
            g.loc = c.loc;
            g.name = c.str("-name");
            g.allowPaths = c.has("-allow_paths");
            g.type = c.has("-asynchronous") ? ClockGroups::Asynchronous
                     : c.has("-logically_exclusive") ? ClockGroups::LogicallyExclusive
                     : c.has("-physically_exclusive") ? ClockGroups::PhysicallyExclusive : ClockGroups::None;
            for (const Arg* a : c.all("-group")) g.groups.push_back(clockNames(a->objs));
            sdc.clockGroups.push_back(std::move(g));
        } else if (n == "set_false_path" || n == "set_multicycle_path" || n == "set_max_delay" || n == "set_min_delay" ||
                   n == "group_path") {
            Exception e;
            e.loc = c.loc;
            e.type = n == "set_false_path" ? Exception::FalsePath : n == "set_multicycle_path" ? Exception::Multicycle
                     : n == "set_max_delay" ? Exception::MaxDelay : n == "set_min_delay" ? Exception::MinDelay : Exception::GroupPath;
            e.path = pathOf(c, e.emptyEndpoint);
            e.value = e.type == Exception::Multicycle ? c.num("multiplier") : c.num("delay");
            e.setup = c.has("-setup"); e.hold = c.has("-hold"); e.rise = c.has("-rise"); e.fall = c.has("-fall");
            e.start = c.has("-start"); e.end = c.has("-end");
            e.datapathOnly = c.has("-ignore_clock_latency") || c.has("-datapath_only");
            e.name = c.str("-name");
            sdc.exceptions.push_back(std::move(e));
        } else if (n == "set_clock_uncertainty") {
            Uncertainty u;
            u.loc = c.loc;
            u.value = c.num("uncertainty");
            u.setup = c.has("-setup"); u.hold = c.has("-hold");
            u.objects = c.objs("objects");
            for (const char* o : {"-from", "-rise_from", "-fall_from"})
                for (auto& x : clockNames(c.objs(o))) u.from.push_back(x);
            for (const char* o : {"-to", "-rise_to", "-fall_to"})
                for (auto& x : clockNames(c.objs(o))) u.to.push_back(x);
            sdc.uncertainties.push_back(std::move(u));
        } else if (n == "set_clock_latency") {
            Latency l;
            l.loc = c.loc;
            l.value = c.num("delay");
            l.source = c.has("-source"); l.min = c.has("-min"); l.max = c.has("-max");
            l.rise = c.has("-rise"); l.fall = c.has("-fall"); l.early = c.has("-early"); l.late = c.has("-late");
            l.objects = c.objs("objects");
            l.clocks = clockNames(c.objs("-clock"));
            sdc.latencies.push_back(std::move(l));
        } else if (n == "set_case_analysis") {
            sdc.caseAnalysis.push_back({c.loc, c.str("value"), c.objs("objects")});
        } else if (n == "set_propagated_clock") {
            for (auto& o : c.objs("objects")) sdc.propagated.push_back(o);
        } else if (n == "set_units") {
            for (auto& a : c.args) sdc.units[a.opt.substr(1)] = a.s;
        }
    }

    std::string sdcCommand(const std::vector<std::string>& w, bool& handled) {
        const std::string& c = w[0];
        if (runSdc(w)) return "";
        // ---- object queries
        if (c == "get_ports") return cmdGet(Kind::Port, w);
        if (c == "get_pins") return cmdGet(Kind::Pin, w);
        if (c == "get_cells") return cmdGet(Kind::Cell, w);
        if (c == "get_nets") return cmdGet(Kind::Net, w);
        if (c == "get_clocks") return cmdGet(Kind::Clock, w);
        if (c == "get_lib_cells" || c == "get_libs" || c == "get_lib_pins") return cmdGet(Kind::LibCell, w);
        if (c == "all_clocks") {
            ObjList o;
            findIn(Kind::Clock, "*", Query{}, o);
            return newCol(o);
        }
        if (c == "all_inputs" || c == "all_outputs") {
            bool in = c == "all_inputs";
            ObjList o;
            if (!design) o.push_back({Kind::Port, "*", false});
            else
                for (auto& p : design->names(Kind::Port)) {
                    int d = design->portDir(p);
                    if (d == 2 || d == (in ? 0 : 1)) o.push_back({Kind::Port, p, true});
                }
            return newCol(o);
        }
        if (c == "all_registers") {
            RegQuery q;
            bool cells = false, any = false, edge = false, level = false;
            for (size_t i = 1; i < w.size(); i++) {
                const std::string& a = w[i];
                if (a == "-cells") cells = true;
                else if (a == "-clock_pins") q.clockPins = any = true;
                else if (a == "-data_pins") q.dataPins = any = true;
                else if (a == "-output_pins") q.outputPins = any = true;
                else if (a == "-edge_triggered") edge = true;
                else if (a == "-level_sensitive") level = true;
                else if (a == "-no_hierarchy" || a == "-hsc" || a == "-master_slave" || a == "-async_pins" || a == "-slave_clock_pins") {}
                else if ((a == "-clock" || a == "-rise_clock" || a == "-fall_clock") && i + 1 < w.size())
                    q.clocks = clockNames(toObjs(w[++i], "k", here(), false));
                else diag(Diag::ErrorSev, "unknown-option", here(), "all_registers: unknown option " + a);
            }
            q.cells = cells || !any;
            if (edge != level) { q.edgeTriggered = edge; q.levelSensitive = level; }
            ObjList o;
            if (design) o = design->registers(q);
            else o.push_back({Kind::Cell, "*", false});
            return newCol(o);
        }
        if (c == "current_design") return "";
        if (c == "current_instance") {
            if (w.size() > 1 && w[1] != "/" && w[1] != ".")
                diag(Diag::Warning, "unsupported", here(), "current_instance " + w[1] + ": names are read from the top");
            return "";
        }
        if (c == "set_hierarchy_separator") {
            wantArgs(w, 1, 1, "separator");
            sdc.hierSep = w[1];
            return "";
        }
        // ---- collections
        if (c == "sizeof_collection") {
            wantArgs(w, 1, 1, "collection");
            return std::to_string(toObjs(w[1], "kpicn", here(), true).size());
        }
        if (c == "get_object_name" || c == "get_object_names" || c == "get_name") {
            wantArgs(w, 1, 1, "collection");
            std::vector<std::string> n;
            for (auto& o : toObjs(w[1], "kpicn", here(), true)) n.push_back(o.name);
            return joinList(n);
        }
        if (c == "add_to_collection" || c == "append_to_collection") {
            ObjList o;
            std::set<Obj> seen;
            size_t first = 1;
            bool append = c == "append_to_collection";
            std::string var;
            if (append) {
                wantArgs(w, 2, 3, "var objects");
                var = w[1];
                first = 2;
                if (hasVar(var)) for (auto& x : toObjs(getVar(var), "kpicn", here(), true)) if (seen.insert(x).second) o.push_back(x);
            }
            for (size_t i = first; i < w.size(); i++) {
                if (w[i] == "-unique") continue;
                for (auto& x : toObjs(w[i], "kpicn", here(), true))
                    if (seen.insert(x).second) o.push_back(x);
            }
            std::string h = newCol(o);
            if (append) setVar(var, h);
            return h;
        }
        if (c == "remove_from_collection") {
            wantArgs(w, 2, 2, "collection objects");
            ObjList a = toObjs(w[1], "kpicn", here(), true), b = toObjs(w[2], "kpicn", here(), true), o;
            std::set<Obj> drop(b.begin(), b.end());
            for (auto& x : a)
                if (!drop.count(x)) o.push_back(x);
            return newCol(o);
        }
        if (c == "filter_collection") {
            wantArgs(w, 2, 2, "collection expression");
            ObjList o;
            for (auto& x : toObjs(w[1], "kpicn", here(), true))
                if (!x.resolved || filterOne(x, w[2])) o.push_back(x);
            return newCol(o);
        }
        if (c == "index_collection") {
            wantArgs(w, 2, 2, "collection index");
            ObjList a = toObjs(w[1], "kpicn", here(), true);
            long i = strtol(w[2].c_str(), nullptr, 10);
            if (i < 0 || (size_t)i >= a.size()) fail("index_collection: index out of range");
            return newCol({a[i]});
        }
        if (c == "foreach_in_collection") {
            wantArgs(w, 3, 3, "var collection body");
            int bl = line_;
            for (auto& x : toObjs(w[2], "kpicn", here(), true)) {
                setVar(w[1], newCol({x}));
                try { eval(w[3], bl); } catch (const Brk&) { break; } catch (const Cont&) {}
            }
            return "";
        }
        handled = false;
        return "";
    }
};

}  // namespace sdc
