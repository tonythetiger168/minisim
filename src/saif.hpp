#pragma once
// saif.hpp - switching activity for power analysis: the SAIF reader and
// writer, and the per-bit activity model the power tools work on.
//
//   saif::Db db;
//   saif::load("run.saif", db, threads);          // or a .vcd / .fst waveform
//   const saif::Bit* b = db.find("tb.dut.q[3]");  // t0, t1, tx (ticks), tc (toggles)
//   saif::write(db, os, "dut", "msim", "0.1.0");  // SAIF 2.0
//
// SAIF 2.0 (backward SAIF) holds, per net, the time spent at 0, 1 and x and
// the toggle count TC, already summed over the run. A waveform holds every
// value change; it is read by wave_reader.hpp (the reader mdebug uses) and
// summed here into the same numbers, one signal per thread. scan() also hands
// each toggle to the caller, which is how mpower bins energy over time.
//
// State- and path-dependent SAIF is read too: the COND / IOPATH toggle counts
// of a port go to Db::paths and the LEAKAGE state times of an instance to
// Db::states, as written (conditions as text). write() does not emit them.
//
// A signal is kept per bit. Its name is the simulator's hierarchical name
// with '.' between scopes and "[i]" for a bit of a vector: tb.dut.q[3]. A
// waveform does not always record a vector's range (msim writes "q", width
// 8); the bits of such a vector are numbered from 0 at the right.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "parallel.hpp"
#include "wave_reader.hpp"

namespace saif {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One bit's activity over the run. Times are in ticks of Db::tick seconds.
struct Bit {
    double t0 = 0, t1 = 0, tx = 0;   // time at 0, at 1, and at x or z
    double tc = 0;                   // 0->1 plus 1->0 transitions
};

// A named run of bits, most significant first. SAIF nets are one bit each.
struct Sig {
    uint32_t base = 0;
    int width = 1;
    int wave = -1;   // its wave in the wave::Reader it came from, or -1
    int msb = -1;    // the index of the first bit, where the file declares a range; -1: width - 1
    int step = -1;   // -1: [7:0], +1: [0:7]
    int index(int j) const { return (msb < 0 ? width - 1 : msb) + step * j; }
};

// State- and path-dependent activity (SDPD), as the file gives it. A port's
// toggles split by the condition they happened under (COND), the edge (RISE,
// FALL) and the input that caused them (IOPATH); an instance's time in each
// state (LEAKAGE). Conditions are kept as text, over the cell's pin names.
struct PathCount {
    std::string cond;      // "" for COND_DEFAULT, or when there is no COND
    std::string related;   // "" for IOPATH_DEFAULT, or when there is no IOPATH
    int edge = 0;          // 0 either, 1 RISE, 2 FALL
    double tc = 0;
};
struct StateTime {
    std::string cond;      // "" for COND_DEFAULT
    double t1 = 0;         // ticks the condition held
};

struct Db {
    std::unordered_map<std::string, std::vector<PathCount>> paths;    // "tb.dut.u1.Y"
    std::unordered_map<std::string, std::vector<StateTime>> states;   // "tb.dut.u1"
    double tick = 1e-9;      // seconds per time unit
    double duration = 0;     // ticks
    uint64_t start = 0;      // the time of the first value (waveforms)
    std::vector<Bit> bits;
    std::unordered_map<std::string, Sig> sigs;

    uint32_t add(const std::string& name) {
        auto [it, fresh] = sigs.try_emplace(name, Sig{(uint32_t)bits.size(), 1, -1});
        if (fresh) bits.emplace_back();
        return it->second.base;
    }
    // "tb.dut.en", "tb.dut.q[3]": the bit, or -1.
    int findIndex(const std::string& name) const {
        auto it = sigs.find(name);
        if (it != sigs.end()) return it->second.width == 1 ? (int)it->second.base : -1;
        if (name.empty() || name.back() != ']') return -1;
        size_t br = name.rfind('[');
        if (br == std::string::npos) return -1;
        it = sigs.find(name.substr(0, br));
        if (it == sigs.end()) return -1;
        char* end = nullptr;
        long i = std::strtol(name.c_str() + br + 1, &end, 10);
        if (!end || *end != ']') return -1;
        const Sig& s = it->second;
        long j = (i - (s.msb < 0 ? s.width - 1 : s.msb)) * s.step;
        if (j < 0 || j >= s.width) return -1;
        return (int)s.base + (int)j;
    }
    const Bit* find(const std::string& name) const {
        int i = findIndex(name);
        return i < 0 ? nullptr : &bits[(size_t)i];
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

// ------------------------------------------------------------- waveforms

// Walks one signal's value changes. `out`, if given, gets each bit's T0 / T1 /
// TX / TC (out[0] is the most significant bit); toggle(bit, time, rise) is
// called for every 0<->1 transition. A transition through x counts once the
// bit is known again and differs from its last known value.
template<typename F>
void scan(const wave::Wave& w, uint64_t start, uint64_t end, Bit* out, F&& toggle) {
    int width = w.width;
    if (width <= 0) return;
    struct St {
        uint64_t last;
        char cur, known;
    };
    St one[64];
    std::vector<St> many;
    St* st = one;
    if (width > 64) {
        many.resize((size_t)width);
        st = many.data();
    }
    for (int j = 0; j < width; j++) st[j] = {start, 'x', 'x'};
    auto close = [&](int j, uint64_t t) {
        if (!out) return;
        double dt = (double)(t - st[j].last);
        if (st[j].cur == '0') out[j].t0 += dt;
        else if (st[j].cur == '1') out[j].t1 += dt;
        else out[j].tx += dt;
    };
    for (const wave::Change& c : w.ch) {
        const char* p = w.base + c.off;
        int len = (int)c.len;
        // A short value is extended on the left (IEEE 1364 18.2.3.1): with x
        // or z when its first character is x or z, otherwise with 0.
        int pad = width - len;
        char fill = '0';
        if (pad > 0 && len > 0 && (p[0] == 'x' || p[0] == 'X' || p[0] == 'z' || p[0] == 'Z')) fill = 'x';
        for (int j = 0; j < width; j++) {
            char v = j < pad ? fill : p[j - pad];
            if (v != '0' && v != '1') v = v == 'h' || v == 'H' ? '1' : v == 'l' || v == 'L' ? '0' : 'x';
            St& s = st[j];
            if (v == s.cur) continue;
            close(j, c.t);
            s.last = c.t;
            s.cur = v;
            if (v == 'x') continue;
            if (s.known != 'x' && s.known != v) {
                if (out) out[j].tc += 1;
                toggle(j, c.t, v == '1');
            }
            s.known = v;
        }
    }
    for (int j = 0; j < width; j++) close(j, std::max(end, st[j].last));
}

// Sums a loaded waveform into the Db: one run of bits per signal, every name
// of an aliased signal pointing at the same run.
inline void fromWave(const wave::Reader& r, Db& db, unsigned threads) {
    db.tick = r.timescale.empty() ? 1e-9 : parseTimescale(r.timescale);
    std::vector<uint32_t> base(r.waves.size());
    uint64_t total = db.bits.size(), start = UINT64_MAX;
    for (size_t w = 0; w < r.waves.size(); w++) {
        base[w] = (uint32_t)total;
        total += (uint64_t)std::max(0, r.waves[w].width);
        if (!r.waves[w].ch.empty()) start = std::min(start, r.waves[w].ch[0].t);
    }
    if (total > UINT32_MAX) throw Error("the waveform has more bits than a power run can hold");
    if (start == UINT64_MAX) start = 0;
    db.bits.resize((size_t)total);
    db.start = start;
    db.duration = (double)(r.end > start ? r.end - start : 0);
    for (auto& [name, w] : r.byName) {
        Sig s{base[(size_t)w], r.waves[(size_t)w].width, w};
        // "[7:0]", "[0:7]", "[11:4]": the declared range, when it fits the width
        if (auto it = r.range.find(name); it != r.range.end()) {
            int m = 0, l = 0;
            if (std::sscanf(it->second.c_str(), " [ %d : %d ]", &m, &l) == 2 && std::abs(m - l) + 1 == s.width) {
                s.msb = m;
                s.step = m >= l ? -1 : 1;
            }
        }
        db.sigs[name] = s;
    }
    par::parallelFor(r.waves.size(), threads, [&](size_t w) {
        scan(r.waves[w], start, r.end, db.bits.data() + base[w], [](int, uint64_t, bool) {});
    });
}

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

struct SaifParser {
    std::string path;

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
    // The inside of a list whose head word `head` was just read (COND, IOPATH or
    // LEAKAGE), to its ')'. The forms nest and chain freely:
    //   (COND B (RISE) (IOPATH A (TC 3) (IG 0)) COND B (FALL) (IOPATH A (TC 2)) COND_DEFAULT (TC 1))
    //   (IOPATH A (TC 3) IOPATH_DEFAULT (TC 1))
    //   (LEAKAGE (COND (A*B) (T1 20) (T0 80)) COND_DEFAULT (T1 80) (T0 20))
    // so this walks the tokens, keeps the condition, edge and path in force, and
    // records a count at every (TC n) and a time at every (T1 n).
    struct SdCtx {
        std::string cond, related;
        int edge = 0;
    };
    static bool sdKeyword(const std::string& t) {
        return t == "RISE" || t == "FALL" || t == "RISE_FALL" || t == "IOPATH" || t == "IOPATH_DEFAULT" || t == "TC" ||
               t == "IG" || t == "T0" || t == "T1" || t == "TX" || t == "TZ" || t == "TB" || t == "COND" || t == "COND_DEFAULT";
    }
    // A condition: a word or a parenthesized expression, and what is glued to it.
    static std::string sdExpr(SaifLex& lx) {
        std::string out, t;
        for (;;) {
            size_t p0 = lx.p;
            int l0 = lx.line;
            int k = lx.next(t);
            if (k == 'w' && !sdKeyword(t)) {
                out += t;
                continue;
            }
            if (k == '(') {
                size_t p1 = lx.p;
                int l1 = lx.line;
                std::string h;
                int k2 = lx.next(h);
                lx.p = p1;
                lx.line = l1;
                if (!(k2 == 'w' && sdKeyword(h))) {
                    // a parenthesized part of the expression
                    out += "(";
                    int depth = 1;
                    while (depth > 0) {
                        k = lx.next(t);
                        if (k == 0) lx.fail("unexpected end of file in a condition");
                        if (k == '(') depth++, out += "(";
                        else if (k == ')') depth--, out += ")";
                        else out += t;
                    }
                    continue;
                }
            }
            lx.p = p0;
            lx.line = l0;
            return out;
        }
    }
    void sdpd(SaifLex& lx, const std::string& head, SdCtx cx, std::vector<PathCount>* paths, std::vector<StateTime>* states) {
        std::string t;
        auto word = [&](const std::string& w) {
            if (w == "COND") cx.cond = sdExpr(lx), cx.edge = 0, cx.related.clear();
            else if (w == "COND_DEFAULT") cx.cond.clear(), cx.edge = 0, cx.related.clear();
            else if (w == "RISE") cx.edge = 1;
            else if (w == "FALL") cx.edge = 2;
            else if (w == "RISE_FALL") cx.edge = 0;
            else if (w == "IOPATH") {
                if (lx.next(t) != 'w') lx.fail("expected a port after IOPATH");
                cx.related = t;
            } else if (w == "IOPATH_DEFAULT") cx.related.clear();
        };
        word(head);
        for (;;) {
            int k = lx.next(t);
            if (k == 0) lx.fail("unexpected end of file");
            if (k == ')') return;
            if (k == 'w') {
                word(t);
                continue;
            }
            if (lx.next(t) != 'w') lx.fail("expected a keyword");
            std::string key = t;
            if (key == "TC" || key == "T1") {
                auto v = words(lx);
                double x = v.empty() ? 0 : std::atof(v[0].c_str());
                if (key == "TC" && paths) paths->push_back({cx.cond, cx.related, cx.edge, x});
                if (key == "T1" && states) states->push_back({cx.cond, x});
            } else if (key == "RISE" || key == "FALL" || key == "RISE_FALL") {
                word(key);
                words(lx);
            } else if (key == "COND" || key == "COND_DEFAULT" || key == "IOPATH" || key == "IOPATH_DEFAULT") {
                sdpd(lx, key, cx, paths, states);
            } else {
                skip(lx);
            }
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
                if (key == "COND" || key == "COND_DEFAULT" || key == "IOPATH" || key == "IOPATH_DEFAULT") {
                    sdpd(lx, key, {}, &db.paths[scope.empty() ? name : scope + "." + name], nullptr);
                    continue;
                }
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
            else if (t == "LEAKAGE") sdpd(lx, "", {}, nullptr, &db.states[here]);
            else skip(lx);
        }
    }
    void run(Db& db) {
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
};

// Reads a SAIF file into the Db; throws saif::Error with file:line.
inline void read(const std::string& path, Db& db) {
    SaifParser p;
    p.path = path;
    p.run(db);
}

// Writes the activity as SAIF 2.0, one NET entry per bit.
inline void write(const Db& db, std::ostream& os, const std::string& design, const std::string& program,
                      const std::string& version) {
    struct Node {
        std::map<std::string, Node> kids;
        std::vector<std::pair<std::string, uint32_t>> nets;
    };
    Node root;
    // every name of a bit is written, so an alias is found under either name
    std::vector<std::pair<std::string, uint32_t>> all;
    for (auto& [name, s] : db.sigs) {
        if (s.width == 1) all.push_back({name, s.base});
        else
            for (int j = 0; j < s.width; j++)
                all.push_back({name + "[" + std::to_string(s.index(j)) + "]", s.base + (uint32_t)j});
    }
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


// A .saif file or a waveform (.vcd, .fst: by content) into a Db. Give `keep`
// to hold on to the waveform itself, for scan() later.
inline void load(const std::string& path, Db& db, unsigned threads = 1, wave::Reader* keep = nullptr) {
    auto ends = [&](const char* e) {
        size_t n = std::strlen(e);
        return path.size() >= n && path.compare(path.size() - n, n, e) == 0;
    };
    if (ends(".saif") || ends(".SAIF")) {
        read(path, db);
        return;
    }
    wave::Reader local;
    wave::Reader& r = keep ? *keep : local;
    std::string err;
    if (!r.open(path, threads, err)) throw Error(err);
    fromWave(r, db, threads);
}

}  // namespace saif
