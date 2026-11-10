// spice_netlist.hpp - reading a SPICE netlist: logical lines, numbers with
// engineering suffixes, parameter expressions, and the .subckt hierarchy.
// Nothing here knows about devices; spice.hpp turns the cards into a circuit.
#pragma once
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace spice {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// "1k", "2.2u", "10meg", "1e-9", "100pF": a number, an optional scale
// suffix, then letters that are ignored (units). Returns the end, or null.
inline const char* parseNumber(const char* s, double& v) {
    char* e;
    v = std::strtod(s, &e);
    if (e == s) return nullptr;
    // strtod takes "inf"/"nan" and hex; a SPICE number starts with a digit, sign or dot.
    if (!(std::isdigit((unsigned char)*s) || *s == '.' || *s == '-' || *s == '+')) return nullptr;
    const char* p = e;
    auto is = [&](const char* suf) { return !strncasecmp(p, suf, std::strlen(suf)); };
    if (is("meg")) v *= 1e6;
    else if (is("mil")) v *= 25.4e-6;
    else switch (std::tolower((unsigned char)*p)) {
        case 't': v *= 1e12; break;
        case 'g': v *= 1e9; break;
        case 'k': v *= 1e3; break;
        case 'm': v *= 1e-3; break;
        case 'u': v *= 1e-6; break;
        case 'n': v *= 1e-9; break;
        case 'p': v *= 1e-12; break;
        case 'f': v *= 1e-15; break;
        case 'a': v *= 1e-18; break;
        default: break;
    }
    while (std::isalpha((unsigned char)*p)) p++;
    return p;
}

// Parameters of one level of the hierarchy. Values are kept as text and
// evaluated on first use, so a .param may refer to one defined later.
struct Scope {
    std::map<std::string, std::string> expr;
    std::map<std::string, double> cache;
    std::set<std::string> busy;
    Scope* parent = nullptr;

    void set(const std::string& name, const std::string& e) { expr[name] = e; cache.erase(name); }
    void set(const std::string& name, double v) { expr[name] = "0"; cache[name] = v; }
    bool lookup(const std::string& name, double& v);
};

class Expr {
public:
    Expr(const std::string& text, Scope* sc) : text_(text), p_(text_.c_str()), sc_(sc) {}
    double eval() {
        double v = ternary();
        ws();
        if (*p_) fail("unexpected '" + std::string(p_) + "'");
        return v;
    }

private:
    std::string text_;
    const char* p_;
    Scope* sc_;
    [[noreturn]] void fail(const std::string& why) { throw Error("expression '" + text_ + "': " + why); }
    void ws() { while (std::isspace((unsigned char)*p_)) p_++; }
    bool eat(const char* s) {
        ws();
        size_t n = std::strlen(s);
        if (std::strncmp(p_, s, n)) return false;
        p_ += n;
        return true;
    }
    double ternary() {
        double c = lor();
        if (!eat("?")) return c;
        double a = ternary();
        if (!eat(":")) fail("expected ':'");
        double b = ternary();
        return c != 0 ? a : b;
    }
    double lor() {
        double v = land();
        while (eat("||")) { double r = land(); v = (v != 0 || r != 0); }
        return v;
    }
    double land() {
        double v = cmp();
        while (eat("&&")) { double r = cmp(); v = (v != 0 && r != 0); }
        return v;
    }
    double cmp() {
        double v = add();
        for (;;) {
            if (eat("<=")) v = v <= add();
            else if (eat(">=")) v = v >= add();
            else if (eat("==")) v = v == add();
            else if (eat("!=")) v = v != add();
            else if (eat("<")) v = v < add();
            else if (eat(">")) v = v > add();
            else return v;
        }
    }
    double add() {
        double v = mul();
        for (;;) {
            if (eat("+")) v += mul();
            else if (eat("-")) v -= mul();
            else return v;
        }
    }
    double mul() {
        double v = unary();
        for (;;) {
            ws();
            if (p_[0] == '*' && p_[1] != '*') { p_++; v *= unary(); }
            else if (eat("/")) v /= unary();
            else if (eat("%")) v = std::fmod(v, unary());
            else return v;
        }
    }
    double unary() {
        if (eat("-")) return -unary();
        if (eat("+")) return unary();
        if (eat("!")) return unary() == 0;
        double b = primary();
        if (eat("**") || eat("^")) return std::pow(b, unary());
        return b;
    }
    double primary() {
        ws();
        if (eat("(")) {
            double v = ternary();
            if (!eat(")")) fail("expected ')'");
            return v;
        }
        if (std::isdigit((unsigned char)*p_) || *p_ == '.') {
            double v;
            const char* e = parseNumber(p_, v);
            if (!e) fail("bad number");
            p_ = e;
            return v;
        }
        if (std::isalpha((unsigned char)*p_) || *p_ == '_') {
            std::string id;
            while (std::isalnum((unsigned char)*p_) || *p_ == '_') id += *p_++;
            if (eat("(")) {
                std::vector<double> a;
                if (!eat(")")) {
                    do a.push_back(ternary()); while (eat(","));
                    if (!eat(")")) fail("expected ')'");
                }
                return call(id, a);
            }
            if (id == "pi") return M_PI;
            double v;
            if (sc_ && sc_->lookup(id, v)) return v;
            fail("unknown parameter '" + id + "'");
        }
        fail("unexpected '" + std::string(p_) + "'");
    }
    double call(const std::string& f, const std::vector<double>& a) {
        auto need = [&](size_t n) { if (a.size() != n) fail(f + "() takes " + std::to_string(n) + " argument(s)"); };
        static const std::map<std::string, double (*)(double)> one = {
            {"sqrt", std::sqrt}, {"exp", std::exp}, {"ln", std::log}, {"log", std::log},
            {"log10", std::log10}, {"abs", std::fabs}, {"sin", std::sin}, {"cos", std::cos},
            {"tan", std::tan}, {"asin", std::asin}, {"acos", std::acos}, {"atan", std::atan},
            {"sinh", std::sinh}, {"cosh", std::cosh}, {"tanh", std::tanh}, {"floor", std::floor},
            {"ceil", std::ceil}, {"int", std::trunc}};
        if (auto it = one.find(f); it != one.end()) { need(1); return it->second(a[0]); }
        if (f == "min") { need(2); return std::min(a[0], a[1]); }
        if (f == "max") { need(2); return std::max(a[0], a[1]); }
        if (f == "pow" || f == "pwr") { need(2); return std::pow(a[0], a[1]); }
        if (f == "atan2") { need(2); return std::atan2(a[0], a[1]); }
        if (f == "sgn") { need(1); return (a[0] > 0) - (a[0] < 0); }
        if (f == "if") { need(3); return a[0] != 0 ? a[1] : a[2]; }
        if (f == "db") { need(1); return 20 * std::log10(std::fabs(a[0])); }
        fail("unknown function '" + f + "'");
    }
};

inline bool Scope::lookup(const std::string& name, double& v) {
    if (auto c = cache.find(name); c != cache.end()) { v = c->second; return true; }
    auto it = expr.find(name);
    if (it == expr.end()) return parent ? parent->lookup(name, v) : false;
    if (!busy.insert(name).second) throw Error("parameter '" + name + "' is defined in terms of itself");
    try {
        v = Expr(it->second, this).eval();
    } catch (...) {
        busy.erase(name);
        throw;
    }
    busy.erase(name);
    cache[name] = v;
    return true;
}

// A token as a number: a plain number, or an expression ({...} already has
// its braces removed by the reader's caller).
inline double value(std::string tok, Scope* sc) {
    if (tok.size() >= 2 && tok.front() == '{' && tok.back() == '}') tok = tok.substr(1, tok.size() - 2);
    double v;
    const char* e = parseNumber(tok.c_str(), v);
    if (e && !*e) return v;
    return Expr(tok, sc).eval();
}

struct Card {
    std::vector<std::string> tok;
    std::string text;   // the logical line, lower case
    std::string where;  // file:line
};

// The top level of a netlist or one .subckt body.
struct Block {
    std::string name;
    std::vector<std::string> ports;
    std::vector<std::pair<std::string, std::string>> defaults;
    std::vector<Card> cards;
    std::map<std::string, std::unique_ptr<Block>> subckts;
    Block* parent = nullptr;

    const Block* findSubckt(const std::string& n) const {
        for (const Block* b = this; b; b = b->parent)
            if (auto it = b->subckts.find(n); it != b->subckts.end()) return it->second.get();
        return nullptr;
    }
};

struct Netlist {
    std::string title;
    Block top;
    std::vector<Card> control;   // .tran, .ac, .print, ... in file order
};

// Split "a=1 b={x+1}" style text into name/expression pairs. An expression
// runs to the next `name=`.
inline std::vector<std::pair<std::string, std::string>> splitAssignments(const std::string& s, const std::string& where) {
    std::vector<std::pair<std::string, std::string>> out;
    std::vector<std::pair<size_t, size_t>> names;   // [begin, position of '=')
    int depth = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '{' || s[i] == '(') depth++;
        if (s[i] == '}' || s[i] == ')') depth--;
        if (s[i] != '=' || depth) continue;
        if (i + 1 < s.size() && s[i + 1] == '=') { i++; continue; }
        if (i > 0 && (s[i - 1] == '<' || s[i - 1] == '>' || s[i - 1] == '!' || s[i - 1] == '=')) continue;
        size_t e = i;
        while (e > 0 && std::isspace((unsigned char)s[e - 1])) e--;
        size_t b = e;
        while (b > 0 && (std::isalnum((unsigned char)s[b - 1]) || s[b - 1] == '_')) b--;
        if (b == e) throw Error(where + ": expected a name before '='");
        names.push_back({b, i});
    }
    for (size_t k = 0; k < names.size(); k++) {
        size_t b = names[k].first, eq = names[k].second;
        size_t end = k + 1 < names.size() ? names[k + 1].first : s.size();
        std::string name = s.substr(b, eq - b), e = s.substr(eq + 1, end - eq - 1);
        while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
        std::string t;
        for (char c : e)
            if (!std::isspace((unsigned char)c) && c != '{' && c != '}' && c != '\'') t += c;
        if (t.empty()) throw Error(where + ": '" + name + "=' has no value");
        out.push_back({name, t});
    }
    return out;
}

class Reader {
public:
    Netlist read(const std::string& path) {
        Netlist nl;
        std::vector<Line> lines;
        load(path, lines, true, nl.title, 0);
        Block* cur = &nl.top;
        for (Line& ln : lines) {
            Card c = card(ln);
            if (c.tok.empty()) continue;
            const std::string& k = c.tok[0];
            if (k == ".subckt") {
                if (c.tok.size() < 2) throw Error(c.where + ": .subckt needs a name");
                auto b = std::make_unique<Block>();
                b->name = c.tok[1];
                b->parent = cur;
                size_t i = 2;
                for (; i < c.tok.size(); i++) {
                    if (c.tok[i] == "params:" || c.tok[i].find('=') != std::string::npos) break;
                    b->ports.push_back(c.tok[i]);
                }
                for (; i < c.tok.size(); i++) {
                    auto eq = c.tok[i].find('=');
                    if (eq != std::string::npos) b->defaults.push_back({c.tok[i].substr(0, eq), c.tok[i].substr(eq + 1)});
                }
                Block* nb = b.get();
                cur->subckts[b->name] = std::move(b);
                cur = nb;
            } else if (k == ".ends") {
                if (!cur->parent) throw Error(c.where + ": .ends without .subckt");
                cur = cur->parent;
            } else if (k[0] != '.' || k == ".param" || k == ".model" || k == ".params") {
                cur->cards.push_back(std::move(c));
            } else if (k == ".end") {
                break;
            } else {
                nl.control.push_back(std::move(c));
            }
        }
        if (cur->parent) throw Error(path + ": .subckt " + cur->name + " has no .ends");
        return nl;
    }

private:
    struct Line { std::string text, where; };

    static std::string dirOf(const std::string& p) {
        auto s = p.find_last_of("/\\");
        return s == std::string::npos ? "" : p.substr(0, s + 1);
    }

    void load(const std::string& path, std::vector<Line>& out, bool top, std::string& title, int depth) {
        if (depth > 32) throw Error(path + ": .include nested too deep");
        std::ifstream f(path);
        if (!f) throw Error("cannot open " + path);
        std::string raw;
        int no = 0;
        bool control = false;
        while (std::getline(f, raw)) {
            no++;
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            if (top && no == 1) { title = raw; continue; }   // SPICE: the first line is the title
            size_t b = raw.find_first_not_of(" \t");
            if (b == std::string::npos || raw[b] == '*') continue;
            // Inline comments: ';' anywhere, '$' after white space.
            for (size_t i = b; i < raw.size(); i++)
                if (raw[i] == ';' || (raw[i] == '$' && (i == b || std::isspace((unsigned char)raw[i - 1])))) {
                    raw.erase(i);
                    break;
                }
            std::string t = raw.substr(b);
            while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
            if (t.empty()) continue;
            std::string lo = lower(t);
            if (lo.rfind(".control", 0) == 0) { control = true; continue; }
            if (lo.rfind(".endc", 0) == 0) { control = false; continue; }
            if (control) continue;
            if (t[0] == '+') {
                if (out.empty()) throw Error(path + ":" + std::to_string(no) + ": continuation with nothing to continue");
                out.back().text += " " + lo.substr(1);
                continue;
            }
            if (lo.rfind(".include", 0) == 0 || lo.rfind(".inc ", 0) == 0) {
                std::string name = t.substr(t.find_first_of(" \t") == std::string::npos ? t.size() : t.find_first_of(" \t"));
                size_t s = name.find_first_not_of(" \t\"'");
                size_t e = name.find_last_not_of(" \t\"'");
                if (s == std::string::npos) throw Error(path + ":" + std::to_string(no) + ": .include needs a file");
                name = name.substr(s, e - s + 1);
                if (name[0] != '/') name = dirOf(path) + name;
                std::string ignored;
                load(name, out, false, ignored, depth + 1);
                continue;
            }
            out.push_back({lo, path + ":" + std::to_string(no)});
        }
    }

    // Lines whose arguments are written with parentheses that matter: v(out).
    static bool keepsParens(const std::string& s) {
        for (const char* k : {".print", ".meas", ".ic", ".nodeset", ".save", ".probe", ".plot"})
            if (s.rfind(k, 0) == 0) return true;
        return false;
    }

    static Card card(const Line& ln) {
        Card c;
        c.where = ln.where;
        c.text = ln.text;
        const std::string& s = ln.text;
        if (s.rfind(".param", 0) == 0) {
            size_t sp = s.find_first_of(" \t");
            c.tok.push_back(".param");
            if (sp != std::string::npos)
                for (auto& [n, e] : splitAssignments(s.substr(sp), ln.where)) c.tok.push_back(n + "=" + e);
            return c;
        }
        bool parens = keepsParens(s);
        // Pass 1: drop white space inside {...}, '...' and (for the lines
        // above) (...); turn '...' into {...}; outside, ( ) and , separate.
        std::string t;
        int brace = 0, paren = 0;
        bool quote = false;
        for (char ch : s) {
            if (ch == '\'') { quote = !quote; t += quote ? '{' : '}'; continue; }
            if (ch == '{') brace++;
            if (ch == '}') brace--;
            bool inExpr = brace > 0 || quote;
            if (!inExpr && parens) {
                if (ch == '(') paren++;
                if (ch == ')') paren--;
            }
            if (std::isspace((unsigned char)ch)) {
                if (inExpr || paren > 0) continue;
                t += ' ';
            } else if (!inExpr && !parens && (ch == '(' || ch == ')' || ch == ',')) {
                t += ' ';
            } else {
                t += ch;
            }
        }
        // Pass 2: no spaces around '='.
        std::string u;
        for (size_t i = 0; i < t.size(); i++) {
            if (t[i] == ' ') {
                size_t j = i;
                while (j < t.size() && t[j] == ' ') j++;
                if (j < t.size() && t[j] == '=') { i = j - 1; continue; }
                if (!u.empty() && u.back() == '=') { i = j - 1; continue; }
                u += ' ';
                i = j - 1;
            } else {
                u += t[i];
            }
        }
        std::istringstream is(u);
        std::string w;
        while (is >> w) c.tok.push_back(w);
        return c;
    }
};

}  // namespace spice
