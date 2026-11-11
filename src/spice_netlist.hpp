// spice_netlist.hpp - reading a SPICE netlist: logical lines, numbers with
// engineering suffixes, parameter expressions, and the .subckt hierarchy.
// Nothing here knows about devices; spice.hpp turns the cards into a circuit.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
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
// The random numbers of one Monte Carlo run: a counter-based generator, so
// a run's samples depend on the seed and the run number only.
struct Random {
    uint64_t state;
    Random(uint64_t seed, uint64_t run) : state(seed * 0x9E3779B97F4A7C15ull + run * 0xD1B54A32D192ED03ull + 0x632BE59BD9B4E019ull) {}
    uint64_t next() {   // splitmix64
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }   // [0, 1)
    double normal() {   // Box-Muller
        double u = 1 - uniform(), v = uniform();
        return std::sqrt(-2 * std::log(u)) * std::cos(2 * M_PI * v);
    }
};

struct Scope {
    Random* random = nullptr;   // set on the top scope of a Monte Carlo run
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
        // Statistical parameters: the nominal value unless this is a Monte
        // Carlo run. gauss/unif take a relative variation, agauss/aunif an
        // absolute one; the third argument of the Gaussians is how many
        // sigma that variation is.
        if (f == "gauss" || f == "agauss" || f == "unif" || f == "aunif") {
            bool g = f.back() == 's' , rel = f[0] != 'a';
            if (g ? (a.size() < 2 || a.size() > 4) : (a.size() < 2 || a.size() > 3)) fail(f + "() takes nominal, variation" + (g ? ", sigma" : ""));
            Random* r = nullptr;
            for (Scope* s = sc_; s; s = s->parent) if (s->random) r = s->random;
            if (!r) return a[0];
            double var = rel ? a[1] * std::fabs(a[0]) : a[1];
            if (!g) return a[0] + var * (2 * r->uniform() - 1);
            double sigma = a.size() > 2 && a[2] != 0 ? a[2] : 1;
            return a[0] + var / sigma * r->normal();
        }
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
    std::string alter;           // the .alter block this deck is the result of ("" for the deck itself)

    // The top-level subcircuits point back at `top`, which moves with the netlist.
    Netlist() = default;
    Netlist(Netlist&& o) noexcept : title(std::move(o.title)), top(std::move(o.top)), control(std::move(o.control)), alter(std::move(o.alter)) { adopt(); }
    Netlist& operator=(Netlist&& o) noexcept {
        title = std::move(o.title);
        top = std::move(o.top);
        control = std::move(o.control);
        alter = std::move(o.alter);
        adopt();
        return *this;
    }

private:
    void adopt() {
        for (auto& [n, b] : top.subckts) b->parent = &top;
    }
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
        std::ifstream f(path);
        if (!f) throw Error("cannot open " + path);
        return read(f, path);
    }
    // A netlist held in memory. `name` stands for the file in messages, and
    // an .include with a relative path is looked for next to it.
    Netlist readText(const std::string& text, const std::string& name = "<text>") {
        std::istringstream f(text);
        return read(f, name);
    }
    Netlist read(std::istream& f, const std::string& path) { return std::move(readAll(f, path)[0]); }

    // The netlist and, after it, one more for every .alter block: the deck
    // with that block's lines applied on top of the one before. In a block,
    //   an element, .model or .subckt of a name already there replaces it;
    //   an analysis (.op .dc .ac .tran ...) replaces those of its kind, a
    //   .measure the one of its name;
    //   .lib 'file' section replaces what an earlier .lib took from that file;
    //   everything else (.param, .temp, .option, new elements) is added, and
    //   a later .param or option wins.
    std::vector<Netlist> readAll(const std::string& path) {
        std::ifstream f(path);
        if (!f) throw Error("cannot open " + path);
        return readAll(f, path);
    }
    std::vector<Netlist> readAll(std::istream& f, const std::string& path) {
        std::string title;
        std::vector<Line> all;
        load(f, path, all, true, title, 0, "", "");
        std::vector<std::vector<Line>> parts(1);
        std::vector<std::string> names{""};
        for (Line& ln : all) {
            if (ln.text.rfind(".alter", 0) == 0 && (ln.text.size() == 6 || std::isspace((unsigned char)ln.text[6]))) {
                parts.emplace_back();
                size_t b = ln.text.find_first_not_of(" \t", 6);
                names.push_back(b == std::string::npos ? "" : ln.text.substr(b));
            } else if (ln.text == ".end" || ln.text.rfind(".end ", 0) == 0) {
                break;
            } else {
                parts.back().push_back(std::move(ln));
            }
        }
        std::vector<Netlist> out;
        std::vector<Line> deck = std::move(parts[0]);
        out.push_back(parse(deck, path, title));
        for (size_t k = 1; k < parts.size(); k++) {
            deck = altered(deck, parts[k]);
            out.push_back(parse(deck, path, title));
            out.back().alter = names[k].empty() ? "alter " + std::to_string(k) : names[k];
        }
        return out;
    }

private:
    struct Line { std::string text, where, lib; };

    static std::string word(const std::string& s, size_t n) {
        std::istringstream is(s);
        std::string w;
        for (size_t i = 0; i <= n; i++)
            if (!(is >> w)) return "";
        return w;
    }
    // What a line is, for replacement by an .alter block: "" when it is
    // only ever added.
    static std::string identity(const std::string& t) {
        std::string w0 = word(t, 0);
        if (w0.empty()) return "";
        if (w0[0] != '.') return "e:" + w0;
        if (w0 == ".model") return "m:" + word(t, 1);
        if (w0 == ".meas" || w0 == ".measure") {
            std::string a = word(t, 1);
            bool kind = a == "tran" || a == "ac" || a == "dc" || a == "op";
            return "s:" + word(t, kind ? 2 : 1);
        }
        for (const char* a : {".op", ".dc", ".ac", ".tran", ".noise", ".tf", ".sens", ".four"})
            if (w0 == a) return std::string("a:") + a;
        return "";
    }

    static std::vector<Line> altered(const std::vector<Line>& base, const std::vector<Line>& block) {
        std::set<std::string> libs, ids, subs;
        int depth = 0;
        for (const Line& ln : block) {
            if (!ln.lib.empty()) libs.insert(ln.lib);
            std::string w0 = word(ln.text, 0);
            if (w0 == ".subckt") { if (depth++ == 0) subs.insert(word(ln.text, 1)); continue; }
            if (w0 == ".ends") { depth--; continue; }
            if (depth == 0)
                if (std::string id = identity(ln.text); !id.empty()) ids.insert(id);
        }
        std::vector<Line> out;
        depth = 0;
        bool dropping = false;
        for (const Line& ln : base) {
            std::string w0 = word(ln.text, 0);
            if (w0 == ".subckt") {
                if (depth++ == 0) dropping = subs.count(word(ln.text, 1)) != 0;
                if (!dropping && !libs.count(ln.lib)) out.push_back(ln);
                continue;
            }
            if (w0 == ".ends") {
                depth--;
                if (!dropping && !libs.count(ln.lib)) out.push_back(ln);
                if (depth == 0) dropping = false;
                continue;
            }
            if (dropping || (!ln.lib.empty() && libs.count(ln.lib))) continue;
            if (depth == 0 && ids.count(identity(ln.text))) continue;
            out.push_back(ln);
        }
        out.insert(out.end(), block.begin(), block.end());
        return out;
    }

    Netlist parse(const std::vector<Line>& lines, const std::string& path, const std::string& title) {
        Netlist nl;
        nl.title = title;
        Block* cur = &nl.top;
        for (const Line& ln : lines) {
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
                // HSPICE's "sweep monte=N" at the end of an analysis line is
                // the same as a line ".step monte N".
                for (size_t i = 1; i + 1 < c.tok.size(); i++)
                    if (c.tok[i] == "sweep" && c.tok[i + 1].rfind("monte=", 0) == 0) {
                        Card st;
                        st.where = c.where;
                        st.tok = {".step", "monte", c.tok[i + 1].substr(6)};
                        st.text = ".step monte " + st.tok[2];
                        c.tok.erase(c.tok.begin() + i, c.tok.begin() + i + 2);
                        nl.control.push_back(std::move(st));
                        break;
                    }
                nl.control.push_back(std::move(c));
            }
        }
        if (cur->parent) throw Error(path + ": .subckt " + cur->name + " has no .ends");
        return nl;
    }

    static std::string dirOf(const std::string& p) {
        auto s = p.find_last_of("/\\");
        return s == std::string::npos ? "" : p.substr(0, s + 1);
    }

    // `want`: read only the section of that name (the file is a library:
    // ".lib name" ... ".endl"). `tag` marks the lines as taken from a library
    // file, so that an .alter block can take another section instead.
    void load(std::istream& f, const std::string& path, std::vector<Line>& out, bool top, std::string& title,
              int depth, const std::string& want, const std::string& tag) {
        std::string raw;
        int no = 0;
        bool control = false;
        bool inside = want.empty(), found = want.empty(), fresh = true;
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
            if (lo.rfind(".lib", 0) == 0 && (lo.size() == 4 || std::isspace((unsigned char)lo[4]))) {
                std::istringstream is(t.substr(4));
                std::string a, b;
                is >> a >> b;
                auto strip = [](std::string s) {
                    while (!s.empty() && (s.front() == '\'' || s.front() == '"')) s.erase(0, 1);
                    while (!s.empty() && (s.back() == '\'' || s.back() == '"')) s.pop_back();
                    return s;
                };
                a = strip(a); b = strip(b);
                if (a.empty()) throw Error(path + ":" + std::to_string(no) + ": .lib needs a section name, or a file and a section");
                if (b.empty()) {
                    // A section starts here; outside a library read it is skipped.
                    inside = !want.empty() && lower(a) == want;
                    if (inside) found = true;
                    if (want.empty()) inside = false;
                    fresh = true;
                    continue;
                }
                if (!inside) continue;
                if (a[0] != '/') a = dirOf(path) + a;
                if (depth >= 32) throw Error(path + ": .lib nested too deep");
                std::ifstream inc(a);
                if (!inc) throw Error(path + ":" + std::to_string(no) + ": cannot open " + a);
                std::string ignored;
                load(inc, a, out, false, ignored, depth + 1, lower(b), tag.empty() ? a : tag);
                fresh = true;
                continue;
            }
            if (lo.rfind(".endl", 0) == 0) { inside = want.empty(); fresh = true; continue; }
            if (!inside) continue;
            if (t[0] == '+') {
                if (out.empty() || fresh) throw Error(path + ":" + std::to_string(no) + ": continuation with nothing to continue");
                out.back().text += " " + lo.substr(1);
                continue;
            }
            fresh = false;
            if (lo.rfind(".include", 0) == 0 || lo.rfind(".inc ", 0) == 0) {
                std::string name = t.substr(t.find_first_of(" \t") == std::string::npos ? t.size() : t.find_first_of(" \t"));
                size_t s = name.find_first_not_of(" \t\"'");
                size_t e = name.find_last_not_of(" \t\"'");
                if (s == std::string::npos) throw Error(path + ":" + std::to_string(no) + ": .include needs a file");
                name = name.substr(s, e - s + 1);
                if (name[0] != '/') name = dirOf(path) + name;
                std::string ignored;
                if (depth >= 32) throw Error(path + ": .include nested too deep");
                std::ifstream inc(name);
                if (!inc) throw Error(path + ":" + std::to_string(no) + ": cannot open " + name);
                load(inc, name, out, false, ignored, depth + 1, "", tag);
                continue;
            }
            out.push_back({lo, path + ":" + std::to_string(no), tag});
        }
        if (!found) throw Error(path + ": no .lib section '" + want + "'");
    }

    // Lines whose arguments are written with parentheses that matter: v(out).
    static bool keepsParens(const std::string& s) {
        for (const char* k : {".print", ".meas", ".ic", ".nodeset", ".save", ".probe", ".plot", ".noise", ".tf", ".sens", ".four"})
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

// ------------------------------------------------------------------ flatten
//
// The netlist as a list of primitive elements with flat names, for tools
// that want the devices and their connections rather than a simulation
// (layout versus schematic, cell characterization).

struct Primitive {
    std::string path;                 // "x1.x2.m1"
    char kind = 0;                    // first letter of the element: 'm', 'r', 'c', 'd', 'q', ..., 'x'
    std::vector<std::string> nodes;   // flat net names in card order; ground is "0"
    std::string model;                // model name ("" when the element has none)
    std::map<std::string, double> params;
    std::string where;                // file:line of the card
};

struct Flat {
    std::vector<std::string> ports;   // of the top subcircuit, in .subckt order; empty for the top level
    std::vector<Primitive> prims;     // in netlist order, depth first
};

// Flattens the top level of the netlist, or the subcircuit `top` when one is
// named. What a caller can rely on:
//
// - Net names. A net of the top block keeps its name; a net inside instance
//   x1 is "x1.net". A net connected through a port has the name it has in
//   the highest block that sees it. "0" and "gnd" are "0" everywhere, and a
//   .global net has its bare name at every level.
// - Nodes. R, C, L, D, V, I: two. M: four (d g s b). Q: three, or four when
//   a substrate node comes before the model. E, G: four (out+, out-, in+,
//   in-). F, H: two, and `model` is the flat name of the controlling source.
//   K: none, and `nodes` holds the flat names of the two inductors.
// - Parameters are numbers in SI units, expressions evaluated in the scope
//   of the instance ("w=650000u" gives 0.65). The value of an R, C, L, E, G,
//   F, H or K is "value"; a plain DC value of a V or I is "dc"; the area
//   written after a D or Q model is "area". An R, C or L written with a
//   model in place of a value ("r1 a b rpoly w=1u l=10u") has that model.
// - X of a subcircuit the netlist defines is expanded. Its m= multiplies the
//   m of everything inside (unless the subcircuit has its own parameter m).
// - X of a subcircuit the netlist does not define is kept as kind 'x': the
//   last word before the name=value pairs is `model`, the words before it
//   are `nodes`, and the pairs are `params`.
inline Flat flatten(const Netlist& nl, const std::string& top = "") {
    struct Env {
        Scope sc;
        const Block* blk = nullptr;
        std::string prefix;
        std::map<std::string, std::string> ports;
        double mult = 1;
    };
    std::set<std::string> globals;
    for (const Card& k : nl.control)
        if (k.tok[0] == ".global") globals.insert(k.tok.begin() + 1, k.tok.end());
    Flat out;
    auto declare = [](Env& e) {
        for (const Card& k : e.blk->cards)
            if (k.tok[0] == ".param" || k.tok[0] == ".params")
                for (size_t i = 1; i < k.tok.size(); i++) {
                    auto eq = k.tok[i].find('=');
                    if (eq == std::string::npos) throw Error(k.where + ": .param needs name=value");
                    e.sc.set(k.tok[i].substr(0, eq), k.tok[i].substr(eq + 1));
                }
    };
    auto net = [&](Env& e, const std::string& t) -> std::string {
        if (t == "0" || t == "gnd") return "0";
        if (auto it = e.ports.find(t); it != e.ports.end()) return it->second;
        if (globals.count(t)) return t;
        return e.prefix + t;
    };
    auto isKV = [](const std::string& t) { return t.find('=') != std::string::npos; };
    int depth = 0;
    std::function<void(Env&)> walk = [&](Env& e) {
        for (const Card& k : e.blk->cards) {
            const auto& t = k.tok;
            if (t.empty() || t[0][0] == '.') continue;
            try {
                // Positional words, then name=value pairs.
                size_t last = t.size();
                while (last > 1 && (isKV(t[last - 1]) || t[last - 1] == "params:")) last--;
                Primitive p;
                p.kind = t[0][0];
                p.path = e.prefix + t[0];
                p.where = k.where;
                auto pairs = [&](Scope& into, bool evaluate) {
                    for (size_t i = last; i < t.size(); i++) {
                        auto eq = t[i].find('=');
                        if (eq == std::string::npos) continue;
                        double v = value(t[i].substr(eq + 1), &e.sc);
                        if (evaluate) p.params[t[i].substr(0, eq)] = v;
                        else into.set(t[i].substr(0, eq), v);
                    }
                };
                auto nodes = [&](size_t n) {
                    if (last < 1 + n) throw Error("expected " + std::to_string(n) + " nodes");
                    for (size_t i = 1; i <= n; i++) p.nodes.push_back(net(e, t[i]));
                };
                // A word that is a number or an expression of known parameters.
                auto number = [&](const std::string& w, double& v) {
                    try { v = value(w, &e.sc); return true; }
                    catch (const Error&) { return false; }
                };
                if (p.kind == 'x') {
                    if (last < 2) throw Error("expected Xname nodes... subcircuit");
                    const std::string& sub = t[last - 1];
                    const Block* b = e.blk->findSubckt(sub);
                    if (!b) {
                        for (size_t i = 1; i + 1 < last; i++) p.nodes.push_back(net(e, t[i]));
                        p.model = sub;
                        pairs(e.sc, true);
                        if (e.mult != 1) p.params["m"] = e.mult * (p.params.count("m") ? p.params["m"] : 1.0);
                        out.prims.push_back(std::move(p));
                        continue;
                    }
                    if (last - 2 != b->ports.size())
                        throw Error("'" + sub + "' has " + std::to_string(b->ports.size()) + " ports, " +
                                    std::to_string(last - 2) + " given");
                    if (++depth > 64) throw Error("subcircuits nested too deep (is '" + sub + "' inside itself?)");
                    Env child;
                    child.blk = b;
                    child.prefix = p.path + ".";
                    child.sc.parent = &e.sc;
                    child.mult = e.mult;
                    for (size_t i = 0; i < b->ports.size(); i++) child.ports[b->ports[i]] = net(e, t[1 + i]);
                    bool ownM = false;
                    for (auto& [pn, pe] : b->defaults) { child.sc.set(pn, pe); ownM |= pn == "m"; }
                    pairs(child.sc, false);
                    if (double m; !ownM && child.sc.expr.count("m") && child.sc.lookup("m", m)) child.mult *= m;
                    declare(child);
                    walk(child);
                    depth--;
                    continue;
                }
                size_t at = 1;   // the next positional word
                double v;
                switch (p.kind) {
                case 'r': case 'c': case 'l':
                    nodes(2); at = 3;
                    if (at < last) {
                        if (number(t[at], v)) p.params["value"] = v;
                        else p.model = t[at];
                        at++;
                        // "r1 a b 1k rmodel" and "r1 a b rmodel 1k"
                        if (at < last) {
                            if (number(t[at], v)) p.params["value"] = v;
                            else if (p.model.empty()) p.model = t[at];
                        }
                    }
                    break;
                case 'd':
                    nodes(2);
                    if (last > 3) p.model = t[3];
                    if (last > 4 && number(t[4], v)) p.params["area"] = v;
                    break;
                case 'q': {
                    double unused;
                    bool four = last > 5 && !number(t[5], unused);
                    nodes(four ? 4 : 3);
                    at = four ? 5 : 4;
                    if (at >= last) throw Error("expected a model name");
                    p.model = t[at];
                    if (at + 1 < last && number(t[at + 1], v)) p.params["area"] = v;
                    break;
                }
                case 'm':
                    nodes(4);
                    if (last < 6) throw Error("expected Mname d g s b model");
                    p.model = t[5];
                    break;
                case 'v': case 'i':
                    nodes(2);
                    if (last > 3 && t[3] == "dc" && last > 4) { if (number(t[4], v)) p.params["dc"] = v; }
                    else if (last > 3 && number(t[3], v)) p.params["dc"] = v;
                    break;
                case 'e': case 'g':
                    nodes(4);
                    if (last > 5 && number(t[5], v)) p.params["value"] = v;
                    break;
                case 'f': case 'h':
                    nodes(2);
                    if (last < 5) throw Error("expected a controlling source and a gain");
                    p.model = e.prefix + t[3];
                    if (number(t[4], v)) p.params["value"] = v;
                    break;
                case 'k':
                    if (last < 4) throw Error("expected Kname L1 L2 coupling");
                    p.nodes = {e.prefix + t[1], e.prefix + t[2]};
                    if (number(t[3], v)) p.params["value"] = v;
                    break;
                default:
                    // An element kind this reader has no rule for: every
                    // positional word is a node.
                    for (size_t i = 1; i < last; i++) p.nodes.push_back(net(e, t[i]));
                    break;
                }
                pairs(e.sc, true);
                if (e.mult != 1) p.params["m"] = e.mult * (p.params.count("m") ? p.params["m"] : 1.0);
                out.prims.push_back(std::move(p));
            } catch (const Error& x) {
                std::string w = x.what();
                throw Error(w.rfind(k.where, 0) == 0 ? w : k.where + ": " + w);
            }
        }
    };
    Env root;
    root.blk = &nl.top;
    declare(root);
    if (top.empty()) {
        walk(root);
        return out;
    }
    const Block* b = nl.top.findSubckt(lower(top));
    if (!b) throw Error("no .subckt named '" + top + "'");
    Env e;
    e.blk = b;
    e.sc.parent = &root.sc;
    for (const std::string& port : b->ports) {
        std::string name = port == "gnd" ? "0" : port;
        e.ports[port] = name;
        out.ports.push_back(name);
    }
    for (auto& [pn, pe] : b->defaults) e.sc.set(pn, pe);
    declare(e);
    walk(e);
    return out;
}

}  // namespace spice
