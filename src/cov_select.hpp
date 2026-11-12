// Which code coverage msim writes: the metrics (-cm) and the part of the
// design (-cm_hier). One selection, read from three vendors' notations:
//   - metrics: VCS words joined by + (line+cond+fsm+tgl+branch), Xcelium
//     letters joined by : (b:e:t:f, or all / A), Questa +cover letters (bcesftx);
//   - scope file: VCS -cm_hier lines (+tree tb.dut [depth], -tree, +module,
//     -module, +file, -file) and Xcelium covfile commands (select_coverage /
//     deselect_coverage -block -expr -toggle -fsm -all  -instance tb.dut[...]
//     | -module m, set_expr_coverable_operators -all).
// Only code coverage is selected: covergroups and assertions are always written.
#pragma once
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace covsel {

enum : uint32_t { LINE = 1, BRANCH = 2, COND = 4, TGL = 8, FSM = 16, ALL = 31 };

// The metrics of a -cm value; false (and err set) when it names none we know.
inline bool parseMetrics(std::string_view v, uint32_t& mask, std::string& err) {
    auto lower = [](std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
    std::string s(v);
    if (s.empty()) { err = "an empty metric list"; return false; }
    if (s == "all" || s == "A" || s == "ALL") { mask = ALL; return true; }
    auto words = [&](char sep) {
        std::vector<std::string> out;
        size_t p = 0;
        while (p <= s.size()) {
            size_t q = s.find(sep, p);
            if (q == std::string::npos) q = s.size();
            if (q > p) out.push_back(s.substr(p, q - p));
            p = q + 1;
        }
        return out;
    };
    uint32_t m = 0;
    if (s.find(':') != std::string::npos || (s.size() == 1 && std::string_view("betfua").find(s[0]) != std::string_view::npos)) {
        // Xcelium -coverage b:e:t:f:u:a (u, a: functional, always written here)
        for (auto& w : words(':')) {
            std::string l = lower(w);
            if (l == "b" || l == "block") m |= LINE | BRANCH;
            else if (l == "e" || l == "expr") m |= COND;
            else if (l == "t" || l == "toggle") m |= TGL;
            else if (l == "f" || l == "fsm") m |= FSM;
            else if (l == "u" || l == "a" || l == "functional" || l == "assertion") {}
            else if (l == "all") m |= ALL;
            else { err = "unknown metric '" + w + "' (b, e, t, f, u, a, all)"; return false; }
        }
        mask = m;
        return true;
    }
    static const std::string_view vcs[] = {"line", "cond", "fsm", "tgl", "branch", "assert", "obc", "path"};
    bool isVcs = s.find('+') != std::string::npos;
    for (auto& w : words('+'))
        for (auto x : vcs) if (lower(w) == x) isVcs = true;
    if (isVcs) {
        for (auto& w : words('+')) {
            std::string l = lower(w);
            if (l == "line") m |= LINE;
            else if (l == "branch") m |= BRANCH;
            else if (l == "cond") m |= COND;
            else if (l == "tgl") m |= TGL;
            else if (l == "fsm") m |= FSM;
            else if (l == "assert" || l == "obc" || l == "path") {}
            else { err = "unknown metric '" + w + "' (line, cond, fsm, tgl, branch, assert)"; return false; }
        }
        mask = m;
        return true;
    }
    // Questa +cover=bcesftx
    for (char c : s) {
        switch (std::tolower((unsigned char)c)) {
            case 's': m |= LINE; break;
            case 'b': m |= BRANCH; break;
            case 'c': case 'e': m |= COND; break;
            case 'f': m |= FSM; break;
            case 't': case 'x': m |= TGL; break;
            default: err = std::string("unknown metric '") + c + "' in '" + s + "' (b, c, e, s, f, t, x)"; return false;
        }
    }
    mask = m;
    return true;
}

struct Rule {
    bool include = true;
    enum Kind { Tree, Module, File } kind = Tree;
    std::string name;     // instance path (dots), module or file name
    int depth = 0;        // Tree: levels from name (1 = only that instance), 0 = all below
    uint32_t mask = ALL;  // the metrics it is about
};

struct Selection {
    uint32_t metrics = ALL;
    std::vector<Rule> rules;
    bool allops = false;   // set_expr_coverable_operators -all
    std::vector<std::string> notes;

    // Is metric m of the code of instance path / module, in file, written?
    // The last rule that matches decides; with no match, the code is written
    // unless an include rule for that metric exists.
    bool selected(uint32_t m, std::string_view path, std::string_view module, std::string_view file = {}) const {
        if (!(metrics & m)) return false;
        bool anyInclude = false, decided = false, on = false;
        for (auto& r : rules) {
            if (!(r.mask & m)) continue;
            if (r.include) anyInclude = true;
            if (matches(r, path, module, file)) { decided = true; on = r.include; }
        }
        return decided ? on : !anyInclude;
    }

    static bool matches(const Rule& r, std::string_view path, std::string_view module, std::string_view file) {
        switch (r.kind) {
            case Rule::Module: return !module.empty() && module == r.name;
            case Rule::File:
                return !file.empty() && (file == r.name ||
                       (file.size() > r.name.size() && file.ends_with(r.name) && file[file.size() - r.name.size() - 1] == '/'));
            case Rule::Tree: {
                if (path == r.name) return true;
                if (path.size() <= r.name.size() || !path.starts_with(r.name) || path[r.name.size()] != '.') return false;
                if (!r.depth) return true;
                int levels = 1;
                for (size_t i = r.name.size(); i < path.size(); i++) levels += path[i] == '.';
                return levels <= r.depth;
            }
        }
        return false;
    }

    // Read a -cm_hier / -covfile file. False (err set) on a line it cannot read.
    bool read(const std::string& fileName, std::string& err) {
        std::ifstream in(fileName);
        if (!in) { err = "cannot read " + fileName; return false; }
        std::string line;
        int no = 0;
        while (std::getline(in, line)) {
            no++;
            for (auto c : {"//", "#"}) if (auto p = line.find(c); p != std::string::npos) line.resize(p);
            std::istringstream ls(line);
            std::vector<std::string> w;
            for (std::string t; ls >> t;) w.push_back(t);
            if (w.empty()) continue;
            std::string where = fileName + ":" + std::to_string(no) + ": ";
            if (!readLine(w, where, err)) return false;
        }
        return true;
    }

    bool readLine(const std::vector<std::string>& w, const std::string& where, std::string& err) {
        const std::string& c = w[0];
        if (c == "+tree" || c == "-tree" || c == "+module" || c == "-module" || c == "+file" || c == "-file" ||
            c == "+moduletree" || c == "-moduletree") {
            if (w.size() < 2) { err = where + c + " needs a name"; return false; }
            Rule r;
            r.include = c[0] == '+';
            r.kind = c.ends_with("tree") && c.find("module") == std::string::npos ? Rule::Tree
                     : c.ends_with("file") ? Rule::File : Rule::Module;
            r.name = w[1];
            if (r.kind == Rule::Tree && w.size() > 2) r.depth = std::atoi(w[2].c_str());
            if (c.ends_with("moduletree")) notes.push_back(where + c + " is read as " + c.substr(0, 1) + "module (that module's instances only)");
            rules.push_back(r);
            return true;
        }
        if (c == "select_coverage" || c == "deselect_coverage") {
            uint32_t mask = 0;
            std::vector<Rule> named;
            for (size_t i = 1; i < w.size(); i++) {
                const std::string& o = w[i];
                if (o == "-block" || o == "-b") mask |= LINE | BRANCH;
                else if (o == "-expression" || o == "-expr" || o == "-e") mask |= COND;
                else if (o == "-toggle" || o == "-t") mask |= TGL;
                else if (o == "-fsm" || o == "-f") mask |= FSM;
                else if (o == "-all" || o == "-bet" || o == "-betf") mask |= ALL;
                else if ((o == "-instance" || o == "-module") && i + 1 < w.size()) {
                    bool inst = o == "-instance";
                    for (i++; i < w.size() && w[i][0] != '-'; i++) {
                        Rule r;
                        r.kind = inst ? Rule::Tree : Rule::Module;
                        std::string n = w[i];
                        for (auto& ch : n) if (ch == ':') ch = '.';
                        if (inst) {
                            if (n.ends_with("...")) n.resize(n.size() - 3);
                            else r.depth = 1;
                            if (n.ends_with(".")) n.pop_back();
                        }
                        r.name = n;
                        named.push_back(r);
                    }
                    i--;
                } else if (o.size() > 1 && o[0] == '-' && o.find_first_not_of("-betf") == std::string::npos) {
                    for (char ch : o.substr(1)) mask |= ch == 'b' ? LINE | BRANCH : ch == 'e' ? COND : ch == 't' ? TGL : FSM;
                } else { err = where + c + ": unknown option " + o; return false; }
            }
            if (named.empty()) { err = where + c + " needs -instance or -module"; return false; }
            for (auto& r : named) {
                r.include = c == "select_coverage";
                r.mask = mask ? mask : ALL;
                rules.push_back(r);
            }
            return true;
        }
        if (c == "set_expr_coverable_operators") {
            for (size_t i = 1; i < w.size(); i++) if (w[i] == "-all") allops = true;
            return true;
        }
        if (c.starts_with("set_")) {
            notes.push_back(where + "ignored covfile command " + c);
            return true;
        }
        err = where + "unknown line '" + c + "' (+tree, -tree, +module, -module, +file, -file, select_coverage, deselect_coverage)";
        return false;
    }
};

}  // namespace covsel
