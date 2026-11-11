// svrf.hpp - reads a rule file in the SVRF style (the statement syntax that
// foundry DRC decks are commonly written in) and turns the part of it that
// drc.hpp can run into a deck in drc.hpp's own format.
//
// What is read:
//
//   LAYER name n [n ...]              a drawn layer by GDS layer number, or a
//                                     name for the union of other layers
//   LAYER MAP n DATATYPE d m          layer number m is GDS layer n, datatype d
//   name = <layer expression>         a derived layer; may be used before it
//                                     is defined
//   a AND b   a OR b   a NOT b   a XOR b
//   a INTERACT b    a NOT INTERACT b
//   a INSIDE b      a NOT INSIDE b    a OUTSIDE b
//   SIZE a BY v     HOLES a   RECTANGLE a   NOT RECTANGLE a   COPY a   EXTENT
//   CONNECT a b [BY c]
//   name { @ description
//          local = <layer expression>
//          INT a < v                  width
//          EXT a < v     EXT a b < v  spacing
//          ENC a b < v                enclosure of a by b
//          AREA a < v    AREA a > v
//          DENSITY a < v WINDOW w [STEP s]     (also > v)
//          <layer expression>         every polygon is a result
//   }
//   #DEFINE #UNDEFINE #IFDEF #IFNDEF #ELSE #ENDIF, INCLUDE "file",
//   // and /* */ comments. Keywords and names are case-insensitive.
//
// Binary layer operations are applied left to right; use parentheses.
// Options after a check's constraint (ABUT, SINGULAR, REGION, OPPOSITE, ...)
// are not applied and are listed in a warning, as are statements that set up
// the run (LAYOUT PATH, DRC RESULTS DATABASE, PRECISION, ...). Anything else
// is an error with its line: a rule is never silently dropped.
#pragma once

#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace svrf {

struct Result {
    std::string deck;                    // the rules in drc.hpp's format
    std::vector<std::string> warnings;   // what was read but not applied
    size_t rules = 0;
};

namespace detail {

struct Fail {
    int line;
    std::string msg;
};

inline std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

struct Tok {
    enum Kind { End, Word, Num, Punct, Str, Note } kind = End;
    std::string text;
    std::string up;        // Word: upper case
    double num = 0;
    int line = 0;
    bool first = false;    // the first token of its line
};

// Comments out, preprocessor lines applied, then tokens.
inline std::vector<Tok> lex(const std::string& src, std::set<std::string> defines) {
    std::vector<Tok> out;
    std::vector<std::pair<bool, bool>> conds;   // (this branch is live, the enclosing ones are live)
    auto live = [&] { return conds.empty() || (conds.back().first && conds.back().second); };
    size_t i = 0, n = src.size();
    int line = 1;
    bool first = true, inBlock = false;
    while (i < n) {
        char c = src[i];
        if (inBlock) {
            if (c == '*' && i + 1 < n && src[i + 1] == '/') { inBlock = false; i += 2; continue; }
            if (c == '\n') { ++line; first = true; }
            ++i;
            continue;
        }
        if (c == '\n') { ++line; first = true; ++i; continue; }
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') { inBlock = true; i += 2; continue; }
        if (c == '#' && first) {
            size_t e = src.find('\n', i);
            if (e == std::string::npos) e = n;
            std::istringstream ss(src.substr(i, e - i));
            std::string dir, name;
            ss >> dir >> name;
            dir = upper(dir);
            name = upper(name);
            if (dir == "#DEFINE") { if (live()) defines.insert(name); }
            else if (dir == "#UNDEFINE") { if (live()) defines.erase(name); }
            else if (dir == "#IFDEF") conds.push_back({defines.count(name) != 0, live()});
            else if (dir == "#IFNDEF") conds.push_back({defines.count(name) == 0, live()});
            else if (dir == "#ELSE") {
                if (conds.empty()) throw Fail{line, "#ELSE without #IFDEF"};
                conds.back().first = !conds.back().first;
            } else if (dir == "#ENDIF") {
                if (conds.empty()) throw Fail{line, "#ENDIF without #IFDEF"};
                conds.pop_back();
            } else {
                throw Fail{line, "unknown directive " + dir};
            }
            i = e;
            continue;
        }
        Tok t;
        t.line = line;
        t.first = first;
        size_t j = i;
        if (c == '@') {
            // A rule's description: the rest of the line.
            j = src.find('\n', i);
            if (j == std::string::npos) j = n;
            size_t a = i + 1, b = j;
            while (a < b && std::isspace(static_cast<unsigned char>(src[a]))) ++a;
            while (b > a && std::isspace(static_cast<unsigned char>(src[b - 1]))) --b;
            t.kind = Tok::Note;
            t.text = src.substr(a, b - a);
        } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                   (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            while (j < n && (std::isalnum(static_cast<unsigned char>(src[j])) || src[j] == '.' || src[j] == '_' ||
                             ((src[j] == '-' || src[j] == '+') && (src[j - 1] == 'e' || src[j - 1] == 'E'))))
                ++j;
            t.text = src.substr(i, j - i);
            char* end = nullptr;
            t.num = std::strtod(t.text.c_str(), &end);
            if (*end) {
                // 1x_dummy and the like: a name that starts with a digit.
                t.kind = Tok::Word;
                t.up = upper(t.text);
            } else {
                t.kind = Tok::Num;
            }
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
            while (j < n && (std::isalnum(static_cast<unsigned char>(src[j])) || src[j] == '_' || src[j] == '.' ||
                             src[j] == '$' || src[j] == ':' || src[j] == '#'))
                ++j;
            t.kind = Tok::Word;
            t.text = src.substr(i, j - i);
            t.up = upper(t.text);
        } else if (c == '"' || c == '\'') {
            j = src.find(c, i + 1);
            if (j == std::string::npos || src.substr(i, j - i).find('\n') != std::string::npos)
                throw Fail{line, "unterminated string"};
            t.kind = Tok::Str;
            t.text = src.substr(i + 1, j - i - 1);
            ++j;
        } else {
            t.kind = Tok::Punct;
            j = i + 1;
            if ((c == '<' || c == '>' || c == '=' || c == '!') && j < n && src[j] == '=') ++j;
            t.text = src.substr(i, j - i);
        }
        i = j;
        first = false;
        if (live()) out.push_back(std::move(t));
    }
    if (!conds.empty()) throw Fail{line, "#IFDEF without #ENDIF"};
    Tok end;
    end.line = line;
    end.first = true;
    out.push_back(end);
    return out;
}

struct Node {
    std::string f;             // "" for a layer name, else the deck's function or operator
    std::string name;          // the layer name as written
    double num = 0;            // grow, shrink
    int line = 0;
    std::vector<Node> args;
};

struct Check {
    std::string fn;            // width, space, enclosure, area, density, or "" for a layer expression
    std::vector<Node> layers;
    char cmp = '<';
    double value = 0, window = 0, step = 0;
    int line = 0;
};

struct Rule {
    std::string id, desc;
    std::vector<std::pair<std::string, Node>> locals;
    std::vector<Check> checks;
    int line = 0;
};

struct Layer {
    std::string name;                  // as first written
    std::string deckName;
    bool drawn = false;
    std::vector<std::string> keys;     // drawn: "66/20", "66/*"
    Node expr;
    int line = 0;
    int state = 0;                     // 0 not written, 1 being written, 2 written
};

struct Connect { Node a, b, by; bool hasBy = false; int line = 0; };

struct Parser {
    std::vector<Tok> t;
    size_t p = 0;
    int depth = 0;
    Result& res;
    std::map<std::string, Layer> layers;                    // by upper-case name
    std::vector<std::string> order;                         // layers as they appear
    std::map<long, std::vector<std::string>> maps;          // LAYER MAP: number -> GDS keys
    struct Pending { std::string name; std::vector<Tok> args; int line; };
    std::vector<Pending> layerStmts;
    std::vector<Rule> rules;
    std::vector<Connect> connects;
    std::set<std::string> ignored;

    Parser(std::vector<Tok> toks, Result& r) : t(std::move(toks)), res(r) {}

    const Tok& cur() const { return t[p]; }
    // The statement goes on: not at the end, and not at the start of a new line (outside parentheses).
    bool more() const { return cur().kind != Tok::End && (depth > 0 || !newStatement(p)); }
    // A line start begins a statement unless the line goes on with a binary
    // operator, or the line before stopped at one.
    bool newStatement(size_t q) const {
        if (!t[q].first) return false;
        static const std::set<std::string> joins = {"AND", "OR", "XOR", "INTERACT", "INSIDE", "OUTSIDE", "BY", "WINDOW", "STEP"};
        auto joint = [&](const Tok& k, bool leading) {
            if (k.kind == Tok::Word) return joins.count(k.up) != 0 || (!leading && k.up == "NOT");
            if (k.kind == Tok::Punct)
                return k.text == "<" || k.text == ">" || k.text == "<=" || k.text == ">=" || k.text == "==" || k.text == "=";
            return false;
        };
        if (joint(t[q], true)) return false;
        return !(q > 0 && joint(t[q - 1], false));
    }
    bool word(const char* w) const { return cur().kind == Tok::Word && cur().up == w; }
    bool punct(const char* s) const { return cur().kind == Tok::Punct && cur().text == s; }
    [[noreturn]] void fail(const std::string& m, int line = 0) const { throw Fail{line ? line : cur().line, m}; }

    void skipLine() {
        ++p;
        while (cur().kind != Tok::End && !cur().first) ++p;
    }
    bool atLineStart(size_t q) const { return t[q].kind == Tok::End || newStatement(q); }
    void endOfStatement(const char* what) {
        if (more()) fail(std::string("unexpected '") + cur().text + "' after " + what);
    }
    double number() {
        double sign = 1;
        if (more() && punct("-")) { sign = -1; ++p; }
        if (!more() || cur().kind != Tok::Num) fail("expected a number");
        return sign * t[p++].num;
    }

    Node primary() {
        if (!more()) fail("expected a layer", t[p ? p - 1 : 0].line);
        Node n;
        n.line = cur().line;
        if (punct("(")) {
            ++p; ++depth;
            n = expr();
            if (!punct(")")) fail("expected )");
            ++p; --depth;
            return n;
        }
        if (cur().kind != Tok::Word) fail("expected a layer, found '" + cur().text + "'");
        std::string u = cur().up;
        if (u == "SIZE") {
            ++p;
            Node a = primary();
            if (!more() || !word("BY")) fail("SIZE needs BY and a distance");
            ++p;
            double v = number();
            n.f = v < 0 ? "shrink" : "grow";
            n.num = v < 0 ? -v : v;
            n.args.push_back(std::move(a));
            return n;
        }
        if (u == "HOLES" || u == "RECTANGLE" || u == "COPY") {
            ++p;
            Node a = primary();
            if (u == "COPY") return a;
            n.f = u == "HOLES" ? "holes" : "rects";
            n.args.push_back(std::move(a));
            return n;
        }
        if (u == "NOT") {
            ++p;
            if (!more() || !word("RECTANGLE")) fail("NOT at the start of an expression can only be NOT RECTANGLE");
            ++p;
            n.f = "not_rects";
            n.args.push_back(primary());
            return n;
        }
        if (u == "EXTENT") {
            ++p;
            n.f = "extent";
            return n;
        }
        static const std::set<std::string> edgeOps = {"INT", "INTERNAL", "EXT", "EXTERNAL", "ENC", "ENCLOSURE", "AREA",
                                                      "DENSITY", "LENGTH", "ANGLE", "WITH", "EXPAND", "GROW", "SHRINK",
                                                      "CONVEX", "VERTEX", "PERIMETER", "DONUT", "STAMP", "NET", "OFFGRID"};
        if (edgeOps.count(u)) fail(cur().text + " is not supported inside a layer expression");
        n.name = cur().text;
        ++p;
        return n;
    }

    Node expr() {
        Node l = primary();
        while (more() && cur().kind == Tok::Word) {
            std::string u = cur().up, f;
            int line = cur().line;
            size_t save = p;
            ++p;
            if (u == "AND") f = "&";
            else if (u == "OR") f = "|";
            else if (u == "XOR") f = "^";
            else if (u == "INTERACT") f = "interact";
            else if (u == "INSIDE") f = "inside";
            else if (u == "OUTSIDE") f = "outside";
            else if (u == "NOT") {
                if (more() && word("INTERACT")) { f = "not_interact"; ++p; }
                else if (more() && word("INSIDE")) { f = "not_inside"; ++p; }
                else if (more() && cur().kind == Tok::Word &&
                         (cur().up == "OUTSIDE" || cur().up == "TOUCH" || cur().up == "ENCLOSE" || cur().up == "CUT"))
                    fail("NOT " + cur().text + " is not supported");
                else f = "-";
            } else if (u == "TOUCH" || u == "ENCLOSE" || u == "CUT" || u == "COIN" || u == "COINCIDENT" || u == "IN" ||
                       u == "WITH") {
                fail(t[save].text + " is not supported", line);
            } else {
                p = save;
                break;
            }
            Node n;
            n.f = f;
            n.line = line;
            n.args.push_back(std::move(l));
            n.args.push_back(primary());
            if (more() && f != "&" && f != "|" && f != "^" && f != "-" &&
                (punct("<") || punct(">") || punct("==") || punct("<=") || punct(">=") || punct("!=")))
                fail("a count constraint on " + t[save].text + " is not supported", line);
            l = std::move(n);
        }
        return l;
    }

    // What is left of the statement, as written.
    std::string rest() {
        std::string s;
        while (more()) {
            if (!s.empty()) s += ' ';
            s += cur().text;
            ++p;
        }
        return s;
    }

    Check check(const std::string& ruleId) {
        Check c;
        c.line = cur().line;
        std::string u = cur().kind == Tok::Word ? cur().up : "";
        std::string kw = cur().text;
        if (u == "INT" || u == "INTERNAL") c.fn = "width";
        else if (u == "EXT" || u == "EXTERNAL") c.fn = "space";
        else if (u == "ENC" || u == "ENCLOSURE") c.fn = "enclosure";
        else if (u == "AREA") c.fn = "area";
        else if (u == "DENSITY") c.fn = "density";
        if (c.fn.empty()) {
            c.layers.push_back(expr());
            endOfStatement("the layer expression");
            return c;
        }
        ++p;
        auto atConstraint = [&] {
            return punct("<") || punct(">") || punct("<=") || punct(">=") || punct("==") || punct("!=");
        };
        while (more() && !atConstraint()) {
            if (c.layers.size() == 2) fail(kw + " takes at most two layers");
            c.layers.push_back(primary());
        }
        if (c.layers.empty()) fail(kw + " needs a layer");
        if (!more()) fail(kw + " needs a constraint, as in " + kw + " M1 < 0.1", c.line);
        std::string op = cur().text;
        ++p;
        c.value = number();
        bool sized = c.fn == "area" || c.fn == "density";
        if (op == ">" && sized) c.cmp = '>';
        else if (op != "<") fail("the constraint " + op + " is not supported on " + kw + (sized ? " (only < and >)" : " (only <)"), c.line);
        if (more() && atConstraint()) fail("a range constraint is not supported on " + kw, c.line);
        size_t want = c.fn == "enclosure" ? 2 : 1;
        if (c.fn == "space" && c.layers.size() == 2) want = 2;
        if (c.layers.size() != want) {
            if (c.fn == "width") fail(kw + " between two layers is not supported", c.line);
            fail(kw + " takes " + (want == 2 ? "two layers" : "one layer"), c.line);
        }
        if (c.fn == "density") {
            while (more() && (word("WINDOW") || word("STEP"))) {
                bool win = word("WINDOW");
                ++p;
                double v = number();
                if (more() && cur().kind == Tok::Num) {
                    if (cur().num != v) fail("a window that is not square is not supported", c.line);
                    ++p;
                }
                (win ? c.window : c.step) = v;
            }
            if (c.window <= 0) fail("DENSITY needs WINDOW and a size", c.line);
        }
        std::string opts = rest();
        if (!opts.empty())
            res.warnings.push_back("line " + std::to_string(c.line) + ": rule " + ruleId + ": options not applied: " + opts);
        return c;
    }

    void rule() {
        Rule r;
        r.id = cur().text;
        r.line = cur().line;
        p += 2;   // name {
        ++depth;  // lines inside the braces are told apart by hand
        auto lineEnd = [&](size_t q) { return atLineStart(q); };
        while (true) {
            if (cur().kind == Tok::End) fail("rule " + r.id + " has no closing }", r.line);
            if (punct("}")) { ++p; break; }
            if (cur().kind == Tok::Note) {
                if (!r.desc.empty() && !cur().text.empty()) r.desc += ' ';
                r.desc += cur().text;
                ++p;
                continue;
            }
            // One statement: up to the next line start or the closing brace.
            size_t e = p + 1;
            int par = punct("(") ? 1 : 0;
            for (;; ++e) {
                if (t[e].kind == Tok::End || t[e].kind == Tok::Note) break;
                if (par <= 0 && (lineEnd(e) || (t[e].kind == Tok::Punct && t[e].text == "}"))) break;
                if (t[e].kind == Tok::Punct && t[e].text == "(") ++par;
                if (t[e].kind == Tok::Punct && t[e].text == ")") --par;
            }
            // Parse t[p, e) as its own statement.
            Tok stop;
            stop.line = t[e].line;
            stop.first = true;
            std::vector<Tok> part(t.begin() + long(p), t.begin() + long(e));
            for (Tok& k : part) k.first = false;
            part.push_back(stop);
            Parser sub(std::move(part), res);
            if (sub.t.size() > 2 && sub.t[0].kind == Tok::Word && sub.t[1].kind == Tok::Punct && sub.t[1].text == "=") {
                std::string name = sub.t[0].text;
                sub.p = 2;
                Node n = sub.expr();
                sub.endOfStatement("the layer expression");
                r.locals.push_back({name, std::move(n)});
            } else {
                r.checks.push_back(sub.check(r.id));
            }
            p = e;
        }
        --depth;
        if (r.checks.empty()) fail("rule " + r.id + " has no check", r.line);
        rules.push_back(std::move(r));
    }

    void run() {
        static const std::set<std::string> setup = {
            "LAYOUT", "DRC", "PRECISION", "RESOLUTION", "TITLE", "UNIT", "FLAG", "TEXT", "PORT", "SOURCE", "LVS", "ERC",
            "MASK", "VIRTUAL", "SVRF", "GROUP", "POLYGON", "ATTACH", "DEVICE", "TRACE", "LABEL", "SCONNECT", "PEX",
            "VARIABLE", "EXCLUDE", "DFM", "TVF", "SNAP", "NET", "PATHCHK", "DISCONNECT"};
        while (cur().kind != Tok::End) {
            const Tok& k = cur();
            if (k.kind != Tok::Word) fail("unexpected '" + k.text + "'");
            const Tok& nx = t[p + 1];
            if (nx.kind == Tok::Punct && nx.text == "{") { rule(); continue; }
            if (nx.kind == Tok::Punct && nx.text == "=" && !nx.first) {
                std::string name = k.text;
                int line = k.line;
                p += 2;
                Node n = expr();
                endOfStatement("the layer expression");
                define(name, line).expr = std::move(n);
                continue;
            }
            if (k.up == "LAYER") {
                int line = k.line;
                ++p;
                if (more() && word("MAP")) {
                    ++p;
                    if (!more() || cur().kind != Tok::Num) fail("LAYER MAP needs a GDS layer number");
                    long src = long(t[p++].num);
                    std::string key = std::to_string(src) + "/*";
                    if (more() && (word("DATATYPE") || word("TEXTTYPE"))) {
                        bool text = word("TEXTTYPE");
                        ++p;
                        if (!more() || cur().kind != Tok::Num) fail("only LAYER MAP n DATATYPE d m is supported", line);
                        key = std::to_string(src) + "/" + std::to_string(long(t[p++].num));
                        if (text) { skipLineRest(); continue; }
                    }
                    if (!more() || cur().kind != Tok::Num) fail("LAYER MAP needs a target layer number", line);
                    maps[long(t[p++].num)].push_back(key);
                    endOfStatement("LAYER MAP");
                    continue;
                }
                if (!more() || cur().kind != Tok::Word) fail("LAYER needs a name");
                Pending pd;
                pd.name = t[p++].text;
                pd.line = line;
                while (more()) pd.args.push_back(t[p++]);
                if (pd.args.empty()) fail("LAYER " + pd.name + " needs a layer number", line);
                layerStmts.push_back(std::move(pd));
                continue;
            }
            if (k.up == "CONNECT") {
                Connect c;
                c.line = k.line;
                ++p;
                c.a = primary();
                c.b = primary();
                if (more() && word("BY")) { ++p; c.by = primary(); c.hasBy = true; }
                if (more()) fail("CONNECT takes two layers and an optional BY layer", c.line);
                connects.push_back(std::move(c));
                continue;
            }
            if (k.up == "INCLUDE") fail("INCLUDE is read by svrf::read, not from text");
            if (setup.count(k.up)) {
                std::string what = k.up;
                if (!t[p + 1].first && t[p + 1].kind == Tok::Word) what += " " + t[p + 1].up;
                ignored.insert(what);
                skipLine();
                continue;
            }
            fail("statement " + k.text + " is not supported");
        }
        // The LAYER statements, now that every LAYER MAP is known.
        for (Pending& pd : layerStmts) {
            Layer& l = define(pd.name, pd.line);
            bool allNum = true;
            for (const Tok& a : pd.args) allNum = allNum && a.kind == Tok::Num;
            auto keysOf = [&](const Tok& a, std::vector<std::string>& keys) {
                long n = long(a.num);
                auto m = maps.find(n);
                if (m != maps.end()) keys.insert(keys.end(), m->second.begin(), m->second.end());
                else keys.push_back(std::to_string(n) + "/*");
            };
            if (allNum) {
                l.drawn = true;
                for (const Tok& a : pd.args) keysOf(a, l.keys);
                continue;
            }
            // A union of named layers (and numbers, each a drawn layer of its own).
            Node u;
            bool have = false;
            for (const Tok& a : pd.args) {
                Node n;
                n.line = pd.line;
                if (a.kind == Tok::Num) {
                    std::string hidden = "$" + pd.name + "$" + a.text;
                    Layer& d = define(hidden, pd.line);
                    d.drawn = true;
                    keysOf(a, d.keys);
                    n.name = hidden;
                } else if (a.kind == Tok::Word) {
                    n.name = a.text;
                } else {
                    fail("unexpected '" + a.text + "' in LAYER " + pd.name, pd.line);
                }
                if (!have) { u = std::move(n); have = true; continue; }
                Node o;
                o.f = "|";
                o.line = pd.line;
                o.args.push_back(std::move(u));
                o.args.push_back(std::move(n));
                u = std::move(o);
            }
            define(pd.name, 0).expr = std::move(u);
        }
    }
    void skipLineRest() {
        while (more()) ++p;
    }

    Layer& define(const std::string& name, int line) {
        std::string u = upper(name);
        auto it = layers.find(u);
        if (it != layers.end()) {
            if (line) fail("layer " + name + " is defined twice (line " + std::to_string(it->second.line) + ")", line);
            return it->second;
        }
        Layer& l = layers[u];
        l.name = name;
        l.line = line;
        order.push_back(u);
        return l;
    }

    // ---- writing the deck ----
    std::ostringstream out;
    std::set<std::string> used;
    int temps = 0;

    std::string deckName(const std::string& name) {
        // The deck's names: a letter or _ first, then letters, digits, _ and '.'.
        std::string s;
        for (char c : name) s += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') ? c : '_';
        if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) s = "_" + s;
        static const std::set<std::string> taken = {"layer", "rule", "connect", "label", "device", "grow", "shrink", "interact",
                                                    "not_interact", "inside", "not_inside", "outside", "holes", "rects",
                                                    "not_rects", "not_rect", "extent", "width", "space", "enclosure",
                                                    "overhang", "area", "density", "by", "opposite"};
        std::string base = taken.count(s) ? s + "_" : s, u = base;
        for (int k = 2; used.count(u); ++k) u = base + "_" + std::to_string(k);
        used.insert(u);
        return u;
    }

    static std::string num(double v) {
        char b[48];
        std::snprintf(b, sizeof b, "%.9g", v);
        return b;
    }

    // The deck text of an expression; layers it needs are written first.
    std::string text(const Node& n, const std::map<std::string, std::string>& locals) {
        if (n.f.empty()) {
            std::string u = upper(n.name);
            auto lo = locals.find(u);
            if (lo != locals.end()) return lo->second;
            auto it = layers.find(u);
            if (it == layers.end()) fail("layer " + n.name + " is not defined", n.line);
            write(it->second);
            return it->second.deckName;
        }
        if (n.f == "extent") return "extent()";
        std::string a = text(n.args[0], locals);
        if (n.f == "grow" || n.f == "shrink") return n.f + "(" + a + ", " + num(n.num) + ")";
        if (n.args.size() == 1) return n.f + "(" + a + ")";
        std::string b = text(n.args[1], locals);
        if (n.f == "&" || n.f == "|" || n.f == "-" || n.f == "^") return "(" + a + " " + n.f + " " + b + ")";
        return n.f + "(" + a + ", " + b + ")";
    }

    void write(Layer& l) {
        if (l.state == 2) return;
        if (l.state == 1) fail("layer " + l.name + " is defined by itself", l.line);
        l.state = 1;
        if (l.drawn) {
            l.deckName = deckName(l.name);
            out << "layer " << l.deckName;
            for (const std::string& k : l.keys) out << ' ' << k;
            out << '\n';
        } else {
            std::string e = text(l.expr, {});
            l.deckName = deckName(l.name);
            out << l.deckName << " = " << e << '\n';
        }
        l.state = 2;
    }

    void emit() {
        out << "# Translated from an SVRF rule file by mpv.\n";
        for (const std::string& u : order)
            if (layers[u].drawn) write(layers[u]);
        std::set<std::string> ids;
        for (const Rule& r : rules) {
            std::map<std::string, std::string> locals;
            for (const auto& [name, node] : r.locals) {
                std::string e = text(node, locals);
                std::string t2 = deckName("_" + r.id + "_" + name);
                out << t2 << " = " << e << '\n';
                locals[upper(name)] = t2;
            }
            std::string desc = r.desc;
            for (char& c : desc)
                if (c == '"') c = '\'';
            for (size_t k = 0; k < r.checks.size(); ++k) {
                const Check& c = r.checks[k];
                std::string id = r.id;
                for (char& ch : id)
                    if (ch == '#') ch = '_';
                if (k) id += "/" + std::to_string(k + 1);
                if (!ids.insert(id).second) fail("rule " + r.id + " is defined twice", r.line);
                std::string line = "rule " + id + " ";
                if (c.fn.empty()) {
                    line += text(c.layers[0], locals);
                } else {
                    line += c.fn + "(" + text(c.layers[0], locals);
                    if (c.layers.size() == 2) line += ", " + text(c.layers[1], locals);
                    if (c.fn == "density") {
                        line += ", " + num(c.window);
                        if (c.step > 0) line += ", " + num(c.step);
                    }
                    line += std::string(") ") + c.cmp + " " + num(c.value);
                }
                if (!desc.empty()) line += " \"" + desc + "\"";
                out << line << '\n';
                ++res.rules;
            }
        }
        for (const Connect& c : connects) {
            std::string a = text(c.a, {}), b = text(c.b, {});
            std::string line = "connect " + a + " " + b;
            if (c.hasBy) line += " by " + text(c.by, {});
            out << line << '\n';
        }
        if (!ignored.empty()) {
            std::string s = "statements not applied:";
            for (const std::string& i : ignored) s += " " + i + ";";
            s.pop_back();
            res.warnings.push_back(s);
        }
        res.deck = out.str();
    }
};

}  // namespace detail

// Translates SVRF text. `defines` are names taken as #DEFINEd.
inline bool translate(const std::string& text, Result& res, std::string& err, const std::set<std::string>& defines = {}) {
    res = Result();
    try {
        std::set<std::string> defs;
        for (const std::string& d : defines) defs.insert(detail::upper(d));
        detail::Parser ps(detail::lex(text, defs), res);
        ps.run();
        ps.emit();
    } catch (const detail::Fail& f) {
        err = "line " + std::to_string(f.line) + ": " + f.msg;
        return false;
    }
    return true;
}

// Reads a rule file, with the files it INCLUDEs (relative to the file that
// names them) spliced in.
inline bool read(const std::string& file, Result& res, std::string& err, const std::set<std::string>& defines = {}) {
    std::string text;
    std::vector<std::string> stack;
    auto load = [&](auto&& self, const std::string& path) -> bool {
        for (const std::string& s : stack)
            if (s == path) { err = path + " includes itself"; return false; }
        if (stack.size() > 32) { err = "INCLUDE is nested too deep at " + path; return false; }
        std::ifstream in(path);
        if (!in) { err = "cannot open " + path; return false; }
        stack.push_back(path);
        std::string dir = path.find_last_of("/\\") == std::string::npos ? "" : path.substr(0, path.find_last_of("/\\") + 1);
        for (std::string line; std::getline(in, line);) {
            std::istringstream ss(line);
            std::string w, f;
            ss >> w;
            if (detail::upper(w) == "INCLUDE") {
                std::getline(ss, f);
                size_t a = f.find_first_not_of(" \t\"'"), b = f.find_last_not_of(" \t\r\"'");
                if (a == std::string::npos) { err = path + ": INCLUDE needs a file name"; return false; }
                f = f.substr(a, b - a + 1);
                if (f[0] != '/' && !(f.size() > 1 && f[1] == ':')) f = dir + f;
                if (!self(self, f)) return false;
                text += '\n';
                continue;
            }
            text += line;
            text += '\n';
        }
        stack.pop_back();
        return true;
    };
    if (!load(load, file)) return false;
    if (!translate(text, res, err, defines)) {
        err = file + ": " + err;
        return false;
    }
    return true;
}

}  // namespace svrf
