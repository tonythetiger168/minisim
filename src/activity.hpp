#pragma once
// activity.hpp - switching activity for power analysis.
//
// One model (Db) for the three places activity comes from:
//   * SAIF 2.0 (backward SAIF): T0 / T1 / TX times and the toggle count TC of
//     every net, already summed over the run;
//   * VCD and FST waveforms (msim -vcd / -fst): every value change, which is
//     summed into the same numbers and can also be streamed to a Sink, so the
//     power tool can bin energy over time (peak power).
// and a SAIF writer, so a waveform converts to SAIF (msim -saif, mpower
// -write_saif).
//
// A signal is kept per bit. Its name is the simulator's hierarchical name
// with '.' between scopes and "[i]" for a bit of a vector: tb.dut.q[3].
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef MINISIM_FST
#include "fstapi.h"
#endif

namespace act {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One bit's activity over the run. Times are in ticks of Db::tick seconds.
struct Bit {
    double t0 = 0, t1 = 0, tx = 0;   // time at 0, at 1, and at x or z
    double tc = 0;                   // 0->1 plus 1->0 transitions
    // streaming state (waveform readers)
    uint64_t last = 0;
    char cur = 'x', known = 'x';
};

// What a waveform reader calls for every 0<->1 transition, in time order.
struct Sink {
    virtual ~Sink() = default;
    virtual void toggle(uint32_t bit, uint64_t time, bool rise) = 0;
};

struct Db {
    double tick = 1e-9;      // seconds per time unit
    double duration = 0;     // ticks
    std::vector<Bit> bits;
    std::vector<std::string> names;                      // the first name of each bit
    std::unordered_map<std::string, uint32_t> index;     // every name -> bit

    uint32_t add(const std::string& name) {
        auto [it, fresh] = index.try_emplace(name, (uint32_t)bits.size());
        if (fresh) {
            bits.emplace_back();
            names.push_back(name);
        }
        return it->second;
    }
    const Bit* find(const std::string& name) const {
        auto it = index.find(name);
        return it == index.end() ? nullptr : &bits[it->second];
    }
    int findIndex(const std::string& name) const {
        auto it = index.find(name);
        return it == index.end() ? -1 : (int)it->second;
    }
    double seconds() const { return duration * tick; }
};

// "1 ns", "10ps", "100 fs" -> seconds.
inline double parseTimescale(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    size_t j = i;
    while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == '.')) j++;
    double n = j > i ? std::atof(s.substr(i, j - i).c_str()) : 1.0;
    while (j < s.size() && std::isspace((unsigned char)s[j])) j++;
    std::string u;
    while (j < s.size() && std::isalpha((unsigned char)s[j])) u += (char)std::tolower((unsigned char)s[j++]);
    double m = 1;
    if (u == "s") m = 1;
    else if (u == "ms") m = 1e-3;
    else if (u == "us") m = 1e-6;
    else if (u == "ns") m = 1e-9;
    else if (u == "ps") m = 1e-12;
    else if (u == "fs") m = 1e-15;
    else throw Error("bad timescale '" + s + "'");
    return n * m;
}

// The bits a waveform variable declares. "data [7:0]" of width 8 gives
// data[7] .. data[0], most significant first (the order of a value string);
// "q" of width 1 gives q; "q [3]" gives q[3].
inline std::vector<std::string> varBits(const std::string& scope, std::string name, int width) {
    int msb = width - 1, lsb = 0;
    bool ranged = false;
    // An escaped identifier ends at a space, so a '[' in it is not a range.
    size_t br = name.rfind('[');
    if (br != std::string::npos && !name.empty() && name.back() == ']') {
        std::string r = name.substr(br + 1, name.size() - br - 2);
        size_t colon = r.find(':');
        bool num = !r.empty();
        for (char c : r)
            if (!std::isdigit((unsigned char)c) && c != ':' && c != '-' && c != ' ') num = false;
        if (num) {
            if (colon != std::string::npos) {
                msb = std::atoi(r.substr(0, colon).c_str());
                lsb = std::atoi(r.substr(colon + 1).c_str());
                if (std::abs(msb - lsb) + 1 == width) {
                    ranged = true;
                    name.erase(br);
                }
            } else if (width == 1) {
                // one bit of a vector: keep the select in the name, without the space
                size_t e = br;
                while (e > 0 && name[e - 1] == ' ') e--;
                name = name.substr(0, e) + "[" + r + "]";
            }
        }
    }
    while (!name.empty() && name.back() == ' ') name.pop_back();
    std::string full = scope.empty() ? name : scope + "." + name;
    std::vector<std::string> out;
    if (width == 1 && !ranged) {
        out.push_back(full);
        return out;
    }
    int step = msb >= lsb ? -1 : 1;
    for (int k = 0, b = msb; k < width; k++, b += step) out.push_back(full + "[" + std::to_string(b) + "]");
    return out;
}

// ------------------------------------------------------------- waveforms

struct Reader {
    virtual ~Reader() = default;
    virtual void header(Db& db) = 0;             // the names and the timescale
    virtual void body(Db& db, Sink* sink) = 0;   // the value changes
};

// Shared by the VCD and FST readers: one variable is a run of bits, and a new
// value string updates each bit's time counters.
struct Wave {
    Db* db = nullptr;
    Sink* sink = nullptr;
    uint64_t now = 0, first = 0;
    bool started = false;

    void time(uint64_t t) {
        if (!started) {
            started = true;
            first = t;
            for (Bit& b : db->bits) b.last = t;
        }
        now = t;
    }
    void set(uint32_t id, char v) {
        Bit& b = db->bits[id];
        if (v == 'X') v = 'x';
        else if (v == 'z' || v == 'Z' || v == 'u' || v == 'U' || v == 'w' || v == 'W' || v == '-') v = 'x';
        else if (v == 'h' || v == 'H') v = '1';
        else if (v == 'l' || v == 'L') v = '0';
        if (v == b.cur) return;
        double dt = (double)(now - b.last);
        if (b.cur == '0') b.t0 += dt;
        else if (b.cur == '1') b.t1 += dt;
        else b.tx += dt;
        b.last = now;
        b.cur = v;
        if (v == 'x') return;
        if (b.known != 'x' && b.known != v) {
            b.tc += 1;
            if (sink) sink->toggle(id, now, v == '1');
        }
        b.known = v;
    }
    // A value string for a variable of `width` bits starting at bit `base`.
    // A short string is extended on the left (IEEE 1364 18.2.3.1): with x or
    // z when its first character is x or z, otherwise with 0.
    void value(uint32_t base, int width, const char* s, int n) {
        if (n >= width) {
            s += n - width;
            for (int i = 0; i < width; i++) set(base + i, s[i]);
            return;
        }
        char fill = (n > 0 && (s[0] == 'x' || s[0] == 'X' || s[0] == 'z' || s[0] == 'Z')) ? s[0] : '0';
        int pad = width - n;
        for (int i = 0; i < pad; i++) set(base + i, fill);
        for (int i = 0; i < n; i++) set(base + pad + i, s[i]);
    }
    void finish(uint64_t end) {
        if (!started) return;
        if (end < now) end = now;
        for (Bit& b : db->bits) {
            double dt = (double)(end - b.last);
            if (b.cur == '0') b.t0 += dt;
            else if (b.cur == '1') b.t1 += dt;
            else b.tx += dt;
            b.last = end;
        }
        db->duration = (double)(end - first);
    }
};

struct VcdReader : Reader {
    FILE* f = nullptr;
    std::string path;
    std::vector<char> buf;
    size_t pos = 0, len = 0;
    struct Var { uint32_t base; int width; };
    // identifier code -> variable (aliases share one code and one run of bits)
    std::unordered_map<std::string, Var> vars;

    explicit VcdReader(const std::string& p) : path(p), buf(1 << 20) {
        f = std::fopen(p.c_str(), "rb");
        if (!f) throw Error("cannot read " + p);
    }
    ~VcdReader() override {
        if (f) std::fclose(f);
    }
    int get() {
        if (pos == len) {
            len = std::fread(buf.data(), 1, buf.size(), f);
            pos = 0;
            if (len == 0) return EOF;
        }
        return (unsigned char)buf[pos++];
    }
    bool token(std::string& t) {
        t.clear();
        int c;
        while ((c = get()) != EOF && std::isspace(c)) {}
        if (c == EOF) return false;
        do t += (char)c;
        while ((c = get()) != EOF && !std::isspace(c));
        return true;
    }
    void header(Db& db) override {
        std::string t;
        std::vector<std::string> scope;
        while (token(t)) {
            if (t == "$scope") {
                std::string kind, name;
                token(kind);
                token(name);
                scope.push_back(name);
                while (token(t) && t != "$end") {}
            } else if (t == "$upscope") {
                if (!scope.empty()) scope.pop_back();
                while (token(t) && t != "$end") {}
            } else if (t == "$timescale") {
                std::string ts;
                while (token(t) && t != "$end") ts += t + " ";
                db.tick = parseTimescale(ts);
            } else if (t == "$var") {
                std::string type, w, code, name, rest;
                token(type);
                token(w);
                token(code);
                token(name);
                while (token(t) && t != "$end") rest += (rest.empty() ? "" : " ") + t;
                int width = std::atoi(w.c_str());
                if (type == "real" || type == "realtime" || type == "event" || type == "string" || width <= 0) continue;
                std::string sc;
                for (auto& s : scope) sc += (sc.empty() ? "" : ".") + s;
                auto bitNames = varBits(sc, rest.empty() ? name : name + " " + rest, width);
                auto it = vars.find(code);
                if (it == vars.end()) {
                    uint32_t base = (uint32_t)db.bits.size();
                    // a fresh run of bits even if a name is already known, so the run stays contiguous
                    for (auto& n : bitNames) {
                        db.bits.emplace_back();
                        db.names.push_back(n);
                        db.index.emplace(n, (uint32_t)db.bits.size() - 1);
                    }
                    vars[code] = {base, width};
                } else if (it->second.width == width) {
                    for (int i = 0; i < width; i++) db.index.emplace(bitNames[i], it->second.base + i);
                }
            } else if (t == "$enddefinitions") {
                while (token(t) && t != "$end") {}
                return;
            } else if (t[0] == '$') {
                while (token(t) && t != "$end") {}
            }
        }
    }
    void body(Db& db, Sink* sink) override {
        Wave w;
        w.db = &db;
        w.sink = sink;
        std::string t, code;
        uint64_t last = 0;
        while (token(t)) {
            char c = t[0];
            if (c == '#') {
                last = std::strtoull(t.c_str() + 1, nullptr, 10);
                w.time(last);
            } else if (c == 'b' || c == 'B') {
                if (!token(code)) break;
                if (!w.started) w.time(0);
                auto it = vars.find(code);
                if (it != vars.end()) w.value(it->second.base, it->second.width, t.c_str() + 1, (int)t.size() - 1);
            } else if (c == 'r' || c == 'R' || c == 's' || c == 'S') {
                token(code);
            } else if (c == '$') {
                // $dumpvars, $dumpall, $dumpon, $dumpoff: the values inside are value changes;
                // $comment skips to its $end
                if (t == "$comment")
                    while (token(t) && t != "$end") {}
            } else {
                if (!w.started) w.time(0);
                auto it = vars.find(t.substr(1));
                if (it != vars.end() && it->second.width == 1) w.set(it->second.base, c);
                else if (it != vars.end()) w.value(it->second.base, it->second.width, t.c_str(), 1);
            }
        }
        w.finish(last);
    }
};

#ifdef MINISIM_FST
struct FstReader : Reader {
    void* ctx = nullptr;
    struct Var { uint32_t base; int width; };
    std::vector<Var> vars;   // by handle
    Wave w;

    explicit FstReader(const std::string& p) {
        ctx = fstReaderOpen(p.c_str());
        if (!ctx) throw Error("cannot read " + p);
    }
    ~FstReader() override {
        if (ctx) fstReaderClose(ctx);
    }
    void header(Db& db) override {
        db.tick = std::pow(10.0, (double)fstReaderGetTimescale(ctx));
        std::vector<std::string> scope;
        fstReaderIterateHierRewind(ctx);
        while (struct fstHier* h = fstReaderIterateHier(ctx)) {
            if (h->htyp == FST_HT_SCOPE) scope.push_back(h->u.scope.name);
            else if (h->htyp == FST_HT_UPSCOPE) {
                if (!scope.empty()) scope.pop_back();
            } else if (h->htyp == FST_HT_VAR) {
                auto& v = h->u.var;
                if (v.typ == FST_VT_VCD_REAL || v.typ == FST_VT_VCD_REALTIME || v.typ == FST_VT_VCD_EVENT ||
                    v.typ == FST_VT_GEN_STRING || v.length == 0)
                    continue;
                std::string sc;
                for (auto& s : scope) sc += (sc.empty() ? "" : ".") + s;
                auto bitNames = varBits(sc, v.name, (int)v.length);
                if (vars.size() <= v.handle) vars.resize(v.handle + 1, Var{0, 0});
                if (vars[v.handle].width == 0) {
                    uint32_t base = (uint32_t)db.bits.size();
                    for (auto& n : bitNames) {
                        db.bits.emplace_back();
                        db.names.push_back(n);
                        db.index.emplace(n, (uint32_t)db.bits.size() - 1);
                    }
                    vars[v.handle] = {base, (int)v.length};
                } else if (vars[v.handle].width == (int)v.length) {
                    for (int i = 0; i < (int)v.length; i++) db.index.emplace(bitNames[i], vars[v.handle].base + i);
                }
            }
        }
    }
    static void cb(void* user, uint64_t time, fstHandle h, const unsigned char* value) {
        auto* self = (FstReader*)user;
        if (!self->w.started || time != self->w.now) self->w.time(time);
        if (h >= self->vars.size() || self->vars[h].width == 0) return;
        self->w.value(self->vars[h].base, self->vars[h].width, (const char*)value, self->vars[h].width);
    }
    void body(Db& db, Sink* sink) override {
        w.db = &db;
        w.sink = sink;
        fstReaderSetFacProcessMaskAll(ctx);
        w.time(fstReaderGetStartTime(ctx));
        fstReaderIterBlocks(ctx, cb, this, nullptr);
        w.finish(fstReaderGetEndTime(ctx));
    }
};
#endif

// ------------------------------------------------------------------ SAIF

// SAIF is nested lists. Tokens: ( ) "string" and words; a backslash escapes
// the next character of a word (q\[3\]).
struct SaifLex {
    std::string text;
    size_t p = 0;
    int line = 1;
    std::string path;
    // returns 0 at the end, '(' or ')' for a parenthesis, 'w' for a word (in tok)
    int next(std::string& tok) {
        for (;;) {
            while (p < text.size() && std::isspace((unsigned char)text[p])) {
                if (text[p] == '\n') line++;
                p++;
            }
            if (p + 1 < text.size() && text[p] == '/' && text[p + 1] == '/') {
                while (p < text.size() && text[p] != '\n') p++;
                continue;
            }
            if (p + 1 < text.size() && text[p] == '/' && text[p + 1] == '*') {
                size_t e = text.find("*/", p + 2);
                for (size_t k = p; k < std::min(e, text.size()); k++)
                    if (text[k] == '\n') line++;
                p = e == std::string::npos ? text.size() : e + 2;
                continue;
            }
            break;
        }
        if (p >= text.size()) return 0;
        char c = text[p];
        if (c == '(' || c == ')') {
            p++;
            return c;
        }
        tok.clear();
        if (c == '"') {
            p++;
            while (p < text.size() && text[p] != '"') tok += text[p++];
            p++;
            return 'w';
        }
        while (p < text.size() && !std::isspace((unsigned char)text[p]) && text[p] != '(' && text[p] != ')') {
            if (text[p] == '\\' && p + 1 < text.size()) p++;
            tok += text[p++];
        }
        return 'w';
    }
    [[noreturn]] void fail(const std::string& m) { throw Error(path + ":" + std::to_string(line) + ": " + m); }
};

struct SaifReader : Reader {
    std::string path;
    explicit SaifReader(const std::string& p) : path(p) {}

    // Skips the rest of the list whose '(' and head were already read.
    static void skip(SaifLex& lx) {
        std::string t;
        int depth = 1;
        while (depth > 0) {
            int k = lx.next(t);
            if (k == 0) lx.fail("unexpected end of file");
            if (k == '(') depth++;
            else if (k == ')') depth--;
        }
    }
    // The words of a flat list, up to its ')'.
    static std::vector<std::string> words(SaifLex& lx) {
        std::vector<std::string> out;
        std::string t;
        for (;;) {
            int k = lx.next(t);
            if (k == 0) lx.fail("unexpected end of file");
            if (k == ')') return out;
            if (k == '(') {
                skip(lx);
                continue;
            }
            out.push_back(t);
        }
    }
    void nets(SaifLex& lx, Db& db, const std::string& scope, char divider) {
        std::string t;
        for (;;) {
            int k = lx.next(t);
            if (k == ')') return;
            if (k != '(') lx.fail("expected a net");
            if (lx.next(t) != 'w') lx.fail("expected a net name");
            std::string name = t;
            if (divider != '.')
                for (char& c : name)
                    if (c == divider) c = '.';
            Bit b;
            for (;;) {
                k = lx.next(t);
                if (k == ')') break;
                if (k != '(') lx.fail("expected (T0 ..) in net " + name);
                lx.next(t);
                std::string key = t;
                auto v = words(lx);
                double x = v.empty() ? 0 : std::atof(v[0].c_str());
                if (key == "T0") b.t0 = x;
                else if (key == "T1") b.t1 = x;
                else if (key == "TX" || key == "TZ" || key == "TB") b.tx += x;
                else if (key == "TC") b.tc = x;
            }
            uint32_t id = db.add(scope.empty() ? name : scope + "." + name);
            db.bits[id] = b;
        }
    }
    void instance(SaifLex& lx, Db& db, const std::string& scope, char divider) {
        std::string t;
        if (lx.next(t) != 'w') lx.fail("expected an instance name");
        std::string name = t;
        if (divider != '.')
            for (char& c : name)
                if (c == divider) c = '.';
        std::string here = scope.empty() ? name : scope + "." + name;
        for (;;) {
            int k = lx.next(t);
            if (k == ')') return;
            if (k != '(') lx.fail("expected ( in instance " + here);
            lx.next(t);
            if (t == "INSTANCE") instance(lx, db, here, divider);
            else if (t == "NET" || t == "PORT") nets(lx, db, here, divider);
            else skip(lx);
        }
    }
    void header(Db& db) override {
        std::ifstream f(path);
        if (!f) throw Error("cannot read " + path);
        std::stringstream ss;
        ss << f.rdbuf();
        SaifLex lx;
        lx.text = ss.str();
        lx.path = path;
        std::string t;
        if (lx.next(t) != '(' || lx.next(t) != 'w' || t != "SAIFILE") lx.fail("not a SAIF file");
        char divider = '/';
        for (;;) {
            int k = lx.next(t);
            if (k == ')' || k == 0) break;
            if (k != '(') lx.fail("expected (");
            lx.next(t);
            if (t == "TIMESCALE") {
                std::string ts;
                for (auto& w : words(lx)) ts += w + " ";
                db.tick = parseTimescale(ts);
            } else if (t == "DURATION") {
                auto w = words(lx);
                if (!w.empty()) db.duration = std::atof(w[0].c_str());
            } else if (t == "DIVIDER") {
                auto w = words(lx);
                if (!w.empty() && !w[0].empty()) divider = w[0][0];
            } else if (t == "INSTANCE") {
                instance(lx, db, "", divider);
            } else {
                skip(lx);
            }
        }
    }
    void body(Db&, Sink*) override {}
};

// Writes the activity as SAIF 2.0. `strip` is a scope prefix to leave out
// ("tb.dut" keeps only what is below it, rooted at `root`), "" for all.
inline void writeSaif(const Db& db, std::ostream& os, const std::string& design, const std::string& program,
                      const std::string& version) {
    struct Node {
        std::map<std::string, Node> kids;
        std::vector<std::pair<std::string, uint32_t>> nets;
    };
    Node root;
    // every name of a bit is written, so an alias is found under either name
    std::vector<std::pair<std::string, uint32_t>> all(db.index.begin(), db.index.end());
    std::sort(all.begin(), all.end());
    for (auto& [name, id] : all) {
        Node* n = &root;
        size_t from = 0;
        for (;;) {
            // a '.' inside [..] is not a scope separator
            size_t dot = std::string::npos;
            int depth = 0;
            for (size_t i = from; i < name.size(); i++) {
                if (name[i] == '[') depth++;
                else if (name[i] == ']') depth--;
                else if (name[i] == '.' && depth == 0) {
                    dot = i;
                    break;
                }
            }
            if (dot == std::string::npos) break;
            n = &n->kids[name.substr(from, dot - from)];
            from = dot + 1;
        }
        n->nets.push_back({name.substr(from), id});
    }
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '[' || c == ']' || c == '/' || c == '(' || c == ')' || c == '\\' || c == '"') o += '\\';
            o += c;
        }
        return o;
    };
    auto num = [](double v) {
        char b[64];
        if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%.0f", v);
        else std::snprintf(b, sizeof b, "%.6g", v);
        return std::string(b);
    };
    std::string unit = "ns";
    double scale = 1;
    {
        static const std::pair<const char*, double> us[] = {{"s", 1}, {"ms", 1e-3}, {"us", 1e-6},
                                                            {"ns", 1e-9}, {"ps", 1e-12}, {"fs", 1e-15}};
        for (auto& [u, m] : us) {
            double r = db.tick / m;
            if (r >= 0.999 && r < 999.5) {
                unit = u;
                scale = std::round(r);
                break;
            }
        }
    }
    os << "(SAIFILE\n(SAIFVERSION \"2.0\")\n(DIRECTION \"backward\")\n(DESIGN \"" << design << "\")\n"
       << "(VENDOR \"minisim\")\n(PROGRAM_NAME \"" << program << "\")\n(VERSION \"" << version << "\")\n"
       << "(DIVIDER / )\n(TIMESCALE " << num(scale) << " " << unit << ")\n(DURATION " << num(db.duration) << ")\n";
    std::function<void(const std::string&, const Node&, int)> emit = [&](const std::string& name, const Node& n,
                                                                         int depth) {
        std::string ind((size_t)depth, ' ');
        os << ind << "(INSTANCE " << esc(name) << "\n";
        if (!n.nets.empty()) {
            os << ind << " (NET\n";
            for (auto& [nm, id] : n.nets) {
                const Bit& b = db.bits[id];
                os << ind << "  (" << esc(nm) << " (T0 " << num(b.t0) << ") (T1 " << num(b.t1) << ") (TX "
                   << num(b.tx) << ") (TC " << num(b.tc) << ") (IG 0))\n";
            }
            os << ind << " )\n";
        }
        for (auto& [k, kid] : n.kids) emit(k, kid, depth + 1);
        os << ind << ")\n";
    };
    for (auto& [k, kid] : root.kids) emit(k, kid, 0);
    if (!root.nets.empty()) {
        Node top;
        top.nets = root.nets;
        emit(design.empty() ? "top" : design, top, 0);
    }
    os << ")\n";
}

inline bool endsWith(const std::string& s, const char* e) {
    size_t n = std::strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}

// Opens an activity file by its name: .saif, .fst, anything else is VCD.
inline std::unique_ptr<Reader> open(const std::string& path) {
    if (endsWith(path, ".saif") || endsWith(path, ".SAIF")) return std::make_unique<SaifReader>(path);
    if (endsWith(path, ".fst")) {
#ifdef MINISIM_FST
        return std::make_unique<FstReader>(path);
#else
        throw Error(path + ": this build has no FST support (MINISIM_WITH_FST=OFF)");
#endif
    }
    return std::make_unique<VcdReader>(path);
}

// A whole file into a Db (header and body).
inline void load(const std::string& path, Db& db, Sink* sink = nullptr) {
    auto r = open(path);
    r->header(db);
    r->body(db, sink);
}

}  // namespace act
