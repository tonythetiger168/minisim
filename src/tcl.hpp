#pragma once
// tcl.hpp - the Tcl interpreter of the tool line.
//
// The EDA file formats that are Tcl (SDC, UPF) and the shells of the tools
// (timing, synthesis, place and route) all run on this one interpreter. It is
// a subset: variables and arrays, [command] substitution, braces and quotes,
// expr, if / foreach / for / while, proc, upvar / uplevel, namespace eval
// with namespace variables and procs, list and string commands, regexp /
// regsub (ECMAScript syntax), format, catch, source. There is no file I/O.
//
//   tcl::Interp t;
//   t.addCommand("report_timing", [&](const std::vector<std::string>& w) { ...; return std::string(); });
//   t.load("run.tcl");                     // or t.parse(text, "name")
//   t.shell(std::cin, std::cout, "msta> "); // an interactive prompt
//   for (auto& d : t.diags) ...             // errors, each with file and line
//
// An error never stops the read: the command is reported in `diags` and the
// next one runs, so a tool can show every problem of a file at once. A format
// reader derives from Interp and overrides command() (sdc::Reader does).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace tcl {

struct Loc {
    std::string file;
    int line = 0;
    std::string str() const { return file + ":" + std::to_string(line); }
};

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Diag {
    enum Sev { Warning, ErrorSev };
    Sev sev = Warning;
    // The interpreter's own codes are "syntax" and "unknown-command"; a
    // reader built on it adds its own.
    std::string code;
    Loc loc;
    std::string text;
};

// ---------------------------------------------------------------- utilities

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

// -------------------------------------------------------------- interpreter

class Interp {
public:
    virtual ~Interp() = default;
    std::vector<Diag> diags;
    std::function<void(const std::string&)> onPuts;  // puts goes here, or nowhere
    // Commands of the host tool (report_timing, read_verilog, ...), run with
    // the substituted words; words[0] is the command name. They take part in
    // Tcl like any other command: the result is the string returned, and a
    // thrown tcl::Error is reported with the file and line of the call.
    using HostCommand = std::function<std::string(const std::vector<std::string>&)>;
    std::map<std::string, HostCommand> hostCommands;
    void addCommand(const std::string& name, HostCommand fn) { hostCommands[name] = std::move(fn); }
    // Called for a command nobody knows; return true when it was handled.
    std::function<bool(const std::vector<std::string>&, std::string& result)> onUnknown;
    // Where the command being run is, for a host command's own messages.
    Loc where() const { return Loc{file_, line_}; }
    // When set, [3], [7:0] and [*] are kept as text (a bus index in a name)
    // instead of being run as a command. The netlist formats turn this on.
    bool busBrackets = false;
    // Set by the exit / quit command; parse() and shell() stop there.
    bool exited = false;
    int exitCode = 0;

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

    // Runs a script; returns the result of its last command.
    std::string parse(const std::string& text, const std::string& file = "<tcl>", int firstLine = 1) {
        std::string saved = file_, result;
        file_ = file;
        if (frames_.empty()) frames_.emplace_back();
        try {
            result = eval(text, firstLine);
        } catch (const Ret& r) {
            result = r.v;
        } catch (const Brk&) {
        } catch (const Cont&) {
        } catch (const Exit& e) {
            exited = true;
            exitCode = e.code;
        }
        file_ = saved;
        return result;
    }

    // Global variables, for the host.
    void set(const std::string& name, const std::string& value) {
        if (frames_.empty()) frames_.emplace_back();
        frames_[0][name] = value;
    }
    std::optional<std::string> get(const std::string& name) const {
        if (frames_.empty()) return std::nullopt;
        auto it = frames_[0].find(name);
        return it == frames_[0].end() ? std::nullopt : std::optional<std::string>(it->second);
    }

    // True when `text` is whole commands: no open brace, bracket or quote and
    // no trailing backslash. A prompt reads lines until this holds.
    static bool complete(const std::string& text) {
        int brace = 0, bracket = 0;
        bool quote = false, wordStart = true;
        for (size_t i = 0; i < text.size(); i++) {
            char ch = text[i];
            if (ch == '\\') {
                if (i + 1 >= text.size() || (text[i + 1] == '\n' && i + 2 >= text.size())) return false;
                i++;
                wordStart = false;
                continue;
            }
            if (quote) { if (ch == '"') quote = false; continue; }
            if (ch == '{') brace++;
            else if (ch == '}') { if (brace) brace--; }
            else if (brace) {}
            else if (ch == '[') bracket++;
            else if (ch == ']') { if (bracket) bracket--; }
            else if (ch == '"' && wordStart) quote = true;
            wordStart = isspace((unsigned char)ch) || ch == '[' || ch == ';';
        }
        return !brace && !bracket && !quote;
    }

    // A prompt: reads commands from `in`, prints each result and each error
    // to `out`. Returns the exit code (exit N), or 0 at end of input.
    int shell(std::istream& in, std::ostream& out, const std::string& prompt = "% ", const std::string& name = "<stdin>") {
        std::string buf, line;
        int lineNo = 0, start = 1;
        for (;;) {
            if (!prompt.empty()) out << (buf.empty() ? prompt : std::string(prompt.size(), ' ')) << std::flush;
            if (!std::getline(in, line)) break;
            lineNo++;
            buf += line + "\n";
            if (!complete(buf)) continue;
            size_t before = diags.size();
            std::string res = parse(buf, name, start);
            for (size_t i = before; i < diags.size(); i++)
                out << (diags[i].sev == Diag::ErrorSev ? "Error: " : "Warning: ") << diags[i].text << " (" << diags[i].loc.str() << ")\n";
            if (!res.empty()) out << res << "\n";
            buf.clear();
            start = lineNo + 1;
            if (exited) return exitCode;
        }
        if (!buf.empty()) {
            size_t before = diags.size();
            parse(buf, name, start);
            for (size_t i = before; i < diags.size(); i++) out << "Error: " << diags[i].text << " (" << diags[i].loc.str() << ")\n";
        }
        return exited ? exitCode : 0;
    }

protected:
    // The commands of a reader built on the interpreter (SDC, ...). Return
    // true with `result` set when `w` was one of them. Host commands come
    // first, then these, then Tcl's own.
    virtual bool command(const std::vector<std::string>& /*w*/, std::string& /*result*/) { return false; }

    struct Exit { int code; };
    struct Ret { std::string v; };
    struct Brk {};
    struct Cont {};
    struct Proc { std::vector<std::pair<std::string, std::optional<std::string>>> params; std::string body; std::string file; int line; std::string ns; };

    std::string file_;
    int line_ = 0;  // of the command being run
    int depth_ = 0;
    std::vector<std::unordered_map<std::string, std::string>> frames_;  // [0] is global
    std::vector<std::set<std::string>> globals_;
    // upvar / variable links of each frame: local name -> {frame, name there}
    std::vector<std::unordered_map<std::string, std::pair<size_t, std::string>>> links_;
    std::string errInfo_, errCode_;   // set by error ?info? ?code?, read by catch
    std::map<std::string, Proc> procs_;   // by qualified name, without the leading ::
    std::vector<std::string> ns_{""};     // the current namespace ("" global, "a::b")
    std::set<std::string> namespaces_;
    std::string qualify(const std::string& n) const {   // a name in the current namespace
        if (n.rfind("::", 0) == 0) return n.substr(2);
        return ns_.back().empty() ? n : ns_.back() + "::" + n;
    }

    // ------------------------------------------------------------ diagnostics
    void diag(Diag::Sev sev, const char* code, const Loc& loc, std::string text) {
        diags.push_back({sev, code, loc, std::move(text)});
    }
    Loc here() const { return Loc{file_, line_}; }
    [[noreturn]] void fail(const std::string& m) const { throw Error(m); }

    // -------------------------------------------------------------- variables
    std::unordered_map<std::string, std::string>& scopeOf(std::string& name) {
        if (name.rfind("::", 0) == 0) { name.erase(0, 2); return frames_[0]; }
        if (links_.size() < frames_.size()) links_.resize(frames_.size());
        size_t par = name.find('(');
        std::string base = par == std::string::npos ? name : name.substr(0, par);
        auto& lk = links_[frames_.size() - 1];
        if (auto it = lk.find(base); it != lk.end()) {   // upvar / variable (an array element follows its array)
            name = it->second.second + (par == std::string::npos ? "" : name.substr(par));
            return frames_[it->second.first];
        }
        if (frames_.size() > 1 && globals_.back().count(name)) return frames_[0];
        // at the top level of a namespace eval, a plain name is a namespace variable
        if (frames_.size() == 1 && !ns_.back().empty() && base.find("::") == std::string::npos) name = ns_.back() + "::" + name;
        return frames_.back();
    }
    // Where a variable of frame t lives: {frame, name}, following its links.
    std::pair<size_t, std::string> resolveIn(size_t t, std::string name) {
        if (name.rfind("::", 0) == 0) return {0, name.substr(2)};
        if (links_.size() < frames_.size()) links_.resize(frames_.size());
        if (auto it = links_[t].find(name); it != links_[t].end()) return it->second;
        if (t > 0 && globals_[t].count(name)) return {0, name};
        if (t == 0 && frames_.size() == 1 && !ns_.back().empty() && name.find("::") == std::string::npos) return {0, ns_.back() + "::" + name};
        return {t, name};
    }
    // The frame `level` names (upvar, uplevel): 1 the caller, #0 global.
    size_t levelFrame(const std::string& level) {
        size_t cur = frames_.size() - 1;
        if (!level.empty() && level[0] == '#') {
            long n = strtol(level.c_str() + 1, nullptr, 10);
            if (n < 0 || (size_t)n > cur) fail("bad level \"" + level + "\"");
            return (size_t)n;
        }
        long n = strtol(level.c_str(), nullptr, 10);
        if (n < 0 || (size_t)n > cur) fail("bad level \"" + level + "\"");
        return cur - (size_t)n;
    }
    static bool isLevel(const std::string& s) {
        if (s.empty()) return false;
        size_t k = s[0] == '#' ? 1 : 0;
        if (k >= s.size()) return false;
        for (; k < s.size(); k++) if (!isdigit((unsigned char)s[k])) return false;
        return true;
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
        // A bus index, as in get_ports data[3] or q_reg[*]: the netlist tools
        // read it as part of the name, not as a command.
        if (busBrackets && !inner.empty() && inner.find_first_not_of("0123456789*:") == std::string::npos) {
            out += "[" + inner + "]";
            return;
        }
        int savedLine = line_;
        out += eval(inner, startLine, /*nested*/ true);
        line_ = savedLine;
    }

    // One command's words from s at p. False at the end of the script.
    bool nextCommand(const std::string& s, size_t& p, int& line, std::vector<std::string>& words, int& cmdLine, size_t& startP) {
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
        startP = p;
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
            bool parsed = false;
            size_t start = p;
            try {
                if (!nextCommand(script, p, line, words, cmdLine, start)) break;
                parsed = true;
                if (words.empty()) continue;
                line_ = cmdLine;
                result = run(words);
            } catch (const Error& e) {
                // Inside [ ] the error belongs to the command around it.
                if (nested) throw;
                diag(Diag::ErrorSev, "syntax", Loc{file_, cmdLine}, e.what());
                // The command did not parse to its end: skip the rest of it.
                if (!parsed) {
                    line = cmdLine;
                    p = start;
                    resync(script, p, line);
                }
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
        Interp& r;
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

    static void wantArgs(const std::vector<std::string>& w, size_t lo, size_t hi, const char* usage) {
        if (w.size() - 1 < lo || w.size() - 1 > hi) throw Error("wrong # args: should be \"" + w[0] + " " + usage + "\"");
    }

    std::string run(const std::vector<std::string>& w) {
        const std::string& c = w[0];
        if (auto h = hostCommands.find(c); h != hostCommands.end()) return h->second(w);
        {
            std::string res;
            if (command(w, res)) return res;
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
        if (c == "upvar") {
            size_t i = 1;
            std::string level = "1";
            if (w.size() % 2 == 0 && i < w.size() && isLevel(w[1])) level = w[i++];
            if (w.size() - i < 2 || (w.size() - i) % 2) fail("wrong # args: should be \"upvar ?level? otherVar localVar ?otherVar localVar ...?\"");
            size_t t = levelFrame(level), cur = frames_.size() - 1;
            if (links_.size() < frames_.size()) links_.resize(frames_.size());
            for (; i + 1 < w.size(); i += 2) {
                auto r = resolveIn(t, w[i]);
                if (r.first == cur && r.second == w[i + 1]) fail("can't upvar from variable to itself");
                links_[cur][w[i + 1]] = r;
            }
            return "";
        }
        if (c == "uplevel") {
            size_t i = 1;
            std::string level = "1";
            if (w.size() > 2 && isLevel(w[1])) level = w[i++];
            if (i >= w.size()) fail("wrong # args: should be \"uplevel ?level? command ?arg ...?\"");
            size_t t = levelFrame(level);
            std::string script;
            for (size_t k = i; k < w.size(); k++) script += (k > i ? " " : "") + w[k];
            if (links_.size() < frames_.size()) links_.resize(frames_.size());
            // run in frame t: the frames above it step aside until it returns
            std::vector<std::unordered_map<std::string, std::string>> fr(std::make_move_iterator(frames_.begin() + t + 1), std::make_move_iterator(frames_.end()));
            std::vector<std::set<std::string>> gl(std::make_move_iterator(globals_.begin() + t + 1), std::make_move_iterator(globals_.end()));
            std::vector<std::unordered_map<std::string, std::pair<size_t, std::string>>> lk(std::make_move_iterator(links_.begin() + t + 1), std::make_move_iterator(links_.end()));
            frames_.resize(t + 1);
            globals_.resize(t + 1);
            links_.resize(t + 1);
            struct Back {
                Interp& r;
                decltype(fr)& f;
                decltype(gl)& g;
                decltype(lk)& l;
                ~Back() {
                    for (auto& x : f) r.frames_.push_back(std::move(x));
                    for (auto& x : g) r.globals_.push_back(std::move(x));
                    for (auto& x : l) r.links_.push_back(std::move(x));
                }
            } back{*this, fr, gl, lk};
            return eval(script, line_);
        }
        if (c == "variable") {   // namespace variables, linked into a proc's frame
            if (w.size() < 2) fail("wrong # args: should be \"variable ?name value...? name ?value?\"");
            for (size_t i = 1; i < w.size(); i += 2) {
                std::string q = qualify(w[i]);
                if (frames_.size() > 1) {
                    if (links_.size() < frames_.size()) links_.resize(frames_.size());
                    links_.back()[w[i]] = {0, q};
                }
                if (i + 1 < w.size()) frames_[0][q] = w[i + 1];
            }
            return "";
        }
        if (c == "namespace") {
            if (w.size() < 2) fail("wrong # args: should be \"namespace subcommand ?arg ...?\"");
            const std::string& sc = w[1];
            if (sc == "eval") {
                if (w.size() < 4) fail("wrong # args: should be \"namespace eval name arg ?arg...?\"");
                std::string q = qualify(w[2]);
                for (size_t k = 0; (k = q.find("::", k)) != std::string::npos; k += 2) namespaces_.insert(q.substr(0, k));
                namespaces_.insert(q);
                std::string script;
                for (size_t k = 3; k < w.size(); k++) script += (k > 3 ? " " : "") + w[k];
                ns_.push_back(q);
                struct Pop { Interp& r; ~Pop() { r.ns_.pop_back(); } } pop{*this};
                return eval(script, line_);
            }
            if (sc == "current") return "::" + ns_.back();
            if (sc == "exists") {
                if (w.size() != 3) fail("wrong # args: should be \"namespace exists name\"");
                std::string q = qualify(w[2]);
                return q.empty() || namespaces_.count(q) ? "1" : "0";
            }
            if (sc == "qualifiers") {
                if (w.size() != 3) fail("wrong # args: should be \"namespace qualifiers string\"");
                size_t k = w[2].rfind("::");
                return k == std::string::npos ? "" : w[2].substr(0, k);
            }
            if (sc == "tail") {
                if (w.size() != 3) fail("wrong # args: should be \"namespace tail string\"");
                size_t k = w[2].rfind("::");
                return k == std::string::npos ? w[2] : w[2].substr(k + 2);
            }
            if (sc == "which") {
                std::string n = w.back();
                if (n.rfind("::", 0) == 0) return procs_.count(n.substr(2)) ? n : "";
                if (!ns_.back().empty() && procs_.count(ns_.back() + "::" + n)) return "::" + ns_.back() + "::" + n;
                return procs_.count(n) ? "::" + n : "";
            }
            if (sc == "delete") {
                for (size_t k = 2; k < w.size(); k++) {
                    std::string q = qualify(w[k]);
                    for (auto it = procs_.begin(); it != procs_.end();)
                        it = it->first.rfind(q + "::", 0) == 0 ? procs_.erase(it) : std::next(it);
                    for (auto it = frames_[0].begin(); it != frames_[0].end();)
                        it = it->first.rfind(q + "::", 0) == 0 ? frames_[0].erase(it) : std::next(it);
                    namespaces_.erase(q);
                }
                return "";
            }
            // export, import, path, ensemble ...: names stay reachable by their qualified form
            return "";
        }
        if (c == "regexp") return cmdRegexp(w);
        if (c == "regsub") return cmdRegsub(w);
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
            std::string q = qualify(w[1]);
            p.ns = q.rfind("::") == std::string::npos ? "" : q.substr(0, q.rfind("::"));
            procs_[q] = std::move(p);
            return "";
        }
        auto pit = procs_.end();
        if (c.rfind("::", 0) == 0) pit = procs_.find(c.substr(2));
        else {
            if (!ns_.back().empty()) pit = procs_.find(ns_.back() + "::" + c);
            if (pit == procs_.end()) pit = procs_.find(c);
        }
        if (auto it = pit; it != procs_.end()) {
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
            links_.resize(frames_.size() - 1);
            links_.emplace_back();
            ns_.push_back(p.ns);
            int savedLine = line_;
            struct Pop {
                Interp& r;
                int l;
                ~Pop() {
                    r.frames_.pop_back();
                    r.globals_.pop_back();
                    r.links_.resize(r.frames_.size());
                    r.ns_.pop_back();
                    r.line_ = l;
                }
            } pop{*this, savedLine};
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
            if (w.size() >= 2 && w[1] == "level") return std::to_string(frames_.size() - 1);
            if (w.size() >= 2 && (w[1] == "procs" || w[1] == "commands")) {
                std::vector<std::string> o;
                for (auto& [n, p] : procs_)
                    if (w.size() < 3 || globMatch(w[2].c_str(), n.c_str(), true)) o.push_back(n);
                return joinList(o);
            }
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
            if (err) {
                diags.resize(before);
                // the global errorInfo and errorCode after an error (UPF 1801-2024 5.12)
                setVar("::errorInfo", errInfo_.empty() ? res + "\n    while executing\n\"" + w[1] + "\"" : errInfo_);
                setVar("::errorCode", errCode_.empty() ? std::string("NONE") : errCode_);
                errInfo_.clear();
                errCode_.clear();
            }
            if (w.size() > 2) setVar(w[2], res);
            return err ? "1" : "0";
        }
        if (c == "error") {   // error message ?info? ?code?
            errInfo_ = w.size() > 2 ? w[2] : "";
            errCode_ = w.size() > 3 ? w[3] : "";
            fail(w.size() > 1 ? w[1] : "error");
        }
        if (c == "exit" || c == "quit") throw Exit{w.size() > 1 ? atoi(w[1].c_str()) : 0};
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
            struct Back { Interp& r; std::string f; int l; ~Back() { r.file_ = f; r.line_ = l; } } back{*this, savedFile, savedLine};
            try { eval(ss.str(), 1); } catch (const Ret&) {}
            return "";
        }
        if (c == "package" || c == "suppress_message" || c == "unsuppress_message" ||
            c == "set_message_info" || c == "redirect" || c == "date" || c == "pid") return "";
        if (onUnknown) {
            std::string res;
            if (onUnknown(w, res)) return res;
        }
        diag(Diag::ErrorSev, "unknown-command", here(), "unknown command '" + c + "'");
        return "";
    }

    // regexp ?switches? exp string ?matchVar? ?subMatchVar ...? (ECMAScript
    // syntax, which covers the common Tcl patterns).
    std::string cmdRegexp(const std::vector<std::string>& w) {
        size_t i = 1;
        bool nocase = false, all = false, inl = false, indices = false;
        size_t start = 0;
        for (; i < w.size() && w[i].size() > 1 && w[i][0] == '-'; i++) {
            if (w[i] == "--") { i++; break; }
            if (w[i] == "-nocase") nocase = true;
            else if (w[i] == "-all") all = true;
            else if (w[i] == "-inline") inl = true;
            else if (w[i] == "-indices") indices = true;
            else if (w[i] == "-start" && i + 1 < w.size()) start = (size_t)strtol(w[++i].c_str(), nullptr, 10);
            else if (w[i] == "-line" || w[i] == "-linestop" || w[i] == "-lineanchor" || w[i] == "-expanded") {}
            else fail("bad switch \"" + w[i] + "\": must be -all, -indices, -inline, -line, -nocase, -start or --");
        }
        if (w.size() - i < 2) fail("wrong # args: should be \"regexp ?-switch ...? exp string ?matchVar? ?subMatchVar ...?\"");
        if (inl && w.size() - i > 2) fail("regexp match variables not allowed when using -inline");
        std::regex re;
        try { re = std::regex(w[i], nocase ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript); }
        catch (const std::regex_error& e) { fail("couldn't compile regular expression pattern: " + std::string(e.what())); }
        const std::string& str = w[i + 1];
        std::vector<std::string> vars(w.begin() + (long)i + 2, w.end());
        auto value = [&](const std::smatch& m, size_t k, size_t off) {
            if (k >= m.size() || !m[k].matched) return indices ? std::string("-1 -1") : std::string();
            if (indices) return std::to_string(off + m.position(k)) + " " + std::to_string(off + m.position(k) + m.length(k) - 1);
            return m[k].str();
        };
        std::vector<std::string> inlineOut;
        long count = 0;
        size_t pos = std::min(start, str.size());
        std::smatch m;
        while (pos <= str.size()) {
            if (!std::regex_search(str.cbegin() + (long)pos, str.cend(), m, re,
                                   pos > 0 ? std::regex_constants::match_prev_avail : std::regex_constants::match_default)) break;
            count++;
            if (inl) for (size_t k = 0; k < m.size(); k++) inlineOut.push_back(value(m, k, pos));
            else for (size_t k = 0; k < vars.size(); k++) setVar(vars[k], value(m, k, pos));
            if (!all) break;
            size_t adv = (size_t)m.position(0) + (size_t)m.length(0);
            pos += adv == 0 ? 1 : adv;
            if (m.length(0) == 0 && pos > str.size()) break;
        }
        if (!count && !inl) for (auto& v : vars) setVar(v, indices ? "-1 -1" : "");
        if (inl) return joinList(inlineOut);
        return std::to_string(count);
    }
    // regsub ?switches? exp string subSpec ?varName?: & and \0 to \9 in subSpec.
    std::string cmdRegsub(const std::vector<std::string>& w) {
        size_t i = 1;
        bool nocase = false, all = false;
        for (; i < w.size() && w[i].size() > 1 && w[i][0] == '-'; i++) {
            if (w[i] == "--") { i++; break; }
            if (w[i] == "-nocase") nocase = true;
            else if (w[i] == "-all") all = true;
            else if (w[i] == "-line" || w[i] == "-linestop" || w[i] == "-lineanchor" || w[i] == "-expanded") {}
            else fail("bad switch \"" + w[i] + "\": must be -all, -line, -nocase or --");
        }
        if (w.size() - i < 3 || w.size() - i > 4) fail("wrong # args: should be \"regsub ?-switch ...? exp string subSpec ?varName?\"");
        std::regex re;
        try { re = std::regex(w[i], nocase ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript); }
        catch (const std::regex_error& e) { fail("couldn't compile regular expression pattern: " + std::string(e.what())); }
        // Tcl's subSpec as an ECMAScript format: & -> $&, \n -> $n, $ -> $$
        std::string fmt;
        const std::string& sp = w[i + 2];
        for (size_t k = 0; k < sp.size(); k++) {
            if (sp[k] == '&') fmt += "$&";
            else if (sp[k] == '$') fmt += "$$";
            else if (sp[k] == '\\' && k + 1 < sp.size() && isdigit((unsigned char)sp[k + 1])) { fmt += sp[k + 1] == '0' ? std::string("$&") : std::string("$") + sp[k + 1]; k++; }
            else if (sp[k] == '\\' && k + 1 < sp.size() && (sp[k + 1] == '&' || sp[k + 1] == '\\')) { fmt += sp[k + 1]; k++; }
            else fmt += sp[k];
        }
        const std::string& str = w[i + 1];
        long n = 0;
        for (auto it = std::sregex_iterator(str.begin(), str.end(), re); it != std::sregex_iterator(); ++it) {
            n++;
            if (!all) break;
        }
        std::string out = std::regex_replace(str, re, fmt, all ? std::regex_constants::format_default : std::regex_constants::format_first_only);
        if (w.size() - i == 4) {
            setVar(w[i + 3], out);
            return std::to_string(n);
        }
        return out;
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

}  // namespace tcl
