#pragma once
// wave_reader.hpp - read a VCD or an FST into memory, for every tool that
// looks at a waveform (mdebug, power analysis, ...).
//
//   wave::Reader r;
//   std::string err;
//   if (!r.open("w.vcd", par::defaultThreads(), err)) ...   // VCD or FST, by content
//   const wave::Wave* w = r.find("tb.u.q");                  // hierarchical name
//   int i = w->at(t);                                        // last change <= t, or -1
//   w->ch[i].t; w->bits(i);                                  // its time, its bits (msb first)
//
// Names are scope.scope.name; a vector "q [7:0]" is "q", an array element
// keeps its index ("mem[3]"). Reals, events and strings are skipped.
// r.end is the last time, r.timescale the unit ("1ns").
//
// The VCD reader is built for large dumps: the file is mapped, the body is
// cut at time stamps into one piece per thread, and no value is copied. The
// result does not depend on the thread count. FST needs MINISIM_FST and
// libfst (the `fst` CMake target).
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "parallel.hpp"
#ifdef MINISIM_FST
#include "fstapi.h"
#endif

namespace wave {

// One value change: when, and where its bits are in the mapped file.
struct Change {
    uint64_t t;
    uint64_t off : 40;
    uint64_t len : 24;
};

struct Wave {
    int width = 1;
    const char* base = nullptr;
    std::vector<Change> ch;

    // The last change at or before t, or -1.
    int at(uint64_t t) const {
        int lo = 0, hi = (int)ch.size();
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (ch[mid].t <= t) lo = mid + 1; else hi = mid;
        }
        return lo - 1;
    }

    // The bits of change i, msb first, lower case, at the full width.
    std::string bits(int i) const {
        std::string b(base + ch[i].off, ch[i].len);
        for (char& c : b) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        // A VCD drops leading bits: 0 unless the first bit is x or z.
        if ((int)b.size() < width) {
            char pad = (b[0] == 'x' || b[0] == 'z') ? b[0] : '0';
            b.insert(0, width - b.size(), pad);
        }
        return b;
    }
};

// A VCD reader built for large dumps: the file is mapped, the body is cut at
// time stamps into one piece per thread, and every piece is scanned twice,
// once to count each signal's changes and once to write them in place. No
// value is copied: a change points at its bits in the mapping.
struct Reader {
    std::vector<Wave> waves;
    std::map<std::string, int> byName;
    uint64_t end = 0;
    std::string timescale;
    const char* data = nullptr;
    size_t size = 0;

    ~Reader() { if (data && size) munmap((void*)data, size); }

    static bool space(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

    // Identifier codes are short printable strings: a number, then a table.
    static uint64_t code(const char* p, size_t n) {
        uint64_t c = 0;
        for (size_t i = 0; i < n; i++) c = c * 95 + (uint64_t)((unsigned char)p[i] - 32);
        return c;
    }

    struct Ids {
        std::vector<int> dense;
        std::unordered_map<uint64_t, int> sparse;
        static constexpr uint64_t kDense = 1u << 22;
        void put(uint64_t c, int ix) {
            if (c < kDense) {
                if (dense.size() <= c) dense.resize(c + 1, -1);
                dense[c] = ix;
            } else sparse[c] = ix;
        }
        int get(uint64_t c) const {
            if (c < dense.size()) return dense[c];
            if (c < kDense) return -1;
            auto it = sparse.find(c);
            return it == sparse.end() ? -1 : it->second;
        }
    };

    // Scan [p, e) of the body. `emit(wave, time, bits, len)` for each change.
    template<typename F>
    static uint64_t scan(const char* base, const char* p, const char* e, const Ids& ids, F&& emit) {
        uint64_t now = 0, last = 0;
        auto tokEnd = [&](const char* q) { while (q < e && !space(*q)) q++; return q; };
        while (p < e) {
            while (p < e && space(*p)) p++;
            if (p >= e) break;
            const char* q = tokEnd(p);
            char c = *p;
            if (c == '#') {
                now = 0;
                for (const char* d = p + 1; d < q && *d >= '0' && *d <= '9'; d++) now = now * 10 + (uint64_t)(*d - '0');
                last = std::max(last, now);
            } else if (c == 'b' || c == 'B') {
                const char* i0 = q;
                while (i0 < e && space(*i0)) i0++;
                const char* i1 = tokEnd(i0);
                int w = ids.get(code(i0, (size_t)(i1 - i0)));
                if (w >= 0) emit(w, now, (uint64_t)(p + 1 - base), (uint64_t)(q - p - 1));
                q = i1;
            } else if (c == 'r' || c == 'R' || c == 's' || c == 'S') {
                const char* i0 = q;
                while (i0 < e && space(*i0)) i0++;
                q = tokEnd(i0);
            } else if (c == '$') {
                // $dumpvars / $end bracket ordinary changes; a comment is skipped.
                if (q - p == 8 && !std::memcmp(p, "$comment", 8)) {
                    const char* x = (const char*)memmem(q, (size_t)(e - q), "$end", 4);
                    q = x ? x + 4 : e;
                }
            } else if (q - p >= 2) {
                int w = ids.get(code(p + 1, (size_t)(q - p - 1)));
                if (w >= 0) emit(w, now, (uint64_t)(p - base), 1);
            }
            p = q;
        }
        return last;
    }

    bool load(const std::string& file, unsigned threads, std::string& err) {
        int fd = ::open(file.c_str(), O_RDONLY);
        struct stat sb;
        if (fd < 0 || fstat(fd, &sb) != 0) { err = "cannot open " + file; if (fd >= 0) ::close(fd); return false; }
        size = (size_t)sb.st_size;
        if (size == 0) { ::close(fd); err = file + " is empty"; return false; }
        void* m = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (m == MAP_FAILED) { size = 0; err = "cannot map " + file; return false; }
        data = (const char*)m;
        madvise(m, size, MADV_SEQUENTIAL);
        if (size >= (1ull << 40)) { err = file + " is larger than mdebug reads (1 TB)"; return false; }

        // The header: scopes and variables, up to $enddefinitions.
        Ids ids;
        std::unordered_map<uint64_t, int> seen;
        std::vector<std::string> scope;
        const char* p = data;
        const char* e = data + size;
        auto tok = [&]() -> std::string_view {
            while (p < e && space(*p)) p++;
            const char* q = p;
            while (q < e && !space(*q)) q++;
            std::string_view t(p, (size_t)(q - p));
            p = q;
            return t;
        };
        auto skipToEnd = [&] { for (std::string_view t = tok(); !t.empty() && t != "$end"; t = tok()) {} };
        for (;;) {
            std::string_view t = tok();
            if (t.empty()) break;
            if (t == "$scope") {
                tok();
                scope.emplace_back(tok());
                skipToEnd();
            } else if (t == "$upscope") {
                if (!scope.empty()) scope.pop_back();
                skipToEnd();
            } else if (t == "$var") {
                std::string type(tok());
                int width = std::atoi(std::string(tok()).c_str());
                std::string_view id = tok();
                std::string name(tok()), rest;
                // "q [7:0]" is the vector q; "mem[3]" keeps its index.
                for (std::string_view r = tok(); !r.empty() && r != "$end"; r = tok()) rest += r;
                if (type != "real" && type != "event" && type != "string") {
                    std::string full;
                    for (auto& sc : scope) full += sc + ".";
                    full += name;
                    if (!rest.empty() && rest.find(':') == std::string::npos) full += rest;
                    uint64_t c = code(id.data(), id.size());
                    auto it = seen.find(c);
                    if (it == seen.end()) {
                        it = seen.emplace(c, (int)waves.size()).first;
                        ids.put(c, it->second);
                        waves.emplace_back().width = width;
                        waves.back().base = data;
                    }
                    byName[full] = it->second;
                }
            } else if (t == "$timescale") {
                for (std::string_view r = tok(); !r.empty() && r != "$end"; r = tok()) timescale += r;
            } else if (t == "$enddefinitions") {
                skipToEnd();
                break;
            } else if (t[0] == '$') {
                skipToEnd();
            }
        }

        // The body, cut at time stamps so every piece knows what time it is.
        const char* body = p;
        size_t left = (size_t)(e - body);
        size_t pieces = std::max<size_t>(1, std::min<size_t>(threads, left / (4u << 20)));
        std::vector<const char*> cut{body};
        for (size_t i = 1; i < pieces; i++) {
            const char* guess = body + left / pieces * i;
            const char* x = (const char*)memmem(guess, (size_t)(e - guess), "\n#", 2);
            if (!x) break;
            if (x + 1 > cut.back()) cut.push_back(x + 1);
        }
        cut.push_back(e);
        pieces = cut.size() - 1;
        size_t nw = waves.size();
        std::vector<std::vector<uint32_t>> count(pieces, std::vector<uint32_t>(nw, 0));
        std::vector<uint64_t> lastT(pieces, 0);
        par::parallelFor(pieces, threads, [&](size_t k) {
            auto& c = count[k];
            lastT[k] = scan(data, cut[k], cut[k + 1], ids, [&](int w, uint64_t, uint64_t, uint64_t) { c[w]++; });
        });
        for (uint64_t t : lastT) end = std::max(end, t);
        // Where each piece writes each signal: the pieces before it, in order.
        for (size_t w = 0; w < nw; w++) {
            uint64_t total = 0;
            for (size_t k = 0; k < pieces; k++) {
                uint32_t n = count[k][w];
                count[k][w] = (uint32_t)total;
                total += n;
            }
            waves[w].ch.resize(total);
        }
        par::parallelFor(pieces, threads, [&](size_t k) {
            auto& c = count[k];
            scan(data, cut[k], cut[k + 1], ids, [&](int w, uint64_t t, uint64_t off, uint64_t len) {
                waves[w].ch[c[w]++] = Change{t, off, len};
            });
        });
        compact(threads);
        return true;
    }

    // One entry per change: the later of two at one time, and no entry
    // that repeats the value before it ($dumpvars, then the same at #0).
    void compact(unsigned threads) {
        par::parallelFor(waves.size(), threads, [&](size_t w) {
            auto& ch = waves[w].ch;
            const char* data = waves[w].base;
            size_t n = 0;
            for (size_t i = 0; i < ch.size(); i++) {
                if (n && ch[n - 1].t == ch[i].t) { ch[n - 1] = ch[i]; continue; }
                if (n && ch[n - 1].len == ch[i].len &&
                    !std::memcmp(data + ch[n - 1].off, data + ch[i].off, ch[i].len))
                    continue;
                ch[n++] = ch[i];
            }
            // Replacing at one time can leave a repeat behind it.
            size_t m = 0;
            for (size_t i = 0; i < n; i++) {
                if (m && ch[m - 1].len == ch[i].len &&
                    !std::memcmp(data + ch[m - 1].off, data + ch[i].off, ch[i].len))
                    continue;
                ch[m++] = ch[i];
            }
            ch.resize(m);
        });
    }

#ifdef MINISIM_FST
    // An FST (msim -fst): libfst unpacks the blocks; each signal's values go
    // into a buffer of its own.
    std::vector<std::string> pools;

    bool loadFst(const std::string& file, unsigned threads, std::string& err) {
        fstReaderContext* ctx = fstReaderOpen(file.c_str());
        if (!ctx) { err = "cannot open " + file; return false; }
        std::vector<int> byHandle;   // FST handle -> wave
        std::vector<std::string> scope;
        while (struct fstHier* h = fstReaderIterateHier(ctx)) {
            if (h->htyp == FST_HT_SCOPE) scope.emplace_back(h->u.scope.name);
            else if (h->htyp == FST_HT_UPSCOPE) { if (!scope.empty()) scope.pop_back(); }
            else if (h->htyp == FST_HT_VAR) {
                unsigned char ty = h->u.var.typ;
                if (ty == FST_VT_VCD_REAL || ty == FST_VT_VCD_EVENT || ty == FST_VT_GEN_STRING ||
                    ty == FST_VT_VCD_REAL_PARAMETER || ty == FST_VT_VCD_REALTIME || ty == FST_VT_SV_SHORTREAL)
                    continue;
                std::string name(h->u.var.name), full;
                // "q [7:0]" is the vector q; "mem[3]" keeps its index.
                if (size_t sp = name.find(' '); sp != std::string::npos) {
                    std::string rest = name.substr(sp + 1);
                    name.resize(sp);
                    if (rest.find(':') == std::string::npos) name += rest;
                }
                for (auto& sc : scope) full += sc + ".";
                full += name;
                fstHandle hd = h->u.var.handle;
                if (byHandle.size() <= hd) byHandle.resize(hd + 1, -1);
                if (byHandle[hd] < 0) {
                    byHandle[hd] = (int)waves.size();
                    waves.emplace_back().width = (int)h->u.var.length;
                }
                byName[full] = byHandle[hd];
            }
        }
        pools.resize(waves.size());
        struct Ctx { Reader* v; std::vector<int>* byHandle; } cx{this, &byHandle};
        fstReaderSetFacProcessMaskAll(ctx);
        fstReaderIterBlocks(ctx, [](void* u, uint64_t t, fstHandle hd, const unsigned char* val) {
            Ctx& c = *(Ctx*)u;
            if (hd >= c.byHandle->size()) return;
            int w = (*c.byHandle)[hd];
            if (w < 0) return;
            std::string& pool = c.v->pools[w];
            size_t len = (size_t)c.v->waves[w].width;
            if (pool.size() + len >= (1ull << 40) || len >= (1u << 24)) return;
            c.v->waves[w].ch.push_back(Change{t, pool.size(), len});
            pool.append((const char*)val, len);
        }, &cx, nullptr);
        end = fstReaderGetEndTime(ctx);
        int ts = fstReaderGetTimescale(ctx);
        static const char* unit[] = {"s", "ms", "us", "ns", "ps", "fs"};
        if (int k = -ts; k >= 0 && k <= 15) {   // 10^ts seconds
            int idx = (k + 2) / 3, pw = 3 * idx - k;
            timescale = std::string(pw == 0 ? "1" : pw == 1 ? "10" : "100") + unit[idx];
        }
        fstReaderClose(ctx);
        for (size_t w = 0; w < waves.size(); w++) waves[w].base = pools[w].data();
        compact(threads);
        return true;
    }
#endif

    // By what the file is, not by its name.
    bool open(const std::string& file, unsigned threads, std::string& err) {
        std::ifstream in(file, std::ios::binary);
        if (!in) { err = "cannot open " + file; return false; }
        int c = in.get();
        in.close();
        if (c == '$' || c == '#' || c == ' ' || c == '\n' || c == '\t' || c == '\r') return load(file, threads, err);
#ifdef MINISIM_FST
        return loadFst(file, threads, err);
#else
        err = file + " is not a VCD (this mdebug was built without FST)";
        return false;
#endif
    }

    const Wave* find(const std::string& name) const {
        auto it = byName.find(name);
        return it == byName.end() ? nullptr : &waves[it->second];
    }
};

inline std::string showBits(const std::string& bits) {
    if (bits.size() == 1) return bits;
    bool unk = bits.find_first_of("xz") != std::string::npos;
    if (unk) return std::to_string(bits.size()) + "'b" + bits;
    std::string hex;
    size_t n = bits.size();
    for (size_t i = 0; i < n;) {
        size_t take = (n - i) % 4 ? (n - i) % 4 : 4;
        int v = 0;
        for (size_t k = 0; k < take; k++) v = v * 2 + (bits[i + k] == '1');
        hex += "0123456789abcdef"[v];
        i += take;
    }
    return std::to_string(n) + "'h" + hex;
}

}  // namespace wave
