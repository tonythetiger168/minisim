// scan.hpp - mlsp's workspace index: which file defines which module,
// interface, program, package, primitive, checker, class and `define, and
// which names, macros and `include files each file uses.
//
// It is a token scan (comments and strings skipped), not a parse: it runs
// over every SystemVerilog file of a workspace in parallel in well under a
// second, and it still works on a file that does not parse. The analysis
// itself is always slang's (mlsp.cpp); the index only decides which files go
// into it and answers "where is module X" for a file slang has not seen.
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace lsp {

struct UnitDef {
    std::string name, kind;  // kind: module, interface, program, package, primitive, checker, class, macro
    std::string path;
    unsigned line = 0, col = 0;  // 0-based line, byte column
};

struct FileScan {
    std::vector<UnitDef> defs;
    std::set<std::string> uses;      // names used where a design unit can be: X u(...), X #(...), X::y, import X, X port
    std::set<std::string> macros;    // `NAME uses
    std::vector<std::string> includes;  // `include "x"
};

inline bool isSvSource(const std::filesystem::path& p) {
    auto e = p.extension().string();
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e == ".sv" || e == ".v" || e == ".svh" || e == ".vh" || e == ".svi" || e == ".svp" ||
           e == ".sva" || e == ".vlib" || e == ".vp";
}
inline bool isHeader(const std::filesystem::path& p) {
    auto e = p.extension().string();
    return e == ".svh" || e == ".vh" || e == ".svi";
}

inline FileScan scanText(std::string_view t, const std::string& path) {
    FileScan fs;
    struct Tok {
        std::string_view s;
        unsigned line, col;
    };
    std::vector<Tok> toks;
    unsigned line = 0;
    size_t lineStart = 0;
    auto isId0 = [](char c) { return std::isalpha((unsigned char)c) || c == '_'; };
    auto isId = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; };
    for (size_t k = 0; k < t.size();) {
        char c = t[k];
        if (c == '\n') { line++; lineStart = ++k; continue; }
        if (c == '/' && k + 1 < t.size() && t[k + 1] == '/') {
            while (k < t.size() && t[k] != '\n') k++;
            continue;
        }
        if (c == '/' && k + 1 < t.size() && t[k + 1] == '*') {
            k += 2;
            while (k + 1 < t.size() && !(t[k] == '*' && t[k + 1] == '/')) {
                if (t[k] == '\n') { line++; lineStart = k + 1; }
                k++;
            }
            k += 2;
            continue;
        }
        if (c == '"') {
            size_t b = ++k;
            while (k < t.size() && t[k] != '"' && t[k] != '\n') k += t[k] == '\\' ? 2 : 1;
            // `include "x": the string is the file name.
            if (!toks.empty() && toks.back().s == "`include") fs.includes.emplace_back(t.substr(b, std::min(k, t.size()) - b));
            if (k < t.size() && t[k] == '"') k++;
            continue;
        }
        if (c == '`' && k + 1 < t.size() && isId0(t[k + 1])) {
            size_t b = k++;
            while (k < t.size() && isId(t[k])) k++;
            toks.push_back({t.substr(b, k - b), line, (unsigned)(b - lineStart)});
            continue;
        }
        if (c == '\\') {  // escaped identifier: up to white space
            size_t b = k;
            while (k < t.size() && !std::isspace((unsigned char)t[k])) k++;
            toks.push_back({t.substr(b, k - b), line, (unsigned)(b - lineStart)});
            continue;
        }
        if (isId0(c)) {
            size_t b = k;
            while (k < t.size() && isId(t[k])) k++;
            toks.push_back({t.substr(b, k - b), line, (unsigned)(b - lineStart)});
            continue;
        }
        if (std::isdigit((unsigned char)c) || c == '\'') {  // numbers and based literals: 8'hFF
            k++;
            while (k < t.size() && (std::isalnum((unsigned char)t[k]) || t[k] == '_' || t[k] == '\'')) k++;
            toks.push_back({std::string_view("0"), line, 0});
            continue;
        }
        if (c == ':' && k + 1 < t.size() && t[k + 1] == ':') {
            toks.push_back({t.substr(k, 2), line, (unsigned)(k - lineStart)});
            k += 2;
            continue;
        }
        if (!std::isspace((unsigned char)c)) toks.push_back({t.substr(k, 1), line, (unsigned)(k - lineStart)});
        k++;
    }
    auto isName = [&](std::string_view s) { return !s.empty() && (isId0(s[0]) || s[0] == '\\'); };
    static const std::set<std::string_view> units = {"module", "macromodule", "interface", "program",
                                                     "package", "primitive", "checker", "class"};
    static const std::set<std::string_view> builtinMacros = {
        "`include", "`define", "`undef", "`ifdef", "`ifndef", "`elsif", "`else", "`endif", "`timescale",
        "`default_nettype", "`resetall", "`celldefine", "`endcelldefine", "`line", "`pragma",
        "`begin_keywords", "`end_keywords", "`unconnected_drive", "`nounconnected_drive", "`undefineall",
        "`__FILE__", "`__LINE__", "`protect", "`endprotect", "`protected", "`endprotected"};
    for (size_t k = 0; k < toks.size(); k++) {
        auto& tk = toks[k];
        if (tk.s[0] == '`') {
            if (tk.s == "`define" && k + 1 < toks.size() && toks[k + 1].s[0] != '`') {
                auto& n = toks[k + 1];
                fs.defs.push_back({std::string(n.s), "macro", path, n.line, n.col});
                k++;
                continue;
            }
            if (!builtinMacros.count(tk.s)) fs.macros.insert(std::string(tk.s.substr(1)));
            continue;
        }
        if (!isName(tk.s)) continue;
        {
            std::string_view prev = k ? toks[k - 1].s : std::string_view();
            std::string_view next = k + 1 < toks.size() ? toks[k + 1].s : std::string_view();
            if (next == "::" || next == "#" || prev == "import" || prev == "virtual" || prev == "bind" ||
                (prev == "interface" && k >= 2 && toks[k - 2].s == "virtual") || isName(next))
                fs.uses.insert(std::string(tk.s));
        }
        if (!units.count(tk.s)) continue;
        std::string_view prev = k ? toks[k - 1].s : std::string_view();
        // virtual interface x (a type use), typedef class x (a forward
        // declaration), interface class x (a class), import "DPI-C" ...
        if (prev == "virtual" && tk.s == "interface") continue;
        if (prev == "typedef") continue;
        if (tk.s == "interface" && k + 1 < toks.size() && toks[k + 1].s == "class") continue;
        size_t j = k + 1;
        while (j < toks.size() && (toks[j].s == "automatic" || toks[j].s == "static" || toks[j].s == "virtual" ||
                                   toks[j].s == "interface"))
            j++;
        if (j >= toks.size() || !isName(toks[j].s)) continue;
        std::string kind(tk.s == "macromodule" ? "module" : tk.s);
        fs.defs.push_back({std::string(toks[j].s), kind, path, toks[j].line, toks[j].col});
    }
    return fs;
}

inline bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Every SystemVerilog file under `root`, sorted, skipping build trees and
// hidden directories. Stops at `limit` files.
inline std::vector<std::string> listSources(const std::string& root, size_t limit) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto& p = it->path();
        std::string name = p.filename().string();
        if (it->is_directory(ec)) {
            if (!name.empty() && (name[0] == '.' || name == "node_modules" || name == "_deps" ||
                                  name.rfind("build", 0) == 0 || name == "CMakeFiles"))
                it.disable_recursion_pending();
            continue;
        }
        if (isSvSource(p)) out.push_back(p.lexically_normal().string());
        if (out.size() >= limit) break;
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace lsp
