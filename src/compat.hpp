// compat.hpp - vendor command-line compatibility layer shared by every tool.
//
// Every minisim tool has ONE engine. To accept the command lines users already
// have for Synopsys, Cadence or Siemens tools, a tool registers a table of
// option rules here, and the layer rewrites argv into the tool's native options
// before the tool parses them. Nothing here implements tool behaviour.
//
//   std::vector<std::string> args(argv + 1, argv + argc);
//   compat::Vendor v = compat::detect(argv[0], args, kAliases);   // --compat=, argv0
//   compat::Result r = compat::translate(v, args, kRules);
//   for (auto& n : r.notes) std::cerr << n << "\n";
//   parseNativeOptions(r.args);
//
// Selection: `--compat=synopsys|cadence|siemens` (or `-compat X`) anywhere on
// the command line, else the basename of argv[0] looked up in the tool's alias
// table (so a user may create their own symlink). minisim ships no binaries
// named after other vendors' products.
//
// Unknown options pass through untouched, so the native parser still reports
// them. Rules match exactly (Cadence rules case-insensitively, as xrun does),
// or as a prefix when `prefix` is set.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace compat {

enum class Vendor { Native, Synopsys, Cadence, Siemens };

inline const char* name(Vendor v) {
    switch (v) {
    case Vendor::Synopsys: return "synopsys";
    case Vendor::Cadence: return "cadence";
    case Vendor::Siemens: return "siemens";
    default: return "native";
    }
}

inline std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// Accepts the vendor names and their common short forms.
inline std::optional<Vendor> parseVendor(std::string_view s) {
    std::string l = lower(s);
    if (l == "native" || l == "minisim" || l == "none") return Vendor::Native;
    if (l == "synopsys" || l == "snps") return Vendor::Synopsys;
    if (l == "cadence" || l == "cdns") return Vendor::Cadence;
    if (l == "siemens" || l == "mentor" || l == "mgc") return Vendor::Siemens;
    return std::nullopt;
}

struct Alias {
    std::string_view argv0;  // basename without directory or ".exe"
    Vendor vendor;
};

inline std::string basename(std::string_view path) {
    size_t p = path.find_last_of("/\\");
    std::string b(p == std::string_view::npos ? path : path.substr(p + 1));
    if (b.size() > 4 && lower(b.substr(b.size() - 4)) == ".exe") b.resize(b.size() - 4);
    return b;
}

// Removes every --compat=X / -compat X from args (the last one wins), else
// falls back to argv0. Throws nothing: an unknown vendor name is left in args
// so the tool reports it as a bad option.
inline Vendor detect(std::string_view argv0, std::vector<std::string>& args,
                     std::span<const Alias> aliases, Vendor def = Vendor::Native) {
    std::optional<Vendor> chosen;
    std::vector<std::string> out;
    out.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        std::optional<std::string> val;
        if (a.rfind("--compat=", 0) == 0) val = a.substr(9);
        else if (a.rfind("-compat=", 0) == 0) val = a.substr(8);
        else if ((a == "--compat" || a == "-compat") && i + 1 < args.size()) {
            if (auto v = parseVendor(args[i + 1])) { chosen = v; ++i; continue; }
        }
        if (val) {
            if (auto v = parseVendor(*val)) { chosen = v; continue; }
        }
        out.push_back(a);
    }
    args.swap(out);
    if (chosen) return *chosen;
    std::string b = basename(argv0);
    for (const Alias& al : aliases)
        if (b == al.argv0) return al.vendor;
    return def;
}

enum class Act {
    Rename,  // replace with `to` (split on spaces); keep the value if takesValue
    Drop,    // remove (and its value if takesValue); recorded in notes
};

struct Rule {
    Vendor vendor;
    std::string_view from;
    Act act = Act::Rename;
    std::string_view to = {};
    bool takesValue = false;  // the option's value is the next argument
    bool prefix = false;      // match `from` as a prefix; the rest is appended to `to`
    std::string_view note = {};
};

struct Result {
    std::vector<std::string> args;
    std::vector<std::string> notes;  // one line per dropped option
};

namespace detail {
inline bool caseless(Vendor v) { return v == Vendor::Cadence; }

inline bool startsWith(std::string_view s, std::string_view p, bool ci) {
    if (s.size() < p.size()) return false;
    if (!ci) return s.substr(0, p.size()) == p;
    return lower(s.substr(0, p.size())) == lower(p);
}

inline void splitInto(std::string_view s, std::vector<std::string>& out) {
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ') ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
}
}  // namespace detail

// Rules for options that only control licensing or 32/64-bit selection. They
// have no meaning for minisim and are dropped for every tool.
inline std::span<const Rule> commonRules() {
    static const Rule r[] = {
        {Vendor::Synopsys, "-full64", Act::Drop, {}, false, false, "minisim is always 64-bit"},
        {Vendor::Synopsys, "-licqueue", Act::Drop, {}, false, false, "no licence queue"},
        {Vendor::Synopsys, "+lic+wait", Act::Drop, {}, false, false, "no licence queue"},
        {Vendor::Cadence, "-64bit", Act::Drop, {}, false, false, "minisim is always 64-bit"},
        {Vendor::Cadence, "-licqueue", Act::Drop, {}, false, false, "no licence queue"},
        {Vendor::Cadence, "-nocopyright", Act::Drop},
        {Vendor::Siemens, "-64", Act::Drop, {}, false, false, "minisim is always 64-bit"},
        {Vendor::Siemens, "-nolicqueue", Act::Drop},
        {Vendor::Siemens, "-licqueue", Act::Drop, {}, false, false, "no licence queue"},
    };
    return r;
}

// Rewrites args for vendor v using the tool's rules, then the common rules.
// Native or no matching rule: the argument is copied unchanged.
inline Result translate(Vendor v, const std::vector<std::string>& args,
                        std::span<const Rule> rules) {
    Result res;
    if (v == Vendor::Native) { res.args = args; return res; }
    const bool ci = detail::caseless(v);
    auto find = [&](const std::string& a) -> const Rule* {
        for (auto set : {rules, commonRules()})
            for (const Rule& r : set) {
                if (r.vendor != v) continue;
                if (r.prefix ? detail::startsWith(a, r.from, ci)
                             : a.size() == r.from.size() && detail::startsWith(a, r.from, ci))
                    return &r;
            }
        return nullptr;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const Rule* r = find(a);
        if (!r) { res.args.push_back(a); continue; }
        bool hasVal = r->takesValue && i + 1 < args.size();
        if (r->act == Act::Drop) {
            std::string n = "ignored " + std::string(name(v)) + " option " + a;
            if (hasVal) n += " " + args[i + 1];
            if (!r->note.empty()) n += " (" + std::string(r->note) + ")";
            res.notes.push_back(std::move(n));
        } else {
            std::string to(r->to);
            if (r->prefix) to += a.substr(r->from.size());
            detail::splitInto(to, res.args);
            if (hasVal) res.args.push_back(args[i + 1]);
        }
        if (hasVal) ++i;
    }
    return res;
}

enum class Severity { Note, Warning, Error, Fatal };

struct Msg {
    Severity sev = Severity::Error;
    std::string_view tool;  // e.g. "msim"; Cadence and Siemens put a tool name in front
    std::string_view code;  // short message id
    std::string_view file;  // may be empty
    int line = 0;
    int col = 0;
    std::string_view text;
};

// Formats a diagnostic the way the selected vendor's tools print it, so that
// existing log filters and regression scripts keep working.
//   native   : file:line:col: error: text [CODE]
//   synopsys : Error-[CODE] text\n  "file", line
//   cadence  : tool: *E,CODE (file,line|col): text
//   siemens  : ** Error: (tool-CODE) file(line): text
inline std::string format(Vendor v, const Msg& m) {
    static const char* lowerSev[] = {"note", "warning", "error", "fatal"};
    static const char* titleSev[] = {"Note", "Warning", "Error", "Fatal"};
    static const char cdnSev[] = {'N', 'W', 'E', 'F'};
    const int s = static_cast<int>(m.sev);
    std::string o;
    switch (v) {
    case Vendor::Synopsys:
        o = std::string(titleSev[s]) + "-[" + std::string(m.code) + "] " + std::string(m.text);
        if (!m.file.empty()) o += "\n  \"" + std::string(m.file) + "\", " + std::to_string(m.line);
        return o;
    case Vendor::Cadence:
        o = std::string(m.tool) + ": *" + cdnSev[s] + "," + std::string(m.code);
        if (!m.file.empty()) {
            o += " (" + std::string(m.file) + "," + std::to_string(m.line);
            if (m.col) o += "|" + std::to_string(m.col);
            o += ")";
        }
        return o + ": " + std::string(m.text);
    case Vendor::Siemens:
        o = "** " + std::string(titleSev[s]) + ": (" + std::string(m.tool) + "-" + std::string(m.code) + ")";
        if (!m.file.empty()) o += " " + std::string(m.file) + "(" + std::to_string(m.line) + ")";
        return o + ": " + std::string(m.text);
    default:
        if (!m.file.empty()) {
            o = std::string(m.file) + ":" + std::to_string(m.line) + ":";
            if (m.col) o += std::to_string(m.col) + ":";
            o += " ";
        }
        o += std::string(lowerSev[s]) + ": " + std::string(m.text);
        if (!m.code.empty()) o += " [" + std::string(m.code) + "]";
        return o;
    }
}

}  // namespace compat
