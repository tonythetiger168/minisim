// pdl.hpp - IEEE 1687 Procedural Description Language for mdft.
//
// PDL is Tcl plus the i-commands. This file holds a small Tcl interpreter
// (set, expr, if, for, foreach, while, incr, proc, return, break, continue,
// puts, list, lindex, llength, lappend, format, string length/equal) and the
// retargeter: iWrite / iRead name instrument-level targets, and each iApply
// turns what is pending into the fewest capture-shift-update scans of the
// network that reach those targets, opening the scan multiplexers (SIBs) on
// the way.
//
// i-commands: iProcsForModule, iProc, iCall, iWrite, iRead, iApply, iReset,
// iRunLoop, iNote, iPrefix. iMerge, iTake, iRelease, iClock, iClockOverride,
// iPDLLevel and iUseProcNameSpace are accepted and ignored. iScan is not
// supported.
#pragma once
#include <cstring>

#include "icl.hpp"

namespace dft {

struct Tcl {
    enum Flow { Normal, Return, Break, Continue };
    struct Proc {
        std::vector<std::pair<std::string, std::pair<bool, std::string>>> args;
        std::string body;
    };
    using Cmd = std::function<std::string(std::vector<std::string>&)>;
    std::vector<std::map<std::string, std::string>> frames{1};
    std::map<std::string, Proc> procs;
    std::map<std::string, Cmd> cmds;
    Flow flow = Normal;
    std::string file;
    int line = 1;
    int depth = 0;
    std::ostream* out = &std::cout;

    Error err(const std::string& m) const { return Error(file + ":" + std::to_string(line) + ": " + m); }

    std::string& var(const std::string& n) {
        auto it = frames.back().find(n);
        if (it == frames.back().end()) throw err("no variable '" + n + "'");
        return it->second;
    }

    static std::vector<std::string> listOf(const std::string& s) {
        std::vector<std::string> out;
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
            if (i >= s.size()) break;
            if (s[i] == '{') {
                int d = 1;
                size_t j = i + 1;
                while (j < s.size() && d) {
                    if (s[j] == '{') d++;
                    if (s[j] == '}') d--;
                    j++;
                }
                out.push_back(s.substr(i + 1, j - i - 2));
                i = j;
            } else {
                size_t j = i;
                while (j < s.size() && !std::isspace((unsigned char)s[j])) j++;
                out.push_back(s.substr(i, j - i));
                i = j;
            }
        }
        return out;
    }
    static bool indexLike(const std::string& s) {
        bool digit = false;
        for (char c : s) {
            if (std::isdigit((unsigned char)c)) digit = true;
            else if (c != ':' && c != ' ') return false;
        }
        return digit;
    }

    // $name, ${name}, [script] and backslash escapes in s[i..]; stops at any
    // character in `stop` outside brackets.
    std::string subst(const std::string& s, size_t& i, const char* stop) {
        std::string out;
        while (i < s.size() && !std::strchr(stop, s[i])) {
            char c = s[i];
            if (c == '\\' && i + 1 < s.size()) {
                char n = s[i + 1];
                i += 2;
                if (n == 'n') out += '\n';
                else if (n == 't') out += '\t';
                else if (n == '\n') out += ' ';
                else out += n;
            } else if (c == '$') {
                size_t j = i + 1;
                std::string name;
                if (j < s.size() && s[j] == '{') {
                    size_t e = s.find('}', j);
                    if (e == std::string::npos) throw err("missing '}' after ${");
                    name = s.substr(j + 1, e - j - 1);
                    j = e + 1;
                } else {
                    while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
                    name = s.substr(i + 1, j - i - 1);
                }
                if (name.empty()) {
                    out += '$';
                    i++;
                } else {
                    out += var(name);
                    i = j;
                }
            } else if (c == '[') {
                int d = 1;
                size_t j = i + 1;
                while (j < s.size() && d) {
                    if (s[j] == '[') d++;
                    if (s[j] == ']') d--;
                    j++;
                }
                if (d) throw err("missing ']'");
                std::string inner = s.substr(i + 1, j - i - 2);
                // name[3] and name[7:0] are bit selects, not command calls.
                size_t k = 0;
                std::string plain = inner.find('$') != std::string::npos ? subst(inner, k, "") : inner;
                if (!out.empty() && indexLike(plain)) out += "[" + plain + "]";
                else out += eval(inner);
                i = j;
            } else {
                out += c;
                i++;
            }
        }
        return out;
    }

    std::string eval(const std::string& s) {
        if (++depth > 200) throw err("PDL nests more than 200 calls deep");
        std::string result;
        size_t i = 0;
        while (i < s.size() && flow == Normal) {
            while (i < s.size() && (std::isspace((unsigned char)s[i]) || s[i] == ';')) {
                if (s[i] == '\n') line++;
                i++;
            }
            if (i >= s.size()) break;
            if (s[i] == '#') {
                while (i < s.size() && s[i] != '\n') i++;
                continue;
            }
            std::vector<std::string> w;
            int startLine = line;
            while (i < s.size() && s[i] != '\n' && s[i] != ';') {
                if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r') { i++; continue; }
                if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\n') { i += 2; line++; continue; }
                if (s[i] == '{') {
                    int d = 1;
                    size_t j = i + 1;
                    while (j < s.size() && d) {
                        if (s[j] == '\\' && j + 1 < s.size()) { j += 2; continue; }
                        if (s[j] == '{') d++;
                        if (s[j] == '}') d--;
                        if (s[j] == '\n') line++;
                        j++;
                    }
                    if (d) throw err("missing '}'");
                    w.push_back(s.substr(i + 1, j - i - 2));
                    i = j;
                } else if (s[i] == '"') {
                    i++;
                    w.push_back(subst(s, i, "\""));
                    if (i >= s.size()) throw err("missing '\"'");
                    i++;
                } else w.push_back(subst(s, i, " \t\r\n;"));
            }
            if (w.empty()) continue;
            int after = line;
            line = startLine;
            result = call(w);
            line = std::max(line, after);
        }
        depth--;
        return result;
    }

    std::string call(std::vector<std::string>& w) {
        auto c = cmds.find(w[0]);
        if (c != cmds.end()) return c->second(w);
        auto p = procs.find(w[0]);
        if (p == procs.end()) throw err("unknown command '" + w[0] + "'");
        return callProc(p->second, w, 1);
    }
    std::string callProc(const Proc& p, std::vector<std::string>& w, size_t first) {
        std::map<std::string, std::string> f;
        size_t given = w.size() - first;
        if (given > p.args.size()) throw err("too many arguments to '" + w[first - 1] + "'");
        for (size_t a = 0; a < p.args.size(); a++) {
            if (a < given) f[p.args[a].first] = w[first + a];
            else if (p.args[a].second.first) f[p.args[a].first] = p.args[a].second.second;
            else throw err("'" + w[first - 1] + "' needs the argument '" + p.args[a].first + "'");
        }
        frames.push_back(std::move(f));
        int saveLine = line;
        std::string r = eval(p.body);
        line = saveLine;
        frames.pop_back();
        if (flow == Return) flow = Normal;
        return r;
    }
    static Proc makeProc(const std::string& args, const std::string& body) {
        Proc p;
        for (const std::string& a : listOf(args)) {
            std::vector<std::string> parts = listOf(a);
            if (parts.size() == 2) p.args.push_back({parts[0], {true, parts[1]}});
            else p.args.push_back({a, {false, ""}});
        }
        p.body = body;
        return p;
    }

    // --- expr ---------------------------------------------------------------
    static bool toInt(const std::string& s, long long& v) {
        std::string t;
        for (char c : s)
            if (!std::isspace((unsigned char)c)) t += c;
        if (t.empty()) return false;
        bool neg = false;
        size_t i = 0;
        if (t[0] == '-' || t[0] == '+') neg = t[0] == '-', i = 1;
        if (i >= t.size()) return false;
        int base = 10;
        if (t.compare(i, 2, "0x") == 0 || t.compare(i, 2, "0X") == 0) base = 16, i += 2;
        else if (t.compare(i, 2, "0b") == 0 || t.compare(i, 2, "0B") == 0) base = 2, i += 2;
        if (i >= t.size()) return false;
        char* end = nullptr;
        unsigned long long u = std::strtoull(t.c_str() + i, &end, base);
        if (*end) return false;
        v = neg ? -(long long)u : (long long)u;
        return true;
    }
    struct Ex {
        Tcl& tcl;
        const std::string& s;
        size_t i = 0;
        void ws() {
            while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
        }
        bool eat(const char* op) {
            ws();
            size_t n = std::strlen(op);
            if (s.compare(i, n, op) != 0) return false;
            // do not take the first half of a longer operator
            if (n == 1 && i + 1 < s.size()) {
                char a = op[0], b = s[i + 1];
                if ((a == '<' && (b == '<' || b == '=')) || (a == '>' && (b == '>' || b == '=')) || (a == '&' && b == '&') ||
                    (a == '|' && b == '|') || (a == '=' && b == '=') || (a == '!' && b == '=') || (a == '*' && b == '*'))
                    return false;
            }
            i += n;
            return true;
        }
        bool word(const char* w) {
            ws();
            size_t n = std::strlen(w);
            if (s.compare(i, n, w) != 0 || (i + n < s.size() && std::isalnum((unsigned char)s[i + n]))) return false;
            i += n;
            return true;
        }
        long long num(const std::string& v) {
            long long n;
            if (!toInt(v, n)) throw tcl.err("expr: '" + v + "' is not a number");
            return n;
        }
        std::string primary() {
            ws();
            if (i >= s.size()) throw tcl.err("expr: unexpected end");
            if (s[i] == '(') {
                i++;
                std::string v = ternary();
                if (!eat(")")) throw tcl.err("expr: missing ')'");
                return v;
            }
            if (s[i] == '-') { i++; return std::to_string(-num(primary())); }
            if (s[i] == '+') { i++; return primary(); }
            if (s[i] == '!') { i++; return std::to_string(!num(primary())); }
            if (s[i] == '~') { i++; return std::to_string(~num(primary())); }
            if (s[i] == '$' || s[i] == '[') {
                size_t j = i;
                if (s[i] == '$') {
                    j++;
                    if (j < s.size() && s[j] == '{') j = s.find('}', j) + 1;
                    else
                        while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
                } else {
                    int d = 1;
                    j++;
                    while (j < s.size() && d) {
                        if (s[j] == '[') d++;
                        if (s[j] == ']') d--;
                        j++;
                    }
                }
                std::string piece = s.substr(i, j - i);
                i = j;
                size_t k = 0;
                if (piece[0] == '[') return tcl.eval(piece.substr(1, piece.size() - 2));
                return tcl.subst(piece, k, "");
            }
            if (s[i] == '"') {
                size_t j = s.find('"', i + 1);
                if (j == std::string::npos) throw tcl.err("expr: missing '\"'");
                std::string piece = s.substr(i + 1, j - i - 1);
                i = j + 1;
                size_t k = 0;
                return tcl.subst(piece, k, "");
            }
            if (s[i] == '{') {
                size_t j = s.find('}', i + 1);
                std::string piece = s.substr(i + 1, j - i - 1);
                i = j + 1;
                return piece;
            }
            size_t j = i;
            while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '.')) j++;
            if (j == i) throw tcl.err(std::string("expr: unexpected '") + s[i] + "'");
            std::string v = s.substr(i, j - i);
            i = j;
            return v;
        }
        std::string binary(int level) {
            static const std::vector<std::vector<const char*>> ops{
                {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!=", "eq", "ne"}, {"<=", ">=", "<", ">"},
                {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"}, {"**"}};
            if (level == (int)ops.size()) return primary();
            std::string l = binary(level + 1);
            while (true) {
                const char* hit = nullptr;
                for (const char* op : ops[level])
                    if (std::isalpha((unsigned char)op[0]) ? word(op) : eat(op)) { hit = op; break; }
                if (!hit) return l;
                std::string r = binary(level + 1), o = hit;
                if (o == "eq") { l = l == r ? "1" : "0"; continue; }
                if (o == "ne") { l = l != r ? "1" : "0"; continue; }
                long long a, b;
                bool na = toInt(l, a), nb = toInt(r, b);
                if ((o == "==" || o == "!=") && (!na || !nb)) {
                    l = ((l == r) == (o == "==")) ? "1" : "0";
                    continue;
                }
                a = num(l), b = num(r);
                long long v = 0;
                if (o == "||") v = a || b;
                else if (o == "&&") v = a && b;
                else if (o == "|") v = a | b;
                else if (o == "^") v = a ^ b;
                else if (o == "&") v = a & b;
                else if (o == "==") v = a == b;
                else if (o == "!=") v = a != b;
                else if (o == "<=") v = a <= b;
                else if (o == ">=") v = a >= b;
                else if (o == "<") v = a < b;
                else if (o == ">") v = a > b;
                else if (o == "<<") v = a << b;
                else if (o == ">>") v = a >> b;
                else if (o == "+") v = a + b;
                else if (o == "-") v = a - b;
                else if (o == "*") v = a * b;
                else if (o == "**") {
                    v = 1;
                    for (long long k = 0; k < b; k++) v *= a;
                } else {
                    if (b == 0) throw tcl.err("expr: division by zero");
                    v = o == "/" ? a / b : a % b;
                }
                l = std::to_string(v);
            }
        }
        std::string ternary() {
            std::string c = binary(0);
            if (!eat("?")) return c;
            std::string a = ternary();
            if (!eat(":")) throw tcl.err("expr: missing ':'");
            std::string b = ternary();
            return num(c) ? a : b;
        }
    };
    std::string expr(const std::string& s) {
        Ex e{*this, s};
        std::string v = e.ternary();
        e.ws();
        if (e.i < s.size()) throw err("expr: unexpected '" + s.substr(e.i) + "'");
        return v;
    }
    bool truth(const std::string& s) {
        std::string v = expr(s);
        long long n;
        if (!toInt(v, n)) throw err("'" + v + "' is not a condition");
        return n != 0;
    }

    Tcl() {
        auto need = [this](std::vector<std::string>& w, size_t lo, size_t hi) {
            if (w.size() < lo || w.size() > hi) throw err("wrong number of arguments to '" + w[0] + "'");
        };
        cmds["set"] = [=, this](std::vector<std::string>& w) {
            need(w, 2, 3);
            if (w.size() == 3) frames.back()[w[1]] = w[2];
            return var(w[1]);
        };
        cmds["unset"] = [=, this](std::vector<std::string>& w) {
            for (size_t i = 1; i < w.size(); i++) frames.back().erase(w[i]);
            return std::string();
        };
        cmds["expr"] = [this](std::vector<std::string>& w) {
            std::string s;
            for (size_t i = 1; i < w.size(); i++) s += (i > 1 ? " " : "") + w[i];
            return expr(s);
        };
        cmds["incr"] = [=, this](std::vector<std::string>& w) {
            need(w, 2, 3);
            long long v, d = 1;
            if (!toInt(var(w[1]), v) || (w.size() == 3 && !toInt(w[2], d))) throw err("incr: not a number");
            return var(w[1]) = std::to_string(v + d);
        };
        cmds["puts"] = [this](std::vector<std::string>& w) {
            *out << w.back() << "\n";
            return std::string();
        };
        cmds["proc"] = [=, this](std::vector<std::string>& w) {
            need(w, 4, 4);
            procs[w[1]] = makeProc(w[2], w[3]);
            return std::string();
        };
        cmds["return"] = [this](std::vector<std::string>& w) {
            flow = Return;
            return w.size() > 1 ? w[1] : std::string();
        };
        cmds["break"] = [this](std::vector<std::string>&) {
            flow = Break;
            return std::string();
        };
        cmds["continue"] = [this](std::vector<std::string>&) {
            flow = Continue;
            return std::string();
        };
        cmds["if"] = [this](std::vector<std::string>& w) {
            size_t i = 1;
            while (i < w.size()) {
                if (i + 1 >= w.size()) throw err("if: missing body");
                bool c = truth(w[i++]);
                if (w[i] == "then") i++;
                if (i >= w.size()) throw err("if: missing body");
                if (c) return eval(w[i]);
                i++;
                if (i >= w.size()) break;
                if (w[i] == "elseif") { i++; continue; }
                if (w[i] == "else") i++;
                if (i >= w.size()) throw err("if: missing else body");
                return eval(w[i]);
            }
            return std::string();
        };
        auto loopBody = [this](const std::string& body) {  // false: leave the loop
            eval(body);
            if (flow == Break) { flow = Normal; return false; }
            if (flow == Continue) flow = Normal;
            return flow == Normal;
        };
        cmds["while"] = [=, this](std::vector<std::string>& w) {
            need(w, 3, 3);
            long guard = 0;
            while (truth(w[1])) {
                if (++guard > 10000000) throw err("while: more than 10 million iterations");
                if (!loopBody(w[2])) break;
            }
            return std::string();
        };
        cmds["for"] = [=, this](std::vector<std::string>& w) {
            need(w, 5, 5);
            eval(w[1]);
            long guard = 0;
            while (truth(w[2])) {
                if (++guard > 10000000) throw err("for: more than 10 million iterations");
                if (!loopBody(w[4])) break;
                eval(w[3]);
            }
            return std::string();
        };
        cmds["foreach"] = [=, this](std::vector<std::string>& w) {
            need(w, 4, 4);
            for (const std::string& v : listOf(w[2])) {
                frames.back()[w[1]] = v;
                if (!loopBody(w[3])) break;
            }
            return std::string();
        };
        cmds["list"] = [](std::vector<std::string>& w) {
            std::string s;
            for (size_t i = 1; i < w.size(); i++) {
                bool brace = w[i].empty() || w[i].find_first_of(" \t\n") != std::string::npos;
                s += (i > 1 ? " " : "") + (brace ? "{" + w[i] + "}" : w[i]);
            }
            return s;
        };
        cmds["llength"] = [=, this](std::vector<std::string>& w) {
            need(w, 2, 2);
            return std::to_string(listOf(w[1]).size());
        };
        cmds["lindex"] = [=, this](std::vector<std::string>& w) {
            need(w, 3, 3);
            long long k;
            auto l = listOf(w[1]);
            if (!toInt(w[2], k)) throw err("lindex: not a number");
            return k >= 0 && k < (long long)l.size() ? l[k] : std::string();
        };
        cmds["lappend"] = [=, this](std::vector<std::string>& w) {
            std::string& v = frames.back()[w.at(1)];
            for (size_t i = 2; i < w.size(); i++) v += (v.empty() ? "" : " ") + w[i];
            return v;
        };
        cmds["string"] = [=, this](std::vector<std::string>& w) {
            if (w.size() == 3 && w[1] == "length") return std::to_string(w[2].size());
            if (w.size() == 4 && w[1] == "equal") return std::string(w[2] == w[3] ? "1" : "0");
            if (w.size() == 3 && w[1] == "tolower") return lower(w[2]);
            if (w.size() == 3 && w[1] == "toupper") return upper(w[2]);
            throw err("string: only length, equal, tolower and toupper are supported");
        };
        cmds["format"] = [=, this](std::vector<std::string>& w) {
            if (w.size() < 2) throw err("format: missing format");
            std::string out_, f = w[1];
            size_t arg = 2;
            for (size_t i = 0; i < f.size(); i++) {
                if (f[i] != '%') { out_ += f[i]; continue; }
                size_t j = i + 1;
                while (j < f.size() && !std::isalpha((unsigned char)f[j]) && f[j] != '%') j++;
                if (j >= f.size()) throw err("format: bad format");
                if (f[j] == '%') { out_ += '%'; i = j; continue; }
                if (arg >= w.size()) throw err("format: not enough arguments");
                std::string spec = f.substr(i, j - i);
                char buf[256];
                if (f[j] == 's') std::snprintf(buf, sizeof buf, (spec + "s").c_str(), w[arg].c_str());
                else if (f[j] == 'b') {
                    long long v;
                    if (!toInt(w[arg], v)) throw err("format: not a number");
                    std::string b;
                    for (unsigned long long u = (unsigned long long)v; u; u >>= 1) b.insert(b.begin(), (char)('0' + (u & 1)));
                    int width = std::atoi(spec.c_str() + (spec.size() > 1 && spec[1] == '0' ? 2 : 1));
                    while ((int)b.size() < std::max(width, 1)) b.insert(b.begin(), '0');
                    std::snprintf(buf, sizeof buf, "%s", b.c_str());
                } else {
                    long long v;
                    if (!toInt(w[arg], v)) throw err("format: not a number");
                    std::snprintf(buf, sizeof buf, (spec + "ll" + f[j]).c_str(), v);
                }
                out_ += buf;
                arg++;
                i = j;
            }
            return out_;
        };
    }
};

// Runs PDL against an elaborated network and builds the pattern.
struct Retargeter {
    icl::Network& net;
    Pattern pat;
    bool tap = false;
    Tcl tcl;
    // Pending per register, by pos: 'x' = nothing.
    std::vector<std::string> wr, rd;
    std::map<std::string, std::map<std::string, Tcl::Proc>> iprocs;  // module -> name -> proc
    std::string procModule;
    icl::Ctx* ctx = nullptr;
    std::string prefix;
    bool irLoaded = false;
    size_t rtlPins = 0;
    // statistics
    int scans = 0, applies = 0;
    uint64_t scanBits = 0;

    explicit Retargeter(icl::Network& n, bool viaTap) : net(n), tap(viaTap) {
        wr.resize(net.regs.size());
        rd.resize(net.regs.size());
        for (size_t i = 0; i < net.regs.size(); i++) wr[i].assign(net.regs[i].width, 'x'), rd[i] = wr[i];
        net.pinValue.resize(net.pins.size());
        for (size_t i = 0; i < net.pins.size(); i++) net.pinValue[i].assign(net.pins[i].width, '0');
        rtlPins = net.pins.size();
        ctx = net.root.get();
        doReset();
        install();
    }

    void doReset() {
        pat.reset();
        net.resetState();
        irLoaded = false;
        for (size_t i = 0; i < wr.size(); i++) wr[i].assign(wr[i].size(), 'x'), rd[i].assign(rd[i].size(), 'x');
    }

    // "a.b.R[3:0]" -> the data bits it names, in the current instance.
    icl::DVec target(const std::string& text, std::string* enumReg = nullptr) {
        std::string name = prefix.empty() ? text : prefix + "." + text;
        icl::Expr e;
        e.k = icl::Expr::Ref;
        size_t br = name.find('[');
        e.name = name.substr(0, br);
        if (br != std::string::npos) {
            size_t colon = name.find(':', br), close = name.find(']', br);
            if (close == std::string::npos) throw tcl.err("'" + text + "': missing ']'");
            e.hasIdx = true;
            e.hi = e.lo = std::atoi(name.c_str() + br + 1);
            if (colon != std::string::npos && colon < close) e.lo = std::atoi(name.c_str() + colon + 1);
        }
        if (enumReg) *enumReg = e.name;
        try {
            return net.data(*ctx, e);
        } catch (const Error& x) {
            throw tcl.err(x.what());
        }
    }
    // A PDL value -> bits by significance, `width` wide.
    std::string value(const std::string& v, int width, const icl::DVec& bits) {
        std::string out;
        if (v.size() > 2 && v[0] == '0' && (v[1] == 'b' || v[1] == 'B')) {
            for (auto it = v.rbegin(); it != v.rend() - 2; ++it)
                if (*it != '_') out += (*it == '0' || *it == '1') ? *it : 'x';
        } else if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
            for (auto it = v.rbegin(); it != v.rend() - 2; ++it) {
                char c = (char)std::tolower((unsigned char)*it);
                if (c == '_') continue;
                if (c == 'x') out += "xxxx";
                else if (std::isxdigit((unsigned char)c)) out += bitsOf((uint64_t)(std::isdigit((unsigned char)c) ? c - '0' : c - 'a' + 10), 4);
                else throw tcl.err("'" + v + "' is not a number");
            }
        } else if (!v.empty() && (std::isdigit((unsigned char)v[0]) || v[0] == '\'')) {
            for (char c : v)
                if (!std::isalnum((unsigned char)c) && c != '\'' && c != '_') throw tcl.err("'" + v + "' is not a number");
            out = icl::parseNumber(v).bits;
        } else {
            // An enum symbol of the register the target belongs to.
            const icl::Number* found = nullptr;
            for (const icl::DBit& b : bits) {
                if (b.k != icl::DBit::Reg) continue;
                const icl::FReg& r = net.regs[b.id];
                for (auto& [en, syms] : r.mod->enums) {
                    if (!r.enumName.empty() && en != r.enumName) continue;
                    auto it = syms.find(v);
                    if (it != syms.end()) found = &it->second;
                }
                if (found) break;
            }
            if (!found) throw tcl.err("'" + v + "' is not a number or an enum value of the target");
            out = found->bits;
        }
        for (size_t i = width; i < out.size(); i++)
            if (out[i] == '1') throw tcl.err("the value " + v + " does not fit in " + std::to_string(width) + " bits");
        out.resize(width, '0');
        return out;
    }
    std::string pinSig(int pin, int bit) const {
        return icl::Network::ident(net.pins[pin].name) + "[" + std::to_string(bit) + "]";
    }
    void setPin(int pin, int bit, char v) {
        if (net.pinValue[pin][bit] == v) return;
        net.pinValue[pin][bit] = v;
        pat.setPin(pinSig(pin, bit), std::string("1'b") + v);
    }

    void iWrite(const std::string& name, const std::string& val) {
        icl::DVec bits = target(name);
        std::string v = value(val, (int)bits.size(), bits);
        for (size_t i = 0; i < bits.size(); i++) {
            const icl::DBit& b = bits[i];
            if (v[i] == 'x') continue;
            if (b.k == icl::DBit::Reg) wr[b.id][b.bit] = v[i];
            else if (b.k == icl::DBit::Pin && !net.pins[b.id].instrument && b.id < (int)rtlPins) setPin(b.id, b.bit, v[i]);
            else if ((b.k == icl::DBit::C0 && v[i] == '0') || (b.k == icl::DBit::C1 && v[i] == '1')) continue;
            else throw tcl.err("iWrite " + name + ": bit " + std::to_string(i) + " is not driven by a scan register");
        }
    }
    void iRead(const std::string& name, const std::string& val) {
        icl::DVec bits = target(name);
        std::string v = val.empty() ? std::string(bits.size(), 'x') : value(val, (int)bits.size(), bits);
        for (size_t i = 0; i < bits.size(); i++) {
            const icl::DBit& b = bits[i];
            if (v[i] == 'x') continue;
            if (b.k == icl::DBit::Reg) {
                rd[b.id][b.bit] = v[i];
                // The instrument model: what the register captures shows the value.
                const icl::FReg& r = net.regs[b.id];
                if (r.hasCapture && r.capture[b.bit].k == icl::DBit::Pin) setPin(r.capture[b.bit].id, r.capture[b.bit].bit, v[i]);
            } else if (b.k == icl::DBit::Pin && b.id < (int)rtlPins) {
                auto it = net.pinCapture.find({b.id, b.bit});
                if (it == net.pinCapture.end()) throw tcl.err("iRead " + name + ": no scan register captures bit " + std::to_string(i));
                setPin(b.id, b.bit, v[i]);
                for (auto [reg, pos] : it->second) rd[reg][pos] = v[i];
            } else throw tcl.err("iRead " + name + ": bit " + std::to_string(i) + " is not captured by a scan register");
        }
    }

    // The mux rows that put register `t` on the scan path. Prefers the rows
    // selected now.
    bool route(icl::ScanRef at, int t, std::vector<std::pair<int, int>>& need, std::set<std::pair<int, int>>& dead, int guard = 0) {
        while (true) {
            if (guard++ > (int)(net.regs.size() + net.muxes.size()) * 2 + 8) return false;
            if (at.k == icl::ScanRef::Reg) {
                if (at.id == t) return true;
                if (dead.count({0, at.id})) return false;
                at = net.regs[at.id].si;
                continue;
            }
            if (at.k != icl::ScanRef::Mux) return false;
            if (dead.count({1, at.id})) return false;
            const icl::FMux& m = net.muxes[at.id];
            int cur = net.selected(m);
            std::vector<int> order;
            if (cur >= 0) order.push_back(cur);
            for (int r = 0; r < (int)m.rows.size(); r++)
                if (r != cur) order.push_back(r);
            for (int r : order) {
                size_t mark = need.size();
                need.push_back({at.id, r});
                if (route(m.rows[r].second, t, need, dead, guard)) return true;
                need.resize(mark);
            }
            dead.insert({1, at.id});
            return false;
        }
    }
    // Make `bits` equal `pat`: collect register-bit assignments.
    bool solve(const icl::DVec& bits, const std::string& pattern, std::map<std::pair<int, int>, char>& want) {
        for (size_t i = 0; i < bits.size() && i < pattern.size(); i++) {
            if (pattern[i] == 'x') continue;
            icl::DBit b = bits[i];
            char v = pattern[i];
            while (b.k == icl::DBit::Ex && !net.exprs[b.id].dmux && net.exprs[b.id].op == icl::Expr::Not) {
                b = net.exprs[b.id].args[0][b.bit];
                v = v == '0' ? '1' : '0';
            }
            if (b.k == icl::DBit::Reg) {
                auto [it, fresh] = want.emplace(std::make_pair(b.id, b.bit), v);
                if (!fresh && it->second != v) return false;
            } else if (b.k == icl::DBit::C0 || b.k == icl::DBit::C1) {
                if ((b.k == icl::DBit::C1) != (v == '1')) return false;
            } else if (b.k == icl::DBit::Pin && !net.pins[b.id].instrument && b.id < (int)rtlPins) setPin(b.id, b.bit, v);
            else if (net.evalBit(b) != v) return false;
        }
        return true;
    }

    void iApply() {
        applies++;
        size_t limit = net.regs.size() + net.muxes.size() + 4;
        for (size_t round = 0;; round++) {
            std::vector<int> open;
            for (size_t r = 0; r < wr.size(); r++)
                if (anyCare(wr[r]) || anyCare(rd[r])) open.push_back((int)r);
            if (open.empty()) break;
            if (round > limit) throw tcl.err("iApply: the scan network did not settle");
            std::vector<int> path = net.activePath();
            std::vector<char> on(net.regs.size(), 0);
            for (int r : path) on[r] = 1;
            // What each on-path register should hold after this scan.
            std::vector<std::string> desired(net.regs.size());
            for (int r : path) {
                desired[r] = net.regs[r].value;
                for (size_t b = 0; b < desired[r].size(); b++) {
                    if (wr[r][b] != 'x') desired[r][b] = wr[r][b];
                    else if (desired[r][b] == 'x') desired[r][b] = '0';
                }
            }
            bool progress = false;
            for (int r : path)
                if (anyCare(wr[r]) || anyCare(rd[r])) progress = true;
            // Steer the muxes toward the targets that are off the path.
            std::set<std::pair<int, int>> claimed;  // bits a target's own write holds
            for (int t : open) {
                if (on[t]) continue;
                std::vector<std::pair<int, int>> need;
                std::set<std::pair<int, int>> dead;
                if (!route(net.so, t, need, dead))
                    throw tcl.err("iApply: no scan path reaches " + net.regs[t].name);
                for (auto [mux, row] : need) {
                    const icl::FMux& m = net.muxes[mux];
                    if (net.selected(m) == row) continue;
                    std::map<std::pair<int, int>, char> want;
                    if (!solve(m.sel, m.rows[row].first, want))
                        throw tcl.err("iApply: cannot steer ScanMux " + m.name + " toward " + net.regs[t].name);
                    for (auto& [bit, v] : want) {
                        auto [reg, pos] = bit;
                        if (!on[reg]) continue;  // a deeper level: a later round
                        if (wr[reg][pos] != 'x' && wr[reg][pos] != v) continue;  // the user's write first
                        if (desired[reg][pos] != v) desired[reg][pos] = v, progress = true;
                    }
                }
            }
            if (!progress) {
                std::string names;
                for (int t : open)
                    if (!on[t]) names += (names.empty() ? "" : ", ") + net.regs[t].name;
                throw tcl.err("iApply: cannot bring " + names + " onto the scan path");
            }
            // One capture-shift-update.
            std::string tdi, tdo;
            for (int r : path) {
                tdi += desired[r];
                tdo += rd[r];
            }
            if (tap && !irLoaded) {
                pat.ir(fromMsbFirst("0010"));
                irLoaded = true;
            }
            pat.dr(tdi, tdo);
            scans++, scanBits += tdi.size();
            for (int r : path) {
                net.regs[r].value = desired[r];
                wr[r].assign(wr[r].size(), 'x');
                rd[r].assign(rd[r].size(), 'x');
            }
        }
        // What the instruments now see.
        for (const icl::FObs& o : net.obs) {
            std::string v = net.eval(o.bits);
            if (v.find('x') == std::string::npos) pat.expPin(icl::Network::ident(o.name), vlog(v));
        }
    }

    void install() {
        auto ignore = [](std::vector<std::string>&) { return std::string(); };
        for (const char* c : {"iMerge", "iTake", "iRelease", "iClock", "iClockOverride", "iPDLLevel", "iUseProcNameSpace"})
            tcl.cmds[c] = ignore;
        tcl.cmds["iScan"] = [this](std::vector<std::string>&) -> std::string { throw tcl.err("iScan is not supported"); };
        tcl.cmds["iProcsForModule"] = [this](std::vector<std::string>& w) {
            if (w.size() != 2) throw tcl.err("iProcsForModule needs a module name");
            procModule = w[1];
            size_t ns = procModule.rfind("::");
            if (ns != std::string::npos) procModule = procModule.substr(ns + 2);
            if (!net.lib.modules.count(procModule)) throw tcl.err("iProcsForModule: no ICL Module " + procModule);
            return std::string();
        };
        tcl.cmds["iProc"] = [this](std::vector<std::string>& w) {
            if (w.size() != 4) throw tcl.err("iProc needs a name, an argument list and a body");
            if (procModule.empty()) throw tcl.err("iProc " + w[1] + " before any iProcsForModule");
            iprocs[procModule][w[1]] = Tcl::makeProc(w[2], w[3]);
            return std::string();
        };
        tcl.cmds["iCall"] = [this](std::vector<std::string>& w) {
            if (w.size() < 2) throw tcl.err("iCall needs a procedure");
            return iCall(w, 1);
        };
        tcl.cmds["iWrite"] = [this](std::vector<std::string>& w) {
            if (w.size() != 3) throw tcl.err("iWrite needs a target and a value");
            iWrite(w[1], w[2]);
            return std::string();
        };
        tcl.cmds["iRead"] = [this](std::vector<std::string>& w) {
            if (w.size() != 2 && w.size() != 3) throw tcl.err("iRead needs a target and an optional value");
            iRead(w[1], w.size() == 3 ? w[2] : "");
            return std::string();
        };
        tcl.cmds["iApply"] = [this](std::vector<std::string>&) {
            iApply();
            return std::string();
        };
        tcl.cmds["iReset"] = [this](std::vector<std::string>&) {
            doReset();
            return std::string();
        };
        tcl.cmds["iRunLoop"] = [this](std::vector<std::string>& w) {
            long long n = 0;
            for (size_t i = 1; i < w.size(); i++)
                if (w[i][0] != '-' && !Tcl::toInt(w[i], n)) throw tcl.err("iRunLoop: '" + w[i] + "' is not a count");
            if (n < 1) throw tcl.err("iRunLoop needs a cycle count");
            pat.run((int)n);
            return std::string();
        };
        tcl.cmds["iNote"] = [this](std::vector<std::string>& w) {
            pat.note(w.back());
            return std::string();
        };
        tcl.cmds["iPrefix"] = [this](std::vector<std::string>& w) {
            prefix = w.size() > 1 && w[1] != "-" ? w[1] : "";
            return std::string();
        };
    }

    std::string iCall(std::vector<std::string>& w, size_t at) {
        std::string full = w[at], inst, name = full;
        size_t dot = full.rfind('.');
        if (dot != std::string::npos) inst = full.substr(0, dot), name = full.substr(dot + 1);
        icl::Ctx* c = ctx;
        std::string path = inst;
        while (!path.empty()) {
            size_t d = path.find('.');
            std::string head = path.substr(0, d);
            auto it = c->kids.find(head);
            if (it == c->kids.end()) throw tcl.err("iCall " + full + ": no instance '" + head + "' in " + c->mod->name);
            c = it->second.get();
            path = d == std::string::npos ? "" : path.substr(d + 1);
        }
        auto m = iprocs.find(c->mod->name);
        if (m == iprocs.end() || !m->second.count(name))
            throw tcl.err("iCall " + full + ": Module " + c->mod->name + " has no iProc " + name);
        icl::Ctx* saveCtx = ctx;
        std::string savePrefix = prefix;
        ctx = c, prefix.clear();
        std::vector<std::string> args(w.begin() + at, w.end());
        std::string r = tcl.callProc(m->second.at(name), args, 1);
        ctx = saveCtx, prefix = savePrefix;
        return r;
    }

    void run(const std::string& text, const std::string& file) {
        tcl.file = file, tcl.line = 1;
        tcl.eval(text);
        tcl.flow = Tcl::Normal;
    }
    // Flush what the last iApply did not cover, and end in reset.
    void finish() {
        bool open = false;
        for (size_t r = 0; r < wr.size(); r++)
            if (anyCare(wr[r]) || anyCare(rd[r])) open = true;
        if (open) iApply();
    }
};

}  // namespace dft
