// vhdl/cond.hpp - VHDL-2019 conditional analysis tool directives (1076-2019
// 24.2): `if / `elsif / `else / `end, `warning and `error. Runs on the source
// text before lexing; excluded lines become empty so line numbers stay.
#pragma once

#include <cctype>
#include <map>
#include <string>
#include <vector>

#include "lex.hpp"

namespace vhdl {

// The identifiers a tool supplies (24.2.2); `user` adds or overrides (upper-case keys).
inline std::map<std::string, std::string> condAnalysisIds(const std::map<std::string, std::string>& user) {
    std::map<std::string, std::string> m = {
        {"VHDL_VERSION", "2019"}, {"TOOL_TYPE", "SIMULATION"}, {"TOOL_VENDOR", "minisim"},
        {"TOOL_NAME", "msim"},    {"TOOL_EDITION", "free"},    {"TOOL_VERSION", "1.0"},
    };
    for (auto& [k, v] : user) m[k] = v;
    return m;
}

class CondAnalysis {
public:
    CondAnalysis(const std::map<std::string, std::string>& ids, std::string file) : ids(ids), file(std::move(file)) {}
    std::vector<std::string> warnings;

    std::string run(const std::string& src) {
        struct Frame { bool parentOn, on, taken, sawElse; };
        std::vector<Frame> st;
        std::string out;
        out.reserve(src.size());
        size_t pos = 0;
        int ln = 0;
        auto on = [&] { return st.empty() || (st.back().parentOn && st.back().on); };
        while (pos <= src.size()) {
            size_t e = src.find('\n', pos);
            bool last = e == std::string::npos;
            std::string lineText = src.substr(pos, last ? std::string::npos : e - pos);
            ++ln;
            size_t k = lineText.find_first_not_of(" \t\r");
            if (k != std::string::npos && lineText[k] == '`') {
                directive(lineText.substr(k + 1), ln, st, on);
                lineText.clear();   // the directive itself is not VHDL text
            } else if (!on()) {
                lineText.clear();
            }
            out += lineText;
            if (last) break;
            out += '\n';
            pos = e + 1;
        }
        if (!st.empty()) fail(ln, "`if without `end");
        return out;
    }

private:
    std::map<std::string, std::string> ids;
    std::string file;
    std::string t;   // the directive being read
    size_t p = 0;
    int ln = 0;

    [[noreturn]] void fail(int line, const std::string& m) { throw Error(file + ":" + std::to_string(line) + ": " + m); }

    template <class St, class On>
    void directive(const std::string& d, int line, St& st, On on) {
        t = d;
        p = 0;
        ln = line;
        std::string w = lowerStr(word());
        if (w == "if") {
            bool parent = on();
            bool c = parent ? expr() : (skipRest(), false);
            if (parent) expectWord("then");
            st.push_back({parent, c, c, false});
        } else if (w == "elsif") {
            if (st.empty() || st.back().sawElse) fail(line, "`elsif without `if");
            auto& f = st.back();
            bool c = false;
            if (f.parentOn && !f.taken) { c = expr(); expectWord("then"); }
            f.on = c;
            f.taken = f.taken || c;
        } else if (w == "else") {
            if (st.empty() || st.back().sawElse) fail(line, "`else without `if");
            auto& f = st.back();
            f.on = !f.taken;
            f.taken = true;
            f.sawElse = true;
        } else if (w == "end") {
            if (st.empty()) fail(line, "`end without `if");
            st.pop_back();
        } else if (w == "warning" || w == "error") {
            if (!on()) return;
            std::string msg = str();
            if (w == "error") fail(line, "`error: " + msg);
            warnings.push_back(file + ":" + std::to_string(line) + ": `warning: " + msg);
        } else {
            fail(line, "unknown conditional analysis directive `" + w);
        }
    }

    void ws() { while (p < t.size() && std::isspace((unsigned char)t[p])) ++p; }
    void skipRest() { p = t.size(); }
    std::string word() {
        ws();
        size_t b = p;
        while (p < t.size() && (std::isalnum((unsigned char)t[p]) || t[p] == '_')) ++p;
        return t.substr(b, p - b);
    }
    void expectWord(const char* w) {
        std::string x = lowerStr(word());
        if (x != w) fail(ln, std::string("expected '") + w + "' in a conditional analysis directive");
    }
    std::string str() {
        ws();
        if (p >= t.size() || t[p] != '"') fail(ln, "expected a string literal");
        std::string r;
        for (++p; p < t.size(); ++p) {
            if (t[p] == '"') {
                if (p + 1 < t.size() && t[p + 1] == '"') { r += '"'; ++p; continue; }
                ++p;
                return r;
            }
            r += t[p];
        }
        fail(ln, "unterminated string literal");
    }
    // 24.2.1: relations joined by one kind of logical operator
    bool expr() {
        bool v = relation();
        std::string op;
        for (;;) {
            size_t save = p;
            std::string w = lowerStr(word());
            if (w != "and" && w != "or" && w != "xor" && w != "xnor") { p = save; return v; }
            if (!op.empty() && w != op) fail(ln, "mixed logical operators need parentheses");
            op = w;
            bool r = relation();
            if (w == "and") v = v && r;
            else if (w == "or") v = v || r;
            else if (w == "xor") v = v != r;
            else v = v == r;
        }
    }
    bool relation() {
        ws();
        if (p < t.size() && t[p] == '(') {
            ++p;
            bool v = expr();
            ws();
            if (p >= t.size() || t[p] != ')') fail(ln, "expected ')'");
            ++p;
            return v;
        }
        size_t save = p;
        std::string w = word();
        if (lowerStr(w) == "not") {
            ws();
            if (p >= t.size() || t[p] != '(') fail(ln, "not needs a parenthesized expression");
            return !relation();
        }
        if (w.empty()) { p = save; fail(ln, "expected a conditional analysis identifier"); }
        std::string up;
        for (char c : w) up.push_back((char)std::toupper((unsigned char)c));
        auto it = ids.find(up);
        std::string val = it == ids.end() ? "" : it->second;   // undefined: ""
        ws();
        std::string op;
        if (t.compare(p, 2, "/=") == 0 || t.compare(p, 2, "<=") == 0 || t.compare(p, 2, ">=") == 0) { op = t.substr(p, 2); p += 2; }
        else if (p < t.size() && (t[p] == '=' || t[p] == '<' || t[p] == '>')) { op = t.substr(p, 1); ++p; }
        else fail(ln, "expected a relational operator");
        std::string lit = str();
        int c = val.compare(lit);
        if (op == "=") return c == 0;
        if (op == "/=") return c != 0;
        if (op == "<") return c < 0;
        if (op == "<=") return c <= 0;
        if (op == ">") return c > 0;
        return c >= 0;
    }
};

}  // namespace vhdl
