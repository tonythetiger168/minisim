// emu/base.hpp - small shared pieces of memu: errors, a deterministic random
// number generator, a parallel-for, and the board and memory description
// readers.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace emu {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// splitmix64: the same sequence on every platform and standard library, so a
// partition does not depend on where memu was built.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t below(uint32_t n) { return n ? (uint32_t)(next() % n) : 0; }
    template <class T>
    void shuffle(std::vector<T>& v) {
        for (size_t i = v.size(); i > 1; i--) std::swap(v[i - 1], v[below((uint32_t)i)]);
    }
};

// Runs f(0..n-1) on up to `threads` threads. Each index is its own piece of
// work with its own output, so the result does not depend on the thread
// count. (Moves to src/parallel.hpp, the tool line's shared pool, when that
// header is on main.)
template <class F>
void parallelFor(size_t n, unsigned threads, F&& f) {
    if (threads <= 1 || n <= 1) {
        for (size_t i = 0; i < n; i++) f(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::string> errs(n);
    auto work = [&] {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= n) return;
            try {
                f(i);
            } catch (const std::exception& e) {
                errs[i] = e.what();
                if (errs[i].empty()) errs[i] = "error";
            }
        }
    };
    unsigned k = (unsigned)std::min<size_t>(threads, n);
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < k; t++) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    for (auto& e : errs)
        if (!e.empty()) throw Error(e);
}

inline std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw Error("cannot read " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A line of a board or memory file: a keyword, plain words, and key=value pairs.
struct ConfLine {
    std::string kw;
    std::vector<std::string> words;
    std::map<std::string, std::string> kv;
    int line = 0;
    bool has(const std::string& k) const { return kv.count(k) != 0; }
    std::string str(const std::string& k, const std::string& dflt = "") const {
        auto it = kv.find(k);
        return it == kv.end() ? dflt : it->second;
    }
    double num(const std::string& k, double dflt = 0) const {
        auto it = kv.find(k);
        return it == kv.end() ? dflt : std::atof(it->second.c_str());
    }
};

inline std::vector<ConfLine> readConf(const std::string& path) {
    std::vector<ConfLine> out;
    std::istringstream in(readFile(path));
    std::string ln;
    int no = 0;
    while (std::getline(in, ln)) {
        no++;
        auto h = ln.find('#');
        if (h != std::string::npos) ln.resize(h);
        std::istringstream ls(ln);
        std::string w;
        ConfLine c;
        c.line = no;
        while (ls >> w) {
            if (c.kw.empty()) {
                c.kw = w;
                continue;
            }
            auto eq = w.find('=');
            if (eq == std::string::npos) c.words.push_back(w);
            else c.kv[w.substr(0, eq)] = w.substr(eq + 1);
        }
        if (!c.kw.empty()) out.push_back(std::move(c));
    }
    return out;
}

// ------------------------------------------------------------------- board

struct Fpga {
    std::string name;
    double lut = 0, ff = 0, bramKb = 0;  // 0: not limited
};

struct Link {
    int a = 0, b = 0, pins = 0;
};

// The FPGAs of a board and the wires between them.
//
//   board quad
//   util 0.7                # fill each FPGA to at most 70%
//   tdm_max 16              # most signals one wire may carry in turn
//   fpga F0 lut=200000 ff=400000 bram_kb=16000
//   fpga F1 lut=200000 ff=400000 bram_kb=16000
//   link F0 F1 pins=120
struct Board {
    std::string name = "board";
    std::vector<Fpga> fpgas;
    std::vector<Link> links;
    double util = 0.7;
    int tdmMax = 16;
    std::vector<std::vector<int>> dist, hop, linkOf;  // hop[a][b]: the next FPGA on the way from a to b

    int k() const { return (int)fpgas.size(); }

    static Board uniform(int n, double lut, double ff, double bramKb, int pins) {
        Board b;
        b.name = "uniform";
        for (int i = 0; i < n; i++) b.fpgas.push_back({"F" + std::to_string(i), lut, ff, bramKb});
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) b.links.push_back({i, j, pins});
        b.finish();
        return b;
    }

    static Board load(const std::string& path) {
        Board b;
        std::map<std::string, int> idx;
        for (auto& c : readConf(path)) {
            auto where = path + ":" + std::to_string(c.line) + ": ";
            if (c.kw == "board") {
                if (!c.words.empty()) b.name = c.words[0];
            } else if (c.kw == "util") {
                b.util = c.words.empty() ? 0.7 : std::atof(c.words[0].c_str());
                if (b.util <= 0 || b.util > 1) throw Error(where + "util is a fraction, above 0 and at most 1");
            } else if (c.kw == "tdm_max") {
                b.tdmMax = c.words.empty() ? 16 : std::atoi(c.words[0].c_str());
                if (b.tdmMax < 1) throw Error(where + "tdm_max is at least 1");
            } else if (c.kw == "fpga") {
                if (c.words.empty()) throw Error(where + "fpga needs a name");
                if (idx.count(c.words[0])) throw Error(where + "fpga " + c.words[0] + " is defined twice");
                idx[c.words[0]] = (int)b.fpgas.size();
                b.fpgas.push_back({c.words[0], c.num("lut"), c.num("ff"), c.num("bram_kb")});
            } else if (c.kw == "link") {
                if (c.words.size() != 2 || !idx.count(c.words[0]) || !idx.count(c.words[1]))
                    throw Error(where + "link names two fpgas defined above it");
                int p = (int)c.num("pins");
                if (p < 1) throw Error(where + "link needs pins=N");
                if (idx[c.words[0]] == idx[c.words[1]]) throw Error(where + "a link joins two different fpgas");
                b.links.push_back({idx[c.words[0]], idx[c.words[1]], p});
            } else {
                throw Error(where + "unknown keyword '" + c.kw + "'");
            }
        }
        if (b.fpgas.empty()) throw Error(path + ": no fpga lines");
        b.finish();
        return b;
    }

    // The FPGA before v on the route from a: the lowest-numbered neighbour of
    // v that is one hop closer to a. Routes from a therefore form a tree.
    int prev(int a, int v) const {
        for (int u = 0; u < k(); u++)
            if (linkOf[u][v] >= 0 && dist[a][u] == dist[a][v] - 1) return u;
        return a;
    }

    // Shortest routes between FPGAs, by hop count, lowest FPGA index first.
    void finish() {
        int n = k();
        const int inf = 1 << 20;
        dist.assign(n, std::vector<int>(n, inf));
        hop.assign(n, std::vector<int>(n, -1));
        linkOf.assign(n, std::vector<int>(n, -1));
        std::vector<Link> merged;
        for (auto& l : links) {
            if (linkOf[l.a][l.b] >= 0) {
                merged[linkOf[l.a][l.b]].pins += l.pins;  // two lines for one pair add up
                continue;
            }
            linkOf[l.a][l.b] = linkOf[l.b][l.a] = (int)merged.size();
            merged.push_back(l);
        }
        links = merged;
        for (int s = 0; s < n; s++) {
            dist[s][s] = 0;
            hop[s][s] = s;
            std::vector<int> q{s};
            for (size_t h = 0; h < q.size(); h++) {
                int u = q[h];
                for (int v = 0; v < n; v++)
                    if (linkOf[u][v] >= 0 && dist[s][v] == inf) {
                        dist[s][v] = dist[s][u] + 1;
                        q.push_back(v);
                    }
            }
        }
        // hop[a][b]: the lowest-numbered neighbour of a that is one step closer to b.
        for (int a = 0; a < n; a++)
            for (int b = 0; b < n; b++) {
                if (a == b || dist[a][b] >= inf) continue;
                for (int v = 0; v < n; v++)
                    if (linkOf[a][v] >= 0 && dist[v][b] == dist[a][b] - 1) {
                        hop[a][b] = v;
                        break;
                    }
            }
    }
};

// ---------------------------------------------------------------- memories

// One port of a memory cell. A pin name with a leading '!' is active low.
struct MemPort {
    std::string clk, addr, din, dout, we, cs, re, wmask;
    bool clkNeg = false, weNeg = false, csNeg = false, reNeg = false;
};

// A memory cell of the netlist and how its pins behave, so memu can put an
// FPGA memory in its place.
//
//   memory SRAM64X8 depth=64 width=8 init=boot.hex
//   port clk=CLK addr=A din=D dout=Q we=WE cs=!CSN     # write, or read when we is low
//   port addr=RA dout=RQ                               # read without a clock
struct MemSpec {
    std::string cell, init;
    int depth = 0, width = 0, abits = 1;
    std::vector<MemPort> ports;
    bool lagRead = false;  // a clockless read moved onto emu_clk (block RAM)
    long bits() const { return (long)depth * width; }
    bool asyncRead() const {
        for (auto& p : ports)
            if (p.clk.empty()) return true;
        return false;
    }
};

inline std::vector<MemSpec> loadMemSpecs(const std::string& path) {
    std::vector<MemSpec> out;
    auto pinOf = [](const std::string& v, bool& neg) {
        neg = !v.empty() && v[0] == '!';
        return neg ? v.substr(1) : v;
    };
    for (auto& c : readConf(path)) {
        auto where = path + ":" + std::to_string(c.line) + ": ";
        if (c.kw == "memory") {
            if (c.words.empty()) throw Error(where + "memory needs the cell name");
            MemSpec m;
            m.cell = c.words[0];
            m.depth = (int)c.num("depth");
            m.width = (int)c.num("width");
            m.init = c.str("init");
            if (m.depth < 1 || m.width < 1) throw Error(where + "memory needs depth=N width=N");
            while ((1L << m.abits) < m.depth) m.abits++;
            out.push_back(std::move(m));
        } else if (c.kw == "port") {
            if (out.empty()) throw Error(where + "port comes after a memory line");
            MemPort p;
            bool dummy = false;
            p.clk = pinOf(c.str("clk"), p.clkNeg);
            p.addr = pinOf(c.str("addr"), dummy);
            p.din = pinOf(c.str("din"), dummy);
            p.dout = pinOf(c.str("dout"), dummy);
            p.wmask = pinOf(c.str("wmask"), dummy);
            p.we = pinOf(c.str("we"), p.weNeg);
            p.cs = pinOf(c.str("cs"), p.csNeg);
            p.re = pinOf(c.str("re"), p.reNeg);
            if (p.addr.empty()) throw Error(where + "port needs addr=PIN");
            if (p.din.empty() && p.dout.empty()) throw Error(where + "port needs din=PIN or dout=PIN");
            if (!p.din.empty() && p.clk.empty()) throw Error(where + "a port that writes needs clk=PIN");
            if (!p.din.empty() && p.we.empty() && !p.dout.empty())
                throw Error(where + "a port with din and dout needs we=PIN to tell a write from a read");
            for (auto& [k, v] : c.kv)
                if (k != "clk" && k != "addr" && k != "din" && k != "dout" && k != "wmask" && k != "we" &&
                    k != "cs" && k != "re")
                    throw Error(where + "unknown port key '" + k + "'");
            out.back().ports.push_back(std::move(p));
        } else {
            throw Error(where + "unknown keyword '" + c.kw + "'");
        }
    }
    for (auto& m : out)
        if (m.ports.empty()) throw Error(path + ": memory " + m.cell + " has no port lines");
    return out;
}

// A Verilog identifier, escaped when it has to be.
inline std::string vname(const std::string& n) {
    bool plain = !n.empty() && (isalpha((unsigned char)n[0]) || n[0] == '_');
    for (char c : n)
        if (!(isalnum((unsigned char)c) || c == '_' || c == '$')) plain = false;
    return plain ? n : "\\" + n + " ";
}

// Shell-style match: * and ?.
inline bool globMatch(const char* p, const char* s) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return true;
            for (; *s; s++)
                if (globMatch(p + 1, s)) return true;
            return globMatch(p + 1, s);
        }
        if (!*s || (*p != '?' && *p != *s)) return false;
    }
    return !*s;
}

}  // namespace emu
