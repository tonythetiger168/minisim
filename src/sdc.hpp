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
// The file is Tcl, so the reader is a small Tcl: variables, [command]
// substitution, braces and quotes, expr, if / foreach / for / while, proc,
// list and string commands, source. Collections ([get_ports ...]) are passed
// between commands as handles, as the tools do.
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sdc {

struct Loc {
    std::string file;
    int line = 0;
    std::string str() const { return file + ":" + std::to_string(line); }
};

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

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

struct Diag {
    enum Sev { Warning, ErrorSev };
    Sev sev = Warning;
    // syntax, unknown-command, unknown-option, missing-value, bad-value,
    // no-match, no-clock, unsupported
    std::string code;
    Loc loc;
    std::string text;
};

// ------------------------------------------------------------ Tcl utilities

inline std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    while (i < n) {
        while (i < n && isspace((unsigned char)s[i])) i++;
        if (i >= n) break;
        std::string w;
        if (s[i] == '{') {
            int depth = 1;
            size_t j = i + 1;
            for (; j < n; j++) {
                if (s[j] == '\\' && j + 1 < n) { j++; continue; }
                if (s[j] == '{') depth++;
                else if (s[j] == '}' && --depth == 0) break;
            }
            w = s.substr(i + 1, j - i - 1);
            i = j < n ? j + 1 : n;
        } else if (s[i] == '"') {
            size_t j = i + 1;
            for (; j < n && s[j] != '"'; j++) {
                if (s[j] == '\\' && j + 1 < n) j++;
                w += s[j];
            }
            i = j < n ? j + 1 : n;
        } else {
            for (; i < n && !isspace((unsigned char)s[i]); i++) {
                if (s[i] == '\\' && i + 1 < n) i++;
                w += s[i];
            }
        }
        out.push_back(std::move(w));
    }
    return out;
}

inline std::string listElem(const std::string& e) {
    if (e.empty()) return "{}";
    if (e.find_first_of(" \t\n;\"$[]{}\\") == std::string::npos) return e;
    int depth = 0;
    bool balanced = true;
    for (char c : e) {
        if (c == '{') depth++;
        else if (c == '}' && --depth < 0) balanced = false;
    }
    if (balanced && depth == 0 && e.back() != '\\') return "{" + e + "}";
    std::string o;
    for (char c : e) {
        if (strchr(" \t\n;\"$[]{}\\", c)) o += '\\';
        o += c;
    }
    return o;
}

inline std::string joinList(const std::vector<std::string>& v) {
    std::string o;
    for (auto& e : v) {
        if (!o.empty()) o += ' ';
        o += listElem(e);
    }
    return o;
}

// Glob as the constraint tools read it: * and ?, brackets literal (a bus bit),
// and * stays inside one hierarchy level unless `cross`.
inline bool globMatch(const char* p, const char* s, bool cross, char sep = '/') {
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (*p == '*') p++;
            for (const char* t = s;; t++) {
                if (globMatch(p, t, cross, sep)) return true;
                if (!*t || (!cross && *t == sep)) return false;
            }
        }
        if (!*s) return false;
        if (*p == '?') {
            if (!cross && *s == sep) return false;
            continue;
        }
        if (*p == '\\' && p[1]) p++;
        if (*p != *s) return false;
    }
    return !*s;
}
inline bool hasWild(const std::string& p) { return p.find_first_of("*?") != std::string::npos; }

// A number, the way Tcl prints it.
inline std::string fmtNum(double d) {
    if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 1e15) {
        char b[40];
        snprintf(b, sizeof b, "%.1f", d);
        return b;
    }
    char b[40];
    snprintf(b, sizeof b, "%.12g", d);
    return b;
}

// ------------------------------------------------------------------- reader

class Reader {
public:
    const Design* design = nullptr;
    Constraints sdc;
    std::vector<Diag> diags;
    // Commands outside SDC that a tool also accepts, in the spec syntax below:
    // extra["set_dont_touch"] = "| objects=o:cn value=s?".
    std::map<std::string, std::string> extra;
    std::function<void(const std::string&)> onPuts;  // puts goes here, or nowhere
    // Commands of the host tool (report_timing, read_verilog, ...), run with
    // the substituted words; words[0] is the command name. They take part in
    // Tcl like any other command: the result is the string returned, and a
    // thrown sdc::Error is reported with the file and line of the call.
    using HostCommand = std::function<std::string(const std::vector<std::string>&)>;
    std::map<std::string, HostCommand> hostCommands;
    void addCommand(const std::string& name, HostCommand fn) { hostCommands[name] = std::move(fn); }
    // Called for a command nobody knows; return true when it was handled.
    std::function<bool(const std::vector<std::string>&, std::string& result)> onUnknown;
    // Where the command being run is, for a host command's own messages.
    Loc where() const { return Loc{file_, line_}; }

    bool ok() const {
        for (auto& d : diags)
            if (d.sev == Diag::ErrorSev) return false;
        return true;
    }

    void load(const std::string& path) {
        std::ifstream in(path);
        if (!in) throw Error("cannot open " + path);
        std::stringstream ss;
        ss << in.rdbuf();
        parse(ss.str(), path);
    }

    void parse(const std::string& text, const std::string& file = "<sdc>") {
        std::string saved = file_;
        file_ = file;
        if (frames_.empty()) frames_.emplace_back();
        try {
            eval(text, 1);
        } catch (const Ret&) {
        } catch (const Brk&) {
        } catch (const Cont&) {
        }
        file_ = saved;
    }

    // The objects behind a word: collection handles and bare names.
    ObjList objects(const std::string& word, const std::string& kinds = "kpicn") {
        return toObjs(word, kinds, Loc{file_, line_}, false);
    }

private:
    struct Ret { std::string v; };
    struct Brk {};
    struct Cont {};
    struct Proc { std::vector<std::pair<std::string, std::optional<std::string>>> params; std::string body; std::string file; int line; };

    std::string file_;
    int line_ = 0;  // of the command being run
    int depth_ = 0;
    std::vector<std::unordered_map<std::string, std::string>> frames_;  // [0] is global
    std::vector<std::set<std::string>> globals_;
    std::map<std::string, Proc> procs_;
    std::vector<ObjList> cols_;
    std::set<std::string> warnedFilter_;

    // ------------------------------------------------------------ diagnostics
    void diag(Diag::Sev sev, const char* code, const Loc& loc, std::string text) {
        diags.push_back({sev, code, loc, std::move(text)});
    }
    Loc here() const { return Loc{file_, line_}; }
    [[noreturn]] void fail(const std::string& m) const { throw Error(m); }

    // -------------------------------------------------------------- variables
    std::unordered_map<std::string, std::string>& scopeOf(std::string& name) {
        if (name.rfind("::", 0) == 0) { name.erase(0, 2); return frames_[0]; }
        if (frames_.size() > 1 && globals_.back().count(name)) return frames_[0];
        return frames_.back();
    }
    std::string getVar(std::string name) {
        if (name.rfind("env(", 0) == 0 || name.rfind("::env(", 0) == 0) {
            std::string k = name.substr(name.find('(') + 1);
            if (!k.empty()) k.pop_back();
            const char* v = getenv(k.c_str());
            if (!v) fail("no such environment variable \"" + k + "\"");
            return v;
        }
        auto& f = scopeOf(name);
        auto it = f.find(name);
        if (it == f.end()) fail("can't read \"" + name + "\": no such variable");
        return it->second;
    }
    void setVar(std::string name, std::string v) { scopeOf(name)[name] = std::move(v); }
    bool hasVar(std::string name) {
        auto& f = scopeOf(name);
        return f.count(name) != 0;
    }

    // ------------------------------------------------------------ the parser
    // Substitutes $var, [cmd] and backslashes in s[p..] until `stop` says so.
    void backslash(const std::string& s, size_t& p, int& line, std::string& out) {
        // s[p] == '\\'
        if (p + 1 >= s.size()) { out += '\\'; p++; return; }
        char c = s[p + 1];
        p += 2;
        switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case '\n':
                line++;
                while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
                out += ' ';
                break;
            case '\r':
                if (p < s.size() && s[p] == '\n') { p++; line++; }
                while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
                out += ' ';
                break;
            default: out += c;
        }
    }
    void dollar(const std::string& s, size_t& p, int& line, std::string& out) {
        // s[p] == '$'
        size_t q = p + 1;
        std::string name;
        if (q < s.size() && s[q] == '{') {
            size_t e = s.find('}', q);
            if (e == std::string::npos) fail("missing close-brace for variable name");
            name = s.substr(q + 1, e - q - 1);
            p = e + 1;
            out += getVar(name);
            return;
        }
        while (q < s.size() && (isalnum((unsigned char)s[q]) || s[q] == '_' || (s[q] == ':' && q + 1 < s.size() && s[q + 1] == ':'))) {
            if (s[q] == ':') q++;
            q++;
        }
        if (q == p + 1) { out += '$'; p++; return; }
        name = s.substr(p + 1, q - p - 1);
        if (q < s.size() && s[q] == '(') {  // array element: the index is substituted
            std::string idx;
            q++;
            while (q < s.size() && s[q] != ')') {
                if (s[q] == '$') dollar(s, q, line, idx);
                else if (s[q] == '[') bracket(s, q, line, idx);
                else idx += s[q++];
            }
            if (q >= s.size()) fail("missing )");
            q++;
            name += "(" + idx + ")";
        }
        p = q;
        out += getVar(name);
    }
    void bracket(const std::string& s, size_t& p, int& line, std::string& out) {
        // s[p] == '[': find the matching ], then evaluate what is inside.
        size_t q = p + 1;
        int depth = 1, brace = 0;
        int startLine = line;
        for (; q < s.size(); q++) {
            char c = s[q];
            if (c == '\\' && q + 1 < s.size()) { if (s[q + 1] == '\n') line++; q++; continue; }
            if (c == '\n') line++;
            if (c == '{') brace++;
            else if (c == '}') { if (brace) brace--; }
            else if (brace) continue;
            else if (c == '[') depth++;
            else if (c == ']' && --depth == 0) break;
        }
        if (q >= s.size()) fail("missing close-bracket");
        std::string inner = s.substr(p + 1, q - p - 1);
        p = q + 1;
        int savedLine = line_;
        out += eval(inner, startLine, /*nested*/ true);
        line_ = savedLine;
    }

    // One command's words from s at p. False at the end of the script.
    bool nextCommand(const std::string& s, size_t& p, int& line, std::vector<std::string>& words, int& cmdLine) {
        words.clear();
        size_t n = s.size();
        for (;;) {  // blank lines, separators and comments
            while (p < n && (isspace((unsigned char)s[p]) || s[p] == ';')) {
                if (s[p] == '\n') line++;
                p++;
            }
            if (p < n && s[p] == '#') {
                while (p < n && s[p] != '\n') {
                    if (s[p] == '\\' && p + 1 < n) { if (s[p + 1] == '\n') line++; p++; }
                    p++;
                }
                continue;
            }
            break;
        }
        if (p >= n) return false;
        cmdLine = line;
        line_ = line;
        while (p < n) {
            while (p < n && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) p++;
            if (p < n && s[p] == '\\' && p + 1 < n && (s[p + 1] == '\n' || s[p + 1] == '\r')) {
                std::string junk;
                backslash(s, p, line, junk);
                continue;
            }
            if (p >= n || s[p] == '\n' || s[p] == ';') {
                if (p < n) { if (s[p] == '\n') line++; p++; }
                break;
            }
            std::string w;
            bool expand = false;
            if (s.compare(p, 3, "{*}") == 0 && p + 3 < n && !isspace((unsigned char)s[p + 3])) {
                expand = true;
                p += 3;
            }
            if (s[p] == '{') {
                int depth = 1;
                size_t q = p + 1;
                for (; q < n; q++) {
                    if (s[q] == '\\' && q + 1 < n) {
                        if (s[q + 1] == '\n') {  // continuation inside braces is one space
                            line++;
                            w += ' ';
                            q++;
                            while (q + 1 < n && (s[q + 1] == ' ' || s[q + 1] == '\t')) q++;
                            continue;
                        }
                        w += s[q];
                        w += s[++q];
                        continue;
                    }
                    if (s[q] == '\n') line++;
                    if (s[q] == '{') depth++;
                    else if (s[q] == '}' && --depth == 0) break;
                    w += s[q];
                }
                if (q >= n) { p = n; fail("missing close-brace"); }
                p = q + 1;
                if (p < n && !isspace((unsigned char)s[p]) && s[p] != ';')
                    fail("extra characters after close-brace");
            } else if (s[p] == '"') {
                p++;
                for (;;) {
                    if (p >= n) fail("missing \"");
                    char c = s[p];
                    if (c == '"') { p++; break; }
                    if (c == '\\') backslash(s, p, line, w);
                    else if (c == '$') dollar(s, p, line, w);
                    else if (c == '[') bracket(s, p, line, w);
                    else { if (c == '\n') line++; w += c; p++; }
                }
            } else {
                while (p < n && !isspace((unsigned char)s[p]) && s[p] != ';') {
                    char c = s[p];
                    if (c == '\\') {
                        if (p + 1 < n && (s[p + 1] == '\n' || s[p + 1] == '\r')) break;
                        backslash(s, p, line, w);
                    } else if (c == '$') dollar(s, p, line, w);
                    else if (c == '[') bracket(s, p, line, w);
                    else { w += c; p++; }
                }
            }
            if (expand) {
                for (auto& e : splitList(w)) words.push_back(e);
            } else {
                words.push_back(std::move(w));
            }
        }
        return true;
    }

    // Skips to the end of the command at p after a parse error.
    void resync(const std::string& s, size_t& p, int& line) {
        int brace = 0;
        while (p < s.size()) {
            char c = s[p];
            if (c == '\\' && p + 1 < s.size()) { if (s[p + 1] == '\n') line++; p += 2; continue; }
            if (c == '{') brace++;
            else if (c == '}' && brace) brace--;
            else if (c == '\n') { line++; if (!brace) { p++; return; } }
            p++;
        }
    }

    std::string eval(const std::string& script, int baseLine, bool nested = false) {
        size_t p = 0;
        int line = baseLine, cmdLine = baseLine;
        std::vector<std::string> words;
        std::string result;
        if (++depth_ > 200) { depth_--; fail("too many nested evaluations"); }
        struct Pop { int& d; ~Pop() { d--; } } pop{depth_};
        for (;;) {
            try {
                if (!nextCommand(script, p, line, words, cmdLine)) break;
                if (words.empty()) continue;
                line_ = cmdLine;
                result = run(words);
            } catch (const Error& e) {
                // Inside [ ] the error belongs to the command around it.
                if (nested) throw;
                diag(Diag::ErrorSev, "syntax", Loc{file_, cmdLine}, e.what());
                if (p < script.size() && words.empty()) resync(script, p, line);
                result.clear();
            }
        }
        return result;
    }

    // ------------------------------------------------------------- expr
    struct Val {
        bool num = false, isInt = false;
        long long i = 0;
        double d = 0;
        std::string s;
        double f() const { return isInt ? (double)i : d; }
        bool truth() const {
            if (num) return isInt ? i != 0 : d != 0;
            return s == "true" || s == "yes" || s == "on";
        }
        std::string str() const {
            if (!num) return s;
            if (isInt) return std::to_string(i);
            return fmtNum(d);
        }
    };
    static Val vInt(long long i) { Val v; v.num = v.isInt = true; v.i = i; return v; }
    static Val vDbl(double d) { Val v; v.num = true; v.d = d; return v; }
    static Val vStr(const std::string& s) {
        Val v;
        v.s = s;
        if (s.empty()) return v;
        const char* b = s.c_str();
        char* e = nullptr;
        long long i = strtoll(b, &e, (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10);
        while (*e && isspace((unsigned char)*e)) e++;
        if (e != b && !*e) { v.num = v.isInt = true; v.i = i; return v; }
        double d = strtod(b, &e);
        while (*e && isspace((unsigned char)*e)) e++;
        if (e != b && !*e) { v.num = true; v.d = d; return v; }
        if (s == "true" || s == "false") return v;
        return v;
    }

    struct ExprP {
        Reader& r;
        const std::string& s;
        size_t p = 0;
        void ws() { while (p < s.size() && isspace((unsigned char)s[p])) p++; }
        bool eat(const char* t) {
            ws();
            size_t n = strlen(t);
            if (s.compare(p, n, t) != 0) return false;
            if (isalpha((unsigned char)t[0]) && p + n < s.size() && (isalnum((unsigned char)s[p + n]) || s[p + n] == '_')) return false;
            p += n;
            return true;
        }
        bool peek(const char* t) { size_t q = p; bool b = eat(t); p = q; return b; }
        Val need(Val v, const char* op) {
            if (!v.num) {
                if (v.s == "true") return vInt(1);
                if (v.s == "false") return vInt(0);
                r.fail(std::string("can't use non-numeric string \"") + v.s + "\" as operand of \"" + op + "\"");
            }
            return v;
        }
        Val primary() {
            ws();
            if (p >= s.size()) r.fail("missing operand in expression");
            char c = s[p];
            if (c == '(') {
                p++;
                Val v = ternary();
                if (!eat(")")) r.fail("missing ) in expression");
                return v;
            }
            if (c == '-') { p++; Val v = need(unary(), "-"); return v.isInt ? vInt(-v.i) : vDbl(-v.d); }
            if (c == '+') { p++; return need(unary(), "+"); }
            if (c == '!') { p++; return vInt(!unary().truth()); }
            if (c == '~') { p++; return vInt(~need(unary(), "~").i); }
            if (c == '$') { std::string o; int l = 0; r.dollar(s, p, l, o); return vStr(o); }
            if (c == '[') { std::string o; int l = r.line_; r.bracket(s, p, l, o); return vStr(o); }
            if (c == '"') {
                std::string o;
                int l = 0;
                p++;
                while (p < s.size() && s[p] != '"') {
                    if (s[p] == '\\') r.backslash(s, p, l, o);
                    else if (s[p] == '$') r.dollar(s, p, l, o);
                    else if (s[p] == '[') r.bracket(s, p, l, o);
                    else o += s[p++];
                }
                p++;
                Val v; v.s = o; return v;
            }
            if (c == '{') {
                size_t e = s.find('}', p);
                if (e == std::string::npos) r.fail("missing } in expression");
                Val v; v.s = s.substr(p + 1, e - p - 1); p = e + 1; return v;
            }
            if (isdigit((unsigned char)c) || c == '.') {
                const char* b = s.c_str() + p;
                char* e = nullptr;
                bool hex = c == '0' && p + 1 < s.size() && (s[p + 1] == 'x' || s[p + 1] == 'X');
                long long i = strtoll(b, &e, hex ? 16 : 10);
                if (!hex && (*e == '.' || *e == 'e' || *e == 'E')) {
                    double d = strtod(b, &e);
                    p = e - s.c_str();
                    return vDbl(d);
                }
                p = e - s.c_str();
                return vInt(i);
            }
            if (isalpha((unsigned char)c)) {
                size_t q = p;
                while (q < s.size() && (isalnum((unsigned char)s[q]) || s[q] == '_')) q++;
                std::string id = s.substr(p, q - p);
                p = q;
                ws();
                if (p < s.size() && s[p] == '(') {
                    p++;
                    std::vector<Val> a;
                    if (!eat(")")) {
                        do a.push_back(ternary()); while (eat(","));
                        if (!eat(")")) r.fail("missing ) after function arguments");
                    }
                    return call(id, a);
                }
                Val v; v.s = id; return v;  // true, false, a bare word
            }
            r.fail(std::string("unexpected '") + c + "' in expression");
        }
        Val call(const std::string& f, std::vector<Val>& a) {
            auto x = [&](size_t i) { if (i >= a.size()) r.fail("too few arguments for " + f + "()"); return need(a[i], f.c_str()).f(); };
            if (f == "abs") { Val v = need(a.at(0), "abs"); return v.isInt ? vInt(std::llabs(v.i)) : vDbl(std::fabs(v.d)); }
            if (f == "min" || f == "max") {
                if (a.empty()) r.fail("too few arguments for " + f + "()");
                Val best = need(a[0], f.c_str());
                for (auto& v : a) { Val n = need(v, f.c_str()); if (f == "min" ? n.f() < best.f() : n.f() > best.f()) best = n; }
                return best;
            }
            if (f == "int" || f == "wide" || f == "entier") return vInt((long long)x(0));
            if (f == "round") return vInt(std::llround(x(0)));
            if (f == "double") return vDbl(x(0));
            if (f == "ceil") return vDbl(std::ceil(x(0)));
            if (f == "floor") return vDbl(std::floor(x(0)));
            if (f == "sqrt") return vDbl(std::sqrt(x(0)));
            if (f == "pow") return vDbl(std::pow(x(0), x(1)));
            if (f == "fmod") return vDbl(std::fmod(x(0), x(1)));
            if (f == "exp") return vDbl(std::exp(x(0)));
            if (f == "log") return vDbl(std::log(x(0)));
            if (f == "log10") return vDbl(std::log10(x(0)));
            r.fail("unknown math function \"" + f + "\"");
        }
        Val unary() { return primary(); }
        Val power() {
            Val a = unary();
            if (eat("**")) {
                Val b = need(power(), "**");
                a = need(a, "**");
                if (a.isInt && b.isInt && b.i >= 0) {
                    long long o = 1;
                    for (long long k = 0; k < b.i; k++) o *= a.i;
                    return vInt(o);
                }
                return vDbl(std::pow(a.f(), b.f()));
            }
            return a;
        }
        Val mul() {
            Val a = power();
            for (;;) {
                ws();
                if (peek("**")) return a;
                char op = p < s.size() ? s[p] : 0;
                if (op != '*' && op != '/' && op != '%') return a;
                p++;
                const char o[2] = {op, 0};
                Val b = need(power(), o);
                a = need(a, o);
                if (a.isInt && b.isInt) {
                    if (op == '*') a = vInt(a.i * b.i);
                    else {
                        if (b.i == 0) r.fail("divide by zero");
                        long long q = a.i / b.i, m = a.i % b.i;
                        if (m != 0 && ((m < 0) != (b.i < 0))) { q--; m += b.i; }  // Tcl floors
                        a = vInt(op == '/' ? q : m);
                    }
                } else if (op == '*') a = vDbl(a.f() * b.f());
                else if (op == '/') { if (b.f() == 0) r.fail("divide by zero"); a = vDbl(a.f() / b.f()); }
                else r.fail("can't use floating-point value as operand of \"%\"");
            }
        }
        Val add() {
            Val a = mul();
            for (;;) {
                ws();
                char op = p < s.size() ? s[p] : 0;
                if (op != '+' && op != '-') return a;
                p++;
                const char o[2] = {op, 0};
                Val b = need(mul(), o);
                a = need(a, o);
                if (a.isInt && b.isInt) a = vInt(op == '+' ? a.i + b.i : a.i - b.i);
                else a = vDbl(op == '+' ? a.f() + b.f() : a.f() - b.f());
            }
        }
        Val shift() {
            Val a = add();
            for (;;) {
                if (eat("<<")) a = vInt(need(a, "<<").i << need(add(), "<<").i);
                else if (eat(">>")) a = vInt(need(a, ">>").i >> need(add(), ">>").i);
                else return a;
            }
        }
        static int cmp(const Val& a, const Val& b) {
            if (a.num && b.num) return a.f() < b.f() ? -1 : a.f() > b.f() ? 1 : 0;
            return a.str().compare(b.str());
        }
        Val rel() {
            Val a = shift();
            for (;;) {
                if (eat("<=")) a = vInt(cmp(a, shift()) <= 0);
                else if (eat(">=")) a = vInt(cmp(a, shift()) >= 0);
                else if (!peek("<<") && eat("<")) a = vInt(cmp(a, shift()) < 0);
                else if (!peek(">>") && eat(">")) a = vInt(cmp(a, shift()) > 0);
                else return a;
            }
        }
        Val eq() {
            Val a = rel();
            for (;;) {
                if (eat("==")) a = vInt(cmp(a, rel()) == 0);
                else if (eat("!=")) a = vInt(cmp(a, rel()) != 0);
                else if (eat("eq")) a = vInt(a.str() == rel().str());
                else if (eat("ne")) a = vInt(a.str() != rel().str());
                else return a;
            }
        }
        Val band() {
            Val a = eq();
            while (!peek("&&") && eat("&")) a = vInt(need(a, "&").i & need(eq(), "&").i);
            return a;
        }
        Val bxor() {
            Val a = band();
            while (eat("^")) a = vInt(need(a, "^").i ^ need(band(), "^").i);
            return a;
        }
        Val bor() {
            Val a = bxor();
            while (!peek("||") && eat("|")) a = vInt(need(a, "|").i | need(bxor(), "|").i);
            return a;
        }
        Val land() {
            Val a = bor();
            while (eat("&&")) { Val b = bor(); a = vInt(a.truth() && b.truth()); }
            return a;
        }
        Val lor() {
            Val a = land();
            while (eat("||")) { Val b = land(); a = vInt(a.truth() || b.truth()); }
            return a;
        }
        Val ternary() {
            Val c = lor();
            if (eat("?")) {
                Val a = ternary();
                if (!eat(":")) r.fail("missing : in expression");
                Val b = ternary();
                return c.truth() ? a : b;
            }
            return c;
        }
    };
    Val expr(const std::string& e) {
        ExprP p{*this, e};
        Val v = p.ternary();
        p.ws();
        if (p.p < e.size()) fail("syntax error in expression \"" + e + "\"");
        return v;
    }

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

    std::map<Kind, std::unordered_set<std::string>> exact_;
    bool exists(Kind k, const std::string& n) {
        auto it = exact_.find(k);
        if (it == exact_.end()) {
            auto& set = exact_[k];
            for (auto& s : design->names(k)) set.insert(s);
            it = exact_.find(k);
        }
        return it->second.count(n) != 0;
    }

    struct Query {
        bool hier = false, regexp = false, nocase = false;
    };
    // Design objects of a kind matching one pattern.
    void findIn(Kind k, const std::string& pat, const Query& q, ObjList& out) {
        if (k == Kind::Clock) {
            std::set<std::string> seen;
            for (auto& c : sdc.clocks)
                if (matchName(pat, c.name, q) && seen.insert(c.name).second) out.push_back({Kind::Clock, c.name, true});
            return;
        }
        if (k == Kind::LibCell || !design) return;
        if (!q.hier && !q.regexp && !q.nocase && !hasWild(pat) && pat.find('\\') == std::string::npos) {
            if (exists(k, pat)) { out.push_back({k, pat, true}); return; }
            if (pat.find('[') != std::string::npos) return;
        }
        for (auto& n : design->names(k))
            if (matchName(pat, n, q)) out.push_back({k, n, true});
    }
    bool matchName(const std::string& pat, const std::string& name, const Query& q) {
        if (q.regexp) {
            try {
                std::regex re(pat, q.nocase ? std::regex::icase | std::regex::ECMAScript : std::regex::ECMAScript);
                return std::regex_match(name, re);
            } catch (const std::regex_error&) {
                fail("bad regular expression \"" + pat + "\"");
            }
        }
        std::string p = pat, n = name;
        if (q.nocase) {
            for (char& c : p) c = (char)tolower((unsigned char)c);
            for (char& c : n) c = (char)tolower((unsigned char)c);
        }
        char sep = sdc.hierSep.empty() ? '/' : sdc.hierSep[0];
        if (sep != '/')
            for (char& c : p) if (c == sep) c = '/';
        auto one = [&](const char* s) {
            if (globMatch(p.c_str(), s, false)) return true;
            // "d" names every bit of the bus d[7:0].
            if (p.find('[') == std::string::npos) {
                const char* b = strrchr(s, '[');
                if (b && s[strlen(s) - 1] == ']' && !strchr(b, '/')) {
                    std::string base(s, b - s);
                    return globMatch(p.c_str(), base.c_str(), false);
                }
            }
            return false;
        };
        if (one(n.c_str())) return true;
        if (q.hier)  // the pattern names the last levels, at any depth
            for (size_t i = 0; i < n.size(); i++)
                if (n[i] == '/' && one(n.c_str() + i + 1)) return true;
        return false;
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

    // -filter {a == b && c =~ d*}
    bool filterOne(const Obj& o, const std::string& expr) {
        // split on || then &&
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
        for (auto& orTerm : splitOn(expr, "||")) {
            bool all = true;
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
                std::string attr = trim(which < 0 ? t : t.substr(0, at));
                std::string want = which < 0 ? "true" : trim(t.substr(at + 2));
                if (which < 0) which = 0;
                std::optional<std::string> have;
                if (attr == "name" || attr == "full_name") {
                    have = o.name;
                    if (attr == "name") {
                        size_t sl = o.name.rfind('/');
                        if (sl != std::string::npos) have = o.name.substr(sl + 1);
                    }
                } else if (attr == "object_class") have = kindName(o.kind);
                else if (attr == "direction" && o.kind == Kind::Port && design) {
                    int d = design->portDir(o.name);
                    if (d >= 0) have = d == 0 ? "in" : d == 1 ? "out" : "inout";
                }
                if (!have && design) have = design->attribute(o, attr);
                if (!have) {
                    if (warnedFilter_.insert(attr).second)
                        diag(Diag::Warning, "unsupported", here(), "-filter: the attribute '" + attr + "' is not known for this design; the filter keeps every object");
                    continue;
                }
                bool m = which >= 2 ? globMatch(want.c_str(), have->c_str(), true) : *have == want;
                if (which == 1 || which == 3) m = !m;
                if (!m) { all = false; break; }
            }
            if (all) return true;
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
            std::set<Obj> seen;
            for (auto& p : pats) {
                if (const ObjList* c = colOf(p)) {  // get_ports [get_ports x]
                    for (auto& o : *c)
                        if (o.kind == k && seen.insert(o).second) out.push_back(o);
                    continue;
                }
                ObjList found;
                findIn(k, p, q, found);
                if (found.empty()) {
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
        if (!filter.empty()) {
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

    // ------------------------------------------------------------ commands
    static void wantArgs(const std::vector<std::string>& w, size_t lo, size_t hi, const char* usage) {
        if (w.size() - 1 < lo || w.size() - 1 > hi) throw Error("wrong # args: should be \"" + w[0] + " " + usage + "\"");
    }

    std::string run(const std::vector<std::string>& w) {
        const std::string& c = w[0];
        if (auto h = hostCommands.find(c); h != hostCommands.end()) return h->second(w);
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
        // ---- Tcl
        if (c == "set") {
            wantArgs(w, 1, 2, "varName ?newValue?");
            if (w.size() == 3) setVar(w[1], w[2]);
            return getVar(w[1]);
        }
        if (c == "unset") {
            for (size_t i = 1; i < w.size(); i++) {
                if (w[i] == "-nocomplain" || w[i] == "--") continue;
                std::string n = w[i];
                scopeOf(n).erase(n);
            }
            return "";
        }
        if (c == "global") {
            if (frames_.size() > 1)
                for (size_t i = 1; i < w.size(); i++) globals_.back().insert(w[i]);
            return "";
        }
        if (c == "expr") {
            std::string e;
            for (size_t i = 1; i < w.size(); i++) e += (i > 1 ? " " : "") + w[i];
            return expr(e).str();
        }
        if (c == "incr") {
            wantArgs(w, 1, 2, "varName ?increment?");
            long long v = hasVar(w[1]) ? strtoll(getVar(w[1]).c_str(), nullptr, 10) : 0;
            v += w.size() > 2 ? strtoll(w[2].c_str(), nullptr, 10) : 1;
            setVar(w[1], std::to_string(v));
            return std::to_string(v);
        }
        if (c == "append") {
            if (w.size() < 2) fail("wrong # args: should be \"append varName ?value ...?\"");
            std::string v = hasVar(w[1]) ? getVar(w[1]) : "";
            for (size_t i = 2; i < w.size(); i++) v += w[i];
            setVar(w[1], v);
            return v;
        }
        if (c == "if") {
            size_t i = 1;
            int bl = line_;
            for (;;) {
                if (i >= w.size()) fail("wrong # args: no expression after \"if\"");
                bool t = expr(w[i++]).truth();
                if (i < w.size() && w[i] == "then") i++;
                if (i >= w.size()) fail("wrong # args: no script following the condition");
                if (t) return eval(w[i], bl);
                i++;
                if (i >= w.size()) return "";
                if (w[i] == "elseif") { i++; continue; }
                if (w[i] == "else") i++;
                if (i >= w.size()) fail("wrong # args: no script following \"else\"");
                return eval(w[i], bl);
            }
        }
        if (c == "foreach") {
            if (w.size() < 4 || w.size() % 2 != 0) fail("wrong # args: should be \"foreach varList list ?varList list ...? command\"");
            int bl = line_;
            std::vector<std::vector<std::string>> vars, lists;
            size_t rounds = 0;
            for (size_t i = 1; i + 1 < w.size(); i += 2) {
                vars.push_back(splitList(w[i]));
                lists.push_back(splitList(w[i + 1]));
                if (vars.back().empty()) fail("foreach varlist is empty");
                rounds = std::max(rounds, (lists.back().size() + vars.back().size() - 1) / vars.back().size());
            }
            for (size_t r = 0; r < rounds; r++) {
                for (size_t k = 0; k < vars.size(); k++)
                    for (size_t v = 0; v < vars[k].size(); v++) {
                        size_t idx = r * vars[k].size() + v;
                        setVar(vars[k][v], idx < lists[k].size() ? lists[k][idx] : "");
                    }
                try { eval(w.back(), bl); } catch (const Brk&) { break; } catch (const Cont&) {}
            }
            return "";
        }
        if (c == "for") {
            wantArgs(w, 4, 4, "start test next command");
            int bl = line_;
            eval(w[1], bl);
            for (int guard = 0; expr(w[2]).truth(); guard++) {
                if (guard > 10000000) fail("for: too many iterations");
                try { eval(w[4], bl); } catch (const Brk&) { break; } catch (const Cont&) {}
                eval(w[3], bl);
            }
            return "";
        }
        if (c == "while") {
            wantArgs(w, 2, 2, "test command");
            int bl = line_;
            for (int guard = 0; expr(w[1]).truth(); guard++) {
                if (guard > 10000000) fail("while: too many iterations");
                try { eval(w[2], bl); } catch (const Brk&) { break; } catch (const Cont&) {}
            }
            return "";
        }
        if (c == "break") throw Brk{};
        if (c == "continue") throw Cont{};
        if (c == "return") throw Ret{w.size() > 1 ? w.back() : ""};
        if (c == "proc") {
            wantArgs(w, 3, 3, "name args body");
            Proc p;
            for (auto& a : splitList(w[2])) {
                auto parts = splitList(a);
                if (parts.empty()) continue;
                p.params.push_back({parts[0], parts.size() > 1 ? std::optional<std::string>(parts[1]) : std::nullopt});
            }
            p.body = w[3];
            p.file = file_;
            p.line = line_;
            procs_[w[1]] = std::move(p);
            return "";
        }
        if (auto it = procs_.find(c); it != procs_.end()) {
            const Proc p = it->second;
            std::unordered_map<std::string, std::string> f;
            size_t ai = 1;
            for (size_t i = 0; i < p.params.size(); i++) {
                if (p.params[i].first == "args" && i + 1 == p.params.size()) {
                    f["args"] = joinList(std::vector<std::string>(w.begin() + std::min(ai, w.size()), w.end()));
                    ai = w.size();
                } else if (ai < w.size()) f[p.params[i].first] = w[ai++];
                else if (p.params[i].second) f[p.params[i].first] = *p.params[i].second;
                else fail("wrong # args: should be \"" + c + " ...\"");
            }
            if (ai < w.size()) fail("wrong # args: too many arguments to \"" + c + "\"");
            frames_.push_back(std::move(f));
            globals_.emplace_back();
            int savedLine = line_;
            struct Pop { Reader& r; int l; ~Pop() { r.frames_.pop_back(); r.globals_.pop_back(); r.line_ = l; } } pop{*this, savedLine};
            try {
                return eval(p.body, p.line);
            } catch (const Ret& r) {
                return r.v;
            }
        }
        if (c == "list") return joinList(std::vector<std::string>(w.begin() + 1, w.end()));
        if (c == "concat") {
            std::vector<std::string> all;
            for (size_t i = 1; i < w.size(); i++)
                for (auto& e : splitList(w[i])) all.push_back(e);
            return joinList(all);
        }
        if (c == "llength") { wantArgs(w, 1, 1, "list"); return std::to_string(splitList(w[1]).size()); }
        if (c == "lindex") {
            wantArgs(w, 1, 2, "list ?index?");
            if (w.size() == 2) return w[1];
            auto l = splitList(w[1]);
            long i = w[2] == "end" ? (long)l.size() - 1 : w[2].rfind("end-", 0) == 0 ? (long)l.size() - 1 - strtol(w[2].c_str() + 4, nullptr, 10) : strtol(w[2].c_str(), nullptr, 10);
            return i >= 0 && (size_t)i < l.size() ? l[i] : "";
        }
        if (c == "lrange") {
            wantArgs(w, 3, 3, "list first last");
            auto l = splitList(w[1]);
            auto idx = [&](const std::string& s) { return s == "end" ? (long)l.size() - 1 : s.rfind("end-", 0) == 0 ? (long)l.size() - 1 - strtol(s.c_str() + 4, nullptr, 10) : strtol(s.c_str(), nullptr, 10); };
            long a = std::max(0L, idx(w[2])), b = std::min((long)l.size() - 1, idx(w[3]));
            std::vector<std::string> o;
            for (long i = a; i <= b; i++) o.push_back(l[i]);
            return joinList(o);
        }
        if (c == "lappend") {
            if (w.size() < 2) fail("wrong # args: should be \"lappend varName ?value ...?\"");
            std::string v = hasVar(w[1]) ? getVar(w[1]) : "";
            for (size_t i = 2; i < w.size(); i++) v += (v.empty() ? "" : " ") + listElem(w[i]);
            setVar(w[1], v);
            return v;
        }
        if (c == "lsort") {
            if (w.size() < 2) fail("wrong # args: should be \"lsort ?options? list\"");
            auto l = splitList(w.back());
            bool numeric = false, dec = false, uniq = false;
            for (size_t i = 1; i + 1 < w.size(); i++) {
                if (w[i] == "-integer" || w[i] == "-real") numeric = true;
                else if (w[i] == "-decreasing") dec = true;
                else if (w[i] == "-unique") uniq = true;
            }
            std::stable_sort(l.begin(), l.end(), [&](const std::string& a, const std::string& b) {
                return numeric ? strtod(a.c_str(), nullptr) < strtod(b.c_str(), nullptr) : a < b;
            });
            if (uniq) l.erase(std::unique(l.begin(), l.end()), l.end());
            if (dec) std::reverse(l.begin(), l.end());
            return joinList(l);
        }
        if (c == "lsearch") {
            if (w.size() < 3) fail("wrong # args: should be \"lsearch ?options? list pattern\"");
            auto l = splitList(w[w.size() - 2]);
            bool exact = false;
            for (size_t i = 1; i + 2 < w.size(); i++) if (w[i] == "-exact") exact = true;
            for (size_t i = 0; i < l.size(); i++)
                if (exact ? l[i] == w.back() : globMatch(w.back().c_str(), l[i].c_str(), true)) return std::to_string(i);
            return "-1";
        }
        if (c == "join") {
            wantArgs(w, 1, 2, "list ?joinString?");
            std::string sep = w.size() > 2 ? w[2] : " ", o;
            auto l = splitList(w[1]);
            for (size_t i = 0; i < l.size(); i++) o += (i ? sep : "") + l[i];
            return o;
        }
        if (c == "split") {
            wantArgs(w, 1, 2, "string ?splitChars?");
            std::string chars = w.size() > 2 ? w[2] : " \t\n";
            std::vector<std::string> o;
            std::string cur;
            for (char ch : w[1]) {
                if (chars.find(ch) != std::string::npos) { o.push_back(cur); cur.clear(); }
                else cur += ch;
            }
            o.push_back(cur);
            return joinList(o);
        }
        if (c == "string") return cmdString(w);
        if (c == "format") return cmdFormat(w);
        if (c == "puts" || c == "echo") {
            if (onPuts) {
                std::string o;
                for (size_t i = 1; i < w.size(); i++) {
                    if (c == "puts" && (w[i] == "-nonewline" || (i + 1 < w.size() && (w[i] == "stdout" || w[i] == "stderr")))) continue;
                    o += (o.empty() ? "" : " ") + w[i];
                }
                onPuts(o);
            }
            return "";
        }
        if (c == "info") {
            if (w.size() >= 3 && w[1] == "exists") return hasVar(w[2]) ? "1" : "0";
            if (w.size() >= 2 && w[1] == "script") return file_;
            fail("info " + (w.size() > 1 ? w[1] : std::string()) + ": not supported");
        }
        if (c == "catch") {
            wantArgs(w, 1, 2, "script ?resultVarName?");
            size_t before = diags.size();
            std::string res;
            bool err = false;
            try {
                res = eval(w[1], line_, /*nested*/ true);
            } catch (const Error& e) {
                err = true;
                res = e.what();
            }
            for (size_t i = before; i < diags.size(); i++)
                if (diags[i].sev == Diag::ErrorSev) { err = true; res = diags[i].text; }
            if (err) diags.resize(before);
            if (w.size() > 2) setVar(w[2], res);
            return err ? "1" : "0";
        }
        if (c == "error") fail(w.size() > 1 ? w[1] : "error");
        if (c == "source") {
            std::string path;
            for (size_t i = 1; i < w.size(); i++)
                if (w[i].empty() || w[i][0] != '-') path = w[i];
            if (path.empty()) fail("wrong # args: should be \"source fileName\"");
            if (path[0] != '/') {
                size_t sl = file_.rfind('/');
                if (sl != std::string::npos) {
                    std::string rel = file_.substr(0, sl + 1) + path;
                    if (std::ifstream(rel)) path = rel;
                }
            }
            std::ifstream in(path);
            if (!in) fail("couldn't read file \"" + path + "\"");
            std::stringstream ss;
            ss << in.rdbuf();
            std::string savedFile = file_;
            int savedLine = line_;
            file_ = path;
            struct Back { Reader& r; std::string f; int l; ~Back() { r.file_ = f; r.line_ = l; } } back{*this, savedFile, savedLine};
            try { eval(ss.str(), 1); } catch (const Ret&) {}
            return "";
        }
        if (c == "namespace" || c == "package" || c == "suppress_message" || c == "unsuppress_message" ||
            c == "set_message_info" || c == "redirect" || c == "date" || c == "pid") return "";
        if (onUnknown) {
            std::string res;
            if (onUnknown(w, res)) return res;
        }
        diag(Diag::ErrorSev, "unknown-command", here(), "unknown command '" + c + "'");
        return "";
    }

    std::string cmdString(const std::vector<std::string>& w) {
        if (w.size() < 3) fail("wrong # args: should be \"string subcommand arg ...\"");
        const std::string& s = w[1];
        if (s == "length") return std::to_string(w[2].size());
        if (s == "tolower" || s == "toupper") {
            std::string o = w[2];
            for (char& ch : o) ch = (char)(s == "tolower" ? tolower((unsigned char)ch) : toupper((unsigned char)ch));
            return o;
        }
        if (s == "trim" || s == "trimleft" || s == "trimright") {
            std::string chars = w.size() > 3 ? w[3] : " \t\n\r";
            size_t a = s == "trimright" ? 0 : w[2].find_first_not_of(chars);
            if (a == std::string::npos) return "";
            size_t b = s == "trimleft" ? w[2].size() - 1 : w[2].find_last_not_of(chars);
            if (b == std::string::npos) return "";
            return w[2].substr(a, b - a + 1);
        }
        if (s == "equal" || s == "compare" || s == "match") {
            bool nocase = false;
            size_t i = 2;
            while (i < w.size() && w[i][0] == '-' && w.size() - i > 2) { if (w[i] == "-nocase") nocase = true; i++; }
            if (w.size() - i != 2) fail("wrong # args for \"string " + s + "\"");
            std::string a = w[i], b = w[i + 1];
            if (nocase) {
                for (char& ch : a) ch = (char)tolower((unsigned char)ch);
                for (char& ch : b) ch = (char)tolower((unsigned char)ch);
            }
            if (s == "equal") return a == b ? "1" : "0";
            if (s == "compare") return std::to_string(a.compare(b) < 0 ? -1 : a.compare(b) > 0 ? 1 : 0);
            return globMatch(a.c_str(), b.c_str(), true) ? "1" : "0";
        }
        if (s == "range" || s == "index") {
            auto idx = [&](const std::string& t) { return t == "end" ? (long)w[2].size() - 1 : t.rfind("end-", 0) == 0 ? (long)w[2].size() - 1 - strtol(t.c_str() + 4, nullptr, 10) : strtol(t.c_str(), nullptr, 10); };
            if (w.size() < (s == "range" ? 5u : 4u)) fail("wrong # args for \"string " + s + "\"");
            long a = std::max(0L, idx(w[3])), b = s == "range" ? std::min((long)w[2].size() - 1, idx(w[4])) : a;
            return a <= b && (size_t)a < w[2].size() ? w[2].substr(a, b - a + 1) : "";
        }
        if (s == "first" || s == "last") {
            if (w.size() < 4) fail("wrong # args for \"string " + s + "\"");
            size_t p = s == "first" ? w[3].find(w[2]) : w[3].rfind(w[2]);
            return p == std::string::npos ? "-1" : std::to_string(p);
        }
        if (s == "map") {
            if (w.size() < 4) fail("wrong # args for \"string map\"");
            auto m = splitList(w[w.size() - 2]);
            const std::string& in = w.back();
            std::string o;
            for (size_t i = 0; i < in.size();) {
                bool hit = false;
                for (size_t k = 0; k + 1 < m.size(); k += 2)
                    if (!m[k].empty() && in.compare(i, m[k].size(), m[k]) == 0) { o += m[k + 1]; i += m[k].size(); hit = true; break; }
                if (!hit) o += in[i++];
            }
            return o;
        }
        fail("string " + s + ": not supported");
    }

    std::string cmdFormat(const std::vector<std::string>& w) {
        if (w.size() < 2) fail("wrong # args: should be \"format formatString ?arg ...?\"");
        std::string o;
        size_t ai = 2;
        const std::string& f = w[1];
        for (size_t i = 0; i < f.size(); i++) {
            if (f[i] != '%') { o += f[i]; continue; }
            if (i + 1 < f.size() && f[i + 1] == '%') { o += '%'; i++; continue; }
            size_t j = i + 1;
            while (j < f.size() && strchr("-+ #0123456789.", f[j])) j++;
            while (j < f.size() && (f[j] == 'l' || f[j] == 'h')) j++;
            if (j >= f.size()) fail("format string ended in the middle of a field");
            if (ai >= w.size()) fail("not enough arguments for all format specifiers");
            std::string spec = f.substr(i, j - i);
            spec.erase(std::remove_if(spec.begin(), spec.end(), [](char ch) { return ch == 'l' || ch == 'h'; }), spec.end());
            char conv = f[j], buf[512];
            const std::string& a = w[ai++];
            if (conv == 'd' || conv == 'i' || conv == 'x' || conv == 'X' || conv == 'o' || conv == 'u' || conv == 'c')
                snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (long long)strtod(a.c_str(), nullptr));
            else if (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G')
                snprintf(buf, sizeof buf, (spec + conv).c_str(), strtod(a.c_str(), nullptr));
            else if (conv == 's')
                snprintf(buf, sizeof buf, (spec + conv).c_str(), a.c_str());
            else fail(std::string("bad field specifier \"") + conv + "\"");
            o += buf;
            i = j;
        }
        return o;
    }
};

}  // namespace sdc
