// json.hpp - the small JSON reader mvplat's platform files use.
//
// Objects, arrays, strings, numbers, true/false/null. Numbers keep their text
// so that "0x80000000" written as a string and 2147483648 written as a number
// both reach the platform as the same address (see Value::u64). Comments
// (// and /* */) are accepted: a platform file is written by hand.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace vp::json {

struct Value {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    std::string s;  // a string's text, or a number's spelling
    std::vector<Value> a;
    std::vector<std::pair<std::string, Value>> o;  // in file order
    int line = 0;

    bool has(const std::string& k) const {
        for (auto& [n, v] : o) if (n == k) return true;
        return false;
    }
    const Value* get(const std::string& k) const {
        for (auto& [n, v] : o) if (n == k) return &v;
        return nullptr;
    }
    // A number or a string holding one: decimal, 0x hex, 0b binary, with an
    // optional K/M/G suffix (powers of 1024) for sizes.
    uint64_t u64() const {
        if (kind == Bool) return b;
        if (kind != Num && kind != Str) throw std::runtime_error(where() + "expected a number");
        std::string t = s;
        uint64_t mul = 1;
        if (!t.empty()) {
            char c = t.back();
            if (c == 'K' || c == 'k') mul = 1ull << 10;
            else if (c == 'M' || c == 'm') mul = 1ull << 20;
            else if (c == 'G' || c == 'g') mul = 1ull << 30;
            if (mul != 1) t.pop_back();
        }
        size_t pos = 0;
        uint64_t v;
        try {
            if (t.size() > 2 && t[0] == '0' && (t[1] == 'b' || t[1] == 'B'))
                v = std::stoull(t.substr(2), &pos, 2), pos += 2;
            else
                v = std::stoull(t, &pos, 0);
        } catch (...) {
            throw std::runtime_error(where() + "'" + s + "' is not a number");
        }
        if (pos != t.size()) throw std::runtime_error(where() + "'" + s + "' is not a number");
        return v * mul;
    }
    double num() const {
        if (kind != Num && kind != Str) throw std::runtime_error(where() + "expected a number");
        try { return std::stod(s); } catch (...) {
            throw std::runtime_error(where() + "'" + s + "' is not a number");
        }
    }
    const std::string& str() const {
        if (kind != Str) throw std::runtime_error(where() + "expected a string");
        return s;
    }
    std::string where() const { return line ? "line " + std::to_string(line) + ": " : ""; }
};

class Parser {
public:
    explicit Parser(const std::string& t) : t_(t) {}
    Value parse() {
        Value v = value();
        ws();
        if (p_ != t_.size()) fail("text after the end of the document");
        return v;
    }

private:
    const std::string& t_;
    size_t p_ = 0;
    int line_ = 1;

    [[noreturn]] void fail(const std::string& m) {
        throw std::runtime_error("line " + std::to_string(line_) + ": " + m);
    }
    void ws() {
        while (p_ < t_.size()) {
            char c = t_[p_];
            if (c == '\n') { line_++; p_++; }
            else if (c == ' ' || c == '\t' || c == '\r') p_++;
            else if (c == '/' && p_ + 1 < t_.size() && t_[p_ + 1] == '/') {
                while (p_ < t_.size() && t_[p_] != '\n') p_++;
            } else if (c == '/' && p_ + 1 < t_.size() && t_[p_ + 1] == '*') {
                p_ += 2;
                while (p_ + 1 < t_.size() && !(t_[p_] == '*' && t_[p_ + 1] == '/')) {
                    if (t_[p_] == '\n') line_++;
                    p_++;
                }
                p_ += 2;
            } else break;
        }
    }
    Value value() {
        ws();
        if (p_ >= t_.size()) fail("unexpected end of the document");
        Value v;
        v.line = line_;
        char c = t_[p_];
        if (c == '{') {
            v.kind = Value::Obj;
            p_++;
            ws();
            if (p_ < t_.size() && t_[p_] == '}') { p_++; return v; }
            for (;;) {
                ws();
                if (p_ >= t_.size() || t_[p_] != '"') fail("expected a key in quotes");
                std::string k = string();
                ws();
                if (p_ >= t_.size() || t_[p_] != ':') fail("expected ':' after \"" + k + "\"");
                p_++;
                v.o.emplace_back(k, value());
                ws();
                if (p_ < t_.size() && t_[p_] == ',') { p_++; ws(); if (p_ < t_.size() && t_[p_] == '}') { p_++; return v; } continue; }
                if (p_ < t_.size() && t_[p_] == '}') { p_++; return v; }
                fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.kind = Value::Arr;
            p_++;
            ws();
            if (p_ < t_.size() && t_[p_] == ']') { p_++; return v; }
            for (;;) {
                v.a.push_back(value());
                ws();
                if (p_ < t_.size() && t_[p_] == ',') { p_++; ws(); if (p_ < t_.size() && t_[p_] == ']') { p_++; return v; } continue; }
                if (p_ < t_.size() && t_[p_] == ']') { p_++; return v; }
                fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.kind = Value::Str; v.s = string(); return v; }
        if (t_.compare(p_, 4, "true") == 0) { v.kind = Value::Bool; v.b = true; p_ += 4; return v; }
        if (t_.compare(p_, 5, "false") == 0) { v.kind = Value::Bool; p_ += 5; return v; }
        if (t_.compare(p_, 4, "null") == 0) { p_ += 4; return v; }
        size_t s = p_;
        while (p_ < t_.size() && (isalnum((unsigned char)t_[p_]) || t_[p_] == '-' || t_[p_] == '+' ||
                                  t_[p_] == '.' || t_[p_] == '_'))
            p_++;
        if (s == p_) fail(std::string("unexpected '") + c + "'");
        v.kind = Value::Num;
        v.s = t_.substr(s, p_ - s);
        return v;
    }
    std::string string() {
        p_++;  // the opening quote
        std::string r;
        while (p_ < t_.size() && t_[p_] != '"') {
            char c = t_[p_++];
            if (c == '\n') line_++;
            if (c != '\\') { r += c; continue; }
            if (p_ >= t_.size()) break;
            char e = t_[p_++];
            switch (e) {
                case 'n': r += '\n'; break;
                case 't': r += '\t'; break;
                case 'r': r += '\r'; break;
                case 'b': r += '\b'; break;
                case 'f': r += '\f'; break;
                case 'u': {
                    unsigned cp = std::stoul(t_.substr(p_, 4), nullptr, 16);
                    p_ += 4;
                    if (cp < 0x80) r += (char)cp;
                    else if (cp < 0x800) { r += (char)(0xc0 | (cp >> 6)); r += (char)(0x80 | (cp & 0x3f)); }
                    else { r += (char)(0xe0 | (cp >> 12)); r += (char)(0x80 | ((cp >> 6) & 0x3f)); r += (char)(0x80 | (cp & 0x3f)); }
                    break;
                }
                default: r += e;
            }
        }
        if (p_ >= t_.size()) fail("a string has no closing quote");
        p_++;
        return r;
    }
};

inline Value parse(const std::string& text) { return Parser(text).parse(); }

}  // namespace vp::json
