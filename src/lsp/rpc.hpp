// rpc.hpp - the Language Server Protocol plumbing mlsp needs: JSON-RPC
// framing on a pair of file descriptors, a small JSON writer, file:// URIs
// and LSP positions (UTF-16 code units, or bytes when the client agrees to
// "utf-8").
//
// JSON is read with vp::json (src/vp/json.hpp), the reader mvplat already
// uses; nothing here parses JSON a second time.
#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <unistd.h>

#include "../vp/json.hpp"

namespace lsp {

using Json = vp::json::Value;

// ---------------------------------------------------------------- writing JSON

inline std::string quote(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 2);
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                }
                else {
                    o += (char)c;
                }
        }
    }
    o += '"';
    return o;
}

// A JSON object written field by field: Obj().f("a", 1).s("b", "x").str().
class Obj {
public:
    Obj& raw(std::string_view k, std::string_view json) {
        o_ += o_.size() > 1 ? "," : "";
        o_ += quote(k);
        o_ += ':';
        o_ += json;
        return *this;
    }
    Obj& s(std::string_view k, std::string_view v) { return raw(k, quote(v)); }
    Obj& i(std::string_view k, long long v) { return raw(k, std::to_string(v)); }
    Obj& b(std::string_view k, bool v) { return raw(k, v ? "true" : "false"); }
    std::string str() const { return o_ + "}"; }

private:
    std::string o_ = "{";
};

inline std::string arr(const std::vector<std::string>& items) {
    std::string o = "[";
    for (size_t k = 0; k < items.size(); k++) o += (k ? "," : "") + items[k];
    return o + "]";
}

// The text of a Value written back as JSON (a request id is echoed as is).
inline std::string write(const Json& v) {
    switch (v.kind) {
        case Json::Null: return "null";
        case Json::Bool: return v.b ? "true" : "false";
        case Json::Num: return v.s;
        case Json::Str: return quote(v.s);
        case Json::Arr: {
            std::vector<std::string> a;
            for (auto& x : v.a) a.push_back(write(x));
            return arr(a);
        }
        case Json::Obj: {
            Obj o;
            for (auto& [k, x] : v.o) o.raw(k, write(x));
            return o.str();
        }
    }
    return "null";
}

// Small getters that never throw: a missing or mistyped field is a default.
inline const Json* at(const Json* v, std::string_view path) {
    while (v && !path.empty()) {
        size_t dot = path.find('.');
        std::string k(path.substr(0, dot));
        v = v->kind == Json::Obj ? v->get(k) : nullptr;
        path = dot == std::string_view::npos ? std::string_view() : path.substr(dot + 1);
    }
    return v;
}
inline std::string str(const Json* v, std::string_view path, std::string def = "") {
    v = at(v, path);
    return v && v->kind == Json::Str ? v->s : def;
}
inline long long num(const Json* v, std::string_view path, long long def = 0) {
    v = at(v, path);
    if (!v || v->kind != Json::Num) return def;
    try { return std::stoll(v->s); } catch (...) { return def; }
}
inline bool boolean(const Json* v, std::string_view path, bool def) {
    v = at(v, path);
    return v && v->kind == Json::Bool ? v->b : def;
}

// ---------------------------------------------------------------- framing

// Reads and writes "Content-Length: N\r\n\r\n<N bytes>" messages. write() may
// be called from any thread.
class Channel {
public:
    Channel(int in, int out) : in_(in), out_(out) {}

    // One message body, or false at the end of the input.
    bool read(std::string& body) {
        size_t len = 0;
        bool haveLen = false;
        for (;;) {
            std::string line;
            if (!readLine(line)) return false;
            if (line.empty()) {
                if (haveLen) break;
                continue;  // stray blank line between messages
            }
            const std::string key = "content-length:";
            if (line.size() > key.size()) {
                std::string head = line.substr(0, key.size());
                for (auto& c : head) c = (char)std::tolower((unsigned char)c);
                if (head == key) {
                    len = std::strtoull(line.c_str() + key.size(), nullptr, 10);
                    haveLen = true;
                }
            }
        }
        body.resize(len);
        size_t got = 0;
        while (got < len) {
            if (pos_ < end_) {
                size_t n = std::min(len - got, end_ - pos_);
                body.replace(got, n, buf_ + pos_, n);
                pos_ += n;
                got += n;
                continue;
            }
            if (!fill()) return false;
        }
        return true;
    }

    void write(const std::string& body) {
        std::string msg = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        std::lock_guard<std::mutex> lk(m_);
        size_t off = 0;
        while (off < msg.size()) {
            ssize_t n = ::write(out_, msg.data() + off, msg.size() - off);
            if (n <= 0) return;
            off += (size_t)n;
        }
    }

private:
    bool fill() {
        ssize_t n = ::read(in_, buf_, sizeof buf_);
        if (n <= 0) return false;
        pos_ = 0;
        end_ = (size_t)n;
        return true;
    }
    bool readLine(std::string& line) {
        line.clear();
        for (;;) {
            if (pos_ >= end_ && !fill()) return false;
            char c = buf_[pos_++];
            if (c == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            line += c;
        }
    }

    int in_, out_;
    char buf_[65536];
    size_t pos_ = 0, end_ = 0;
    std::mutex m_;
};

// ---------------------------------------------------------------- URIs

inline std::string uriToPath(std::string_view uri) {
    std::string_view p = uri;
    if (p.substr(0, 7) == "file://") p.remove_prefix(7);
    std::string o;
    for (size_t k = 0; k < p.size(); k++) {
        if (p[k] == '%' && k + 2 < p.size() && std::isxdigit((unsigned char)p[k + 1]) &&
            std::isxdigit((unsigned char)p[k + 2])) {
            o += (char)std::stoi(std::string(p.substr(k + 1, 2)), nullptr, 16);
            k += 2;
        }
        else {
            o += p[k];
        }
    }
    // file:///c%3A/x on Windows: drop the slash before the drive letter.
    if (o.size() > 2 && o[0] == '/' && std::isalpha((unsigned char)o[1]) && o[2] == ':') o.erase(0, 1);
    return o;
}

inline std::string pathToUri(std::string_view path) {
    static const char* hex = "0123456789ABCDEF";
    std::string o = "file://";
    if (!path.empty() && path[0] != '/') o += '/';  // C:/x
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            o += (char)c;
        }
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

// ---------------------------------------------------------------- positions

// LSP positions count UTF-16 code units by default. `utf8` is set when the
// client accepted "utf-8" in initialize (then a character is a byte).
struct Encoding {
    bool utf8 = false;

    // Units from the start of `line` to byte offset `bytes` of it.
    unsigned units(std::string_view line, size_t bytes) const {
        if (bytes > line.size()) bytes = line.size();
        if (utf8) return (unsigned)bytes;
        unsigned n = 0;
        for (size_t k = 0; k < bytes;) {
            unsigned char c = (unsigned char)line[k];
            size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            n += len == 4 ? 2 : 1;  // outside the BMP: a surrogate pair
            k += len;
        }
        return n;
    }
    // The byte offset in `line` of `units` code units.
    size_t bytes(std::string_view line, unsigned units) const {
        if (utf8) return std::min<size_t>(units, line.size());
        size_t k = 0;
        unsigned n = 0;
        while (k < line.size() && n < units) {
            unsigned char c = (unsigned char)line[k];
            if (c == '\n' || c == '\r') break;
            size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            n += len == 4 ? 2 : 1;
            k += len;
        }
        return std::min(k, line.size());
    }
};

// Start offsets of each line of a text (line 0 starts at 0).
inline std::vector<size_t> lineStarts(std::string_view t) {
    std::vector<size_t> v{0};
    for (size_t k = 0; k < t.size(); k++)
        if (t[k] == '\n') v.push_back(k + 1);
    return v;
}

inline std::string_view lineOf(std::string_view t, const std::vector<size_t>& starts, size_t line) {
    if (line >= starts.size()) return {};
    size_t b = starts[line], e = line + 1 < starts.size() ? starts[line + 1] : t.size();
    return t.substr(b, e - b);
}

}  // namespace lsp
