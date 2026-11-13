// vhdl/lex.hpp - VHDL lexer for msim's VHDL front end (IEEE 1076-2008 lexical
// elements, 15.3 to 15.9). Identifiers are case-insensitive: `low` keeps the
// lower-cased spelling for lookup and `text` the spelling as written, so the
// generated SystemVerilog keeps the user's names.
#pragma once

#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vhdl {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class Tok {
    End,
    Id,        // identifier or reserved word (low tells which)
    ExtId,     // \extended identifier\  .
    Int,       // abstract literal without a point or a based literal with integer value
    Real,      // abstract literal with a point
    Char,      // 'x'
    Str,       // "..."
    BitStr,    // x"FF", 8b"1010", ...
    Op,        // delimiters, compound delimiters
};

struct Token {
    Tok kind = Tok::End;
    std::string text;   // as written (strings: contents without quotes; bit strings: expanded binary digits)
    std::string low;    // lower case (identifiers, operators)
    double real = 0;
    int64_t ival = 0;
    int line = 0, col = 0;
};

inline std::string lowerStr(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

inline bool isReserved(const std::string& w) {
    static const char* kw[] = {
        "abs", "access", "after", "alias", "all", "and", "architecture", "array", "assert", "assume",
        "attribute", "begin", "block", "body", "buffer", "bus", "case", "component", "configuration",
        "constant", "context", "cover", "default", "disconnect", "downto", "else", "elsif", "end",
        "entity", "exit", "fairness", "file", "for", "force", "function", "generate", "generic", "group",
        "guarded", "if", "impure", "in", "inertial", "inout", "is", "label", "library", "linkage",
        "literal", "loop", "map", "mod", "nand", "new", "next", "nor", "not", "null", "of", "on", "open",
        "or", "others", "out", "package", "parameter", "port", "postponed", "procedure", "process",
        "property", "protected", "private", "pure", "range", "record", "register", "reject", "release",
        "rem", "report", "restrict", "return", "rol", "ror", "select", "sequence", "severity",
        "shared", "signal", "sla", "sll", "sra", "srl", "strong", "subtype", "then", "to", "transport",
        "type", "unaffected", "units", "until", "use", "variable", "view", "vmode", "vprop", "vunit",
        "wait", "when", "while", "with", "xnor", "xor"};
    for (auto* k : kw)
        if (w == k) return true;
    return false;
}

class Lexer {
public:
    Lexer(std::string_view src, std::string file) : s(src), file(std::move(file)) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        for (;;) {
            Token t = next(out.empty() ? nullptr : &out.back());
            out.push_back(t);
            if (t.kind == Tok::End) break;
        }
        return out;
    }

private:
    std::string_view s;
    std::string file;
    size_t i = 0;
    int line = 1;
    size_t lineStart = 0;

    [[noreturn]] void fail(const std::string& m) {
        throw Error(file + ":" + std::to_string(line) + ": " + m);
    }
    char peek(size_t k = 0) const { return i + k < s.size() ? s[i + k] : '\0'; }

    void skipSpace() {
        for (;;) {
            char c = peek();
            if (c == '\n') { ++line; ++i; lineStart = i; }
            else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v' || (unsigned char)c == 0xA0) ++i;
            else if (c == '-' && peek(1) == '-') { while (i < s.size() && s[i] != '\n') ++i; }
            else if (c == '/' && peek(1) == '*') {
                i += 2;
                while (i < s.size() && !(s[i] == '*' && peek(1) == '/')) {
                    if (s[i] == '\n') { ++line; lineStart = i + 1; }
                    ++i;
                }
                i += 2;
            } else if ((unsigned char)c == 0xC2 && (unsigned char)peek(1) == 0xA0) i += 2;   // UTF-8 NBSP
            else break;
        }
    }

    static int digitVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        c = (char)std::tolower((unsigned char)c);
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 99;
    }

    // 15.8: bit string literal. Expands to a string of bit characters
    // ('0','1', or the graphic character as written for non-digits).
    std::string expandBits(std::string base, const std::string& digits, int64_t size, bool& ok) {
        ok = true;
        bool sgn = false;
        if (base.size() == 2 && (base[0] == 'u' || base[0] == 's')) { sgn = base[0] == 's'; base = base.substr(1); }
        std::string r;
        if (base == "d") {
            // decimal: the value in binary, minimal length (or `size`)
            uint64_t v = 0;
            for (char c : digits) {
                if (c == '_') continue;
                if (!std::isdigit((unsigned char)c)) { ok = false; return r; }
                v = v * 10 + (c - '0');
            }
            while (v) { r.insert(r.begin(), char('0' + (v & 1))); v >>= 1; }
            if (r.empty()) r = "0";
        } else {
            int bits = base == "b" ? 1 : base == "o" ? 3 : base == "x" ? 4 : 0;
            if (!bits) { ok = false; return r; }
            for (char c : digits) {
                if (c == '_') continue;
                int d = digitVal(c);
                if (d < (1 << bits)) {
                    for (int k = bits - 1; k >= 0; --k) r.push_back(char('0' + ((d >> k) & 1)));
                } else {
                    for (int k = 0; k < bits; ++k) r.push_back((char)std::toupper((unsigned char)c));
                }
            }
        }
        if (size >= 0) {
            while ((int64_t)r.size() < size) r.insert(r.begin(), sgn && !r.empty() ? r[0] : '0');
            while ((int64_t)r.size() > size) r.erase(r.begin());
        }
        return r;
    }

    Token next(const Token* prev) {
        skipSpace();
        Token t;
        t.line = line;
        t.col = (int)(i - lineStart) + 1;
        if (i >= s.size()) return t;
        char c = peek();
        // extended identifier
        if (c == '\\') {
            size_t j = i + 1;
            std::string v;
            while (j < s.size()) {
                if (s[j] == '\\') {
                    if (j + 1 < s.size() && s[j + 1] == '\\') { v.push_back('\\'); j += 2; continue; }
                    break;
                }
                v.push_back(s[j++]);
            }
            i = j + 1;
            t.kind = Tok::ExtId;
            t.text = v;
            t.low = "\\" + v + "\\";
            return t;
        }
        // VHDL-2008 sized bit string: digits followed by base and quote, e.g. 8x"FF"
        if (std::isdigit((unsigned char)c)) {
            size_t j = i;
            while (j < s.size() && std::isdigit((unsigned char)s[j])) ++j;
            size_t k = j;
            std::string b;
            while (k < s.size() && std::isalpha((unsigned char)s[k]) && b.size() < 2) b.push_back((char)std::tolower((unsigned char)s[k++]));
            if (!b.empty() && k < s.size() && s[k] == '"' &&
                (b == "b" || b == "o" || b == "x" || b == "d" || b == "ub" || b == "uo" || b == "ux" || b == "sb" || b == "so" || b == "sx")) {
                int64_t size = std::stoll(std::string(s.substr(i, j - i)));
                i = k;
                return bitString(t, b, size);
            }
            return number(t);
        }
        if (std::isalpha((unsigned char)c)) {
            size_t j = i;
            while (j < s.size() && (std::isalnum((unsigned char)s[j]) || s[j] == '_')) ++j;
            std::string w(s.substr(i, j - i));
            std::string lw = lowerStr(w);
            if (j < s.size() && s[j] == '"' && (lw == "b" || lw == "o" || lw == "x" || lw == "d" || lw == "ub" || lw == "uo" ||
                                                 lw == "ux" || lw == "sb" || lw == "so" || lw == "sx")) {
                i = j;
                return bitString(t, lw, -1);
            }
            i = j;
            t.kind = Tok::Id;
            t.text = w;
            t.low = lw;
            return t;
        }
        if (c == '"' || (c == '%' && false)) {
            ++i;
            std::string v;
            for (;;) {
                if (i >= s.size() || s[i] == '\n') fail("unterminated string literal");
                if (s[i] == '"') {
                    if (peek(1) == '"') { v.push_back('"'); i += 2; continue; }
                    ++i;
                    break;
                }
                v.push_back(s[i++]);
            }
            t.kind = Tok::Str;
            t.text = v;
            return t;
        }
        if (c == '\'') {
            // character literal unless this is an attribute tick / qualified expression
            bool tick = prev && (prev->kind == Tok::Id || prev->kind == Tok::ExtId || prev->low == ")" || prev->low == "]" || prev->kind == Tok::Str) &&
                        !(prev->kind == Tok::Id && isReserved(prev->low) && prev->low != "all" && prev->low != "range" && prev->low != "subtype");
            if (!tick && i + 2 < s.size() && s[i + 2] == '\'') {
                t.kind = Tok::Char;
                t.text = std::string(1, s[i + 1]);
                i += 3;
                return t;
            }
            ++i;
            t.kind = Tok::Op;
            t.text = t.low = "'";
            return t;
        }
        static const char* ops3[] = {"?/=", "?<=", "?>=", nullptr};
        static const char* ops2[] = {"=>", "**", ":=", "/=", ">=", "<=", "<>", "??", "?=", "?<", "?>", "<<", ">>", nullptr};
        for (int k = 0; ops3[k]; ++k)
            if (s.substr(i, 3) == ops3[k]) { i += 3; t.kind = Tok::Op; t.text = t.low = ops3[k]; return t; }
        for (int k = 0; ops2[k]; ++k)
            if (s.substr(i, 2) == ops2[k]) { i += 2; t.kind = Tok::Op; t.text = t.low = ops2[k]; return t; }
        if (std::string_view("&'()*+,-./:;<=>|[]?@^`!").find(c) != std::string_view::npos) {
            ++i;
            t.kind = Tok::Op;
            t.text = t.low = std::string(1, c == '!' ? '|' : c);
            return t;
        }
        fail(std::string("unexpected character '") + c + "'");
    }

    Token bitString(Token& t, const std::string& base, int64_t size) {
        ++i;   // opening quote
        std::string d;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\n') fail("unterminated bit string literal");
            d.push_back(s[i++]);
        }
        ++i;
        bool ok;
        t.kind = Tok::BitStr;
        t.text = expandBits(base, d, size, ok);
        if (!ok) fail("bad bit string literal " + base + "\"" + d + "\"");
        return t;
    }

    Token number(Token& t) {
        size_t j = i;
        auto digits = [&](bool hex) {
            std::string r;
            while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == '_' || (hex && std::isxdigit((unsigned char)s[j])))) {
                if (s[j] != '_') r.push_back(s[j]);
                ++j;
            }
            return r;
        };
        std::string intPart = digits(false);
        if (j < s.size() && (s[j] == '#' || s[j] == ':') && j + 1 < s.size() && std::isxdigit((unsigned char)s[j + 1])) {
            char delim = s[j];
            int base = std::stoi(intPart);
            ++j;
            std::string ip = digits(true), fp;
            bool isReal = false;
            if (j < s.size() && s[j] == '.') { ++j; fp = digits(true); isReal = true; }
            if (j < s.size() && s[j] == delim) ++j;
            int64_t e = 0;
            if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
                ++j;
                bool neg = false;
                if (s[j] == '+') ++j;
                else if (s[j] == '-') { neg = true; ++j; }
                e = std::stoll(digits(false));
                if (neg) e = -e;
            }
            double v = 0;
            for (char ch : ip) v = v * base + digitVal(ch);
            double f = 1.0 / base;
            for (char ch : fp) { v += digitVal(ch) * f; f /= base; }
            for (int64_t k = 0; k < (e < 0 ? -e : e); ++k) v = e < 0 ? v / base : v * base;
            i = j;
            if (isReal) { t.kind = Tok::Real; t.real = v; }
            else { t.kind = Tok::Int; t.ival = (int64_t)v; t.real = v; }
            t.text = std::string(s.substr(t.col - 1 + lineStart, 0));
            return t;
        }
        bool isReal = false;
        std::string num = intPart;
        if (j + 1 < s.size() && s[j] == '.' && std::isdigit((unsigned char)s[j + 1])) {
            ++j;
            num += "." + digits(false);
            isReal = true;
        }
        if (j < s.size() && (s[j] == 'e' || s[j] == 'E') &&
            (std::isdigit((unsigned char)peekAt(j + 1)) || ((peekAt(j + 1) == '+' || peekAt(j + 1) == '-') && std::isdigit((unsigned char)peekAt(j + 2))))) {
            num.push_back('e');
            ++j;
            if (s[j] == '+' || s[j] == '-') num.push_back(s[j++]);
            num += digits(false);
        }
        i = j;
        t.text = num;
        if (isReal) { t.kind = Tok::Real; t.real = std::stod(num); }
        else {
            t.kind = Tok::Int;
            double d = std::stod(num);
            t.real = d;
            t.ival = (int64_t)d;
        }
        return t;
    }
    char peekAt(size_t k) const { return k < s.size() ? s[k] : '\0'; }
};

}  // namespace vhdl
