// edt.hpp - scan test compression for mdft (embedded deterministic test).
//
// A few tester channels feed many scan chains through a decompressor: a ring
// generator (an LFSR that the channels are injected into every shift cycle)
// and a phase shifter (one XOR of three generator bits per chain). The chain
// outputs go back through an XOR compactor to as many output channels.
//
//   mdft -edt design_scan.v -top M -scandef chains.json   writes M_edt.sv:
//       M_edt_decomp, M_edt_comp and the wrapper M_edt
//   -edt_cubes cubes.txt -edt_patterns out.txt            encodes test cubes
//   mdft -edt_encode -scandef ... -edt_cubes ...          encodes only
//
// Encoding: every scan cell is a linear function over GF(2) of the channel
// bits of one pattern, so a test cube (the cells ATPG cares about, the rest
// x) is a system of linear equations, one per care bit. Gaussian elimination
// solves it; the free variables are filled at random. Cubes are independent,
// so they are encoded in parallel, and the result does not depend on the
// thread count.
//
// One pattern on the tester: one clock with edt_update high (the generator
// clears; this is the capture cycle), then T = longest chain + init cycles
// shift clocks. Channel bit (k, u) is what channel k carries in shift cycle u.
#pragma once
#include <atomic>
#include "../parallel.hpp"

#include "lbist.hpp"

namespace dft {

struct EdtOpts {
    std::string scandef, outDir = ".", cubes, patterns, fill = "random";
    int channels = 0, ring = 0, threads = 0, tbPatterns = 8;
};

struct EdtPlan {
    ScanDef sd;
    int n = 0, len = 0, c = 0, r = 0, init = 0, T = 0;
    uint64_t poly = 0, mask = 0;
    std::vector<uint64_t> phase;  // per chain: the generator bits it XORs
    std::vector<int> pos;         // per channel: where it is injected

    void plan(const EdtOpts& o) {
        n = (int)sd.chains.size();
        for (const ScanChain& ch : sd.chains) len = std::max(len, ch.length);
        c = o.channels ? o.channels : std::min(32, std::max(1, (n + 9) / 10));
        if (c < 1 || c > 32) throw Error("-edt_channels must be 1 to 32");
        r = o.ring ? o.ring : (c <= 8 ? 32 : 64);
        if (r < 8 || r > 64) throw Error("-edt_ring must be 8 to 64");
        if (c * 2 > r) throw Error("-edt_ring " + std::to_string(r) + " is too short for " + std::to_string(c) + " channels");
        poly = gf2::primitivePoly(r);
        mask = r == 64 ? ~0ull : ((1ull << r) - 1);
        init = (r + c - 1) / c;  // cycles until the generator can hold any state
        T = len + init;
        for (int k = 0; k < c; k++) pos.push_back((int)((long)k * r / c));
        std::mt19937_64 rng(0xC2B2AE3D27D4EB4Full ^ (uint64_t)r);
        std::set<uint64_t> used;
        for (int i = 0; i < n; i++) {
            for (int tries = 0;; tries++) {
                int a = (int)(rng() % r), b = (int)(rng() % r), d = (int)(rng() % r);
                if (a == b || b == d || a == d) continue;
                uint64_t m = (1ull << a) | (1ull << b) | (1ull << d);
                if (used.insert(m).second || tries > 1000) {
                    phase.push_back(m);
                    break;
                }
            }
        }
    }
    uint64_t step(uint64_t s) const { return ((s << 1) & mask) ^ (((s >> (r - 1)) & 1) ? poly : 0); }
    uint64_t inject(const std::vector<std::string>& ch, int u) const {
        uint64_t v = 0;
        for (int k = 0; k < c; k++)
            if (u < (int)ch[k].size() && ch[k][u] == '1') v |= 1ull << pos[k];
        return v;
    }
    // What the decompressor puts on each chain's scan input in each of
    // `cycles` shift cycles (channels are 0 past their end).
    std::vector<std::string> expand(const std::vector<std::string>& ch, int cycles) const {
        std::vector<std::string> si(n, std::string(cycles, '0'));
        uint64_t g = 0;
        for (int t = 0; t < cycles; t++) {
            for (int i = 0; i < n; i++)
                if (__builtin_popcountll(g & phase[i]) & 1) si[i][t] = '1';
            g = step(g) ^ inject(ch, t);
        }
        return si;
    }
    // The chain contents after T shift cycles, scan-in cell first.
    std::vector<std::string> loads(const std::vector<std::string>& ch) const {
        std::vector<std::string> si = expand(ch, T), out(n);
        for (int i = 0; i < n; i++)
            for (int j = 0; j < sd.chains[i].length; j++) out[i] += si[i][T - 1 - j];
        return out;
    }
};

// A cube set: per pattern, per chain, '0' '1' 'x' by cell from scan-in.
struct CubeSet {
    std::vector<std::vector<std::string>> cubes;
    std::vector<long> ids;
    std::map<long, std::string> pi;   // port input bits of a pattern, passed through
    bool numbered = false;            // chains were given by number without a "chains" line
};
inline CubeSet readCubes(const std::string& path, const EdtPlan& p) {
    std::ifstream f(path);
    if (!f) throw Error("cannot read " + path);
    std::map<std::string, int> chainIdx;
    for (int i = 0; i < p.n; i++) chainIdx[p.sd.chains[i].name] = i;
    CubeSet cs;
    std::map<long, int> where;
    // matpg -cubes numbers the chains in its own order and says which is
    // which in a "chains <scan-in port> ..." line.
    std::vector<int> byNumber;
    std::string line;
    int ln = 0;
    while (std::getline(f, line)) {
        ln++;
        size_t h = line.find('#');
        if (h != std::string::npos) line.resize(h);
        std::istringstream is(line);
        std::string first, chain, bits;
        if (!(is >> first)) continue;
        auto bad = [&](const std::string& why) { throw Error(path + ":" + std::to_string(ln) + ": " + why); };
        if (first == "chains") {
            while (is >> chain) {
                int at = -1;
                for (int i = 0; i < p.n; i++)
                    if (p.sd.chains[i].scanIn == chain) at = i;
                if (at < 0) bad("no chain starts at port '" + chain + "' in the scan description");
                byNumber.push_back(at);
            }
            continue;
        }
        long id = 0;
        try {
            size_t used = 0;
            id = std::stol(first, &used);
            if (used != first.size()) throw 0;
        } catch (...) {
            bad("expected '<pattern> <chain> <bits>'");
        }
        if (!(is >> chain >> bits)) bad("expected '<pattern> <chain> <bits>'");
        if (chain == "pi") {
            cs.pi[id] = bits;
            continue;
        }
        auto it = chainIdx.find(chain);
        if (it == chainIdx.end() && !chain.empty() && chain.find_first_not_of("0123456789") == std::string::npos) {
            // A number: through the "chains" line, else the order of the scan description.
            size_t k = std::stoul(chain);
            if (!byNumber.empty() && k < byNumber.size()) it = chainIdx.find(p.sd.chains[byNumber[k]].name);
            else if (byNumber.empty() && k < (size_t)p.n) {
                if (!cs.numbered)
                    std::cerr << "mdft: " << path << ": chains are numbered and there is no 'chains' line; "
                              << "taking the order of the scan description\n";
                cs.numbered = true;
                it = chainIdx.find(p.sd.chains[k].name);
            }
        }
        if (it == chainIdx.end()) bad("no chain '" + chain + "'");
        int want = p.sd.chains[it->second].length;
        if ((int)bits.size() != want)
            throw Error(path + ":" + std::to_string(ln) + ": chain " + chain + " has " + std::to_string(want) + " cells, the line has " +
                        std::to_string(bits.size()));
        for (char& b : bits) {
            if (b == 'X' || b == '-') b = 'x';
            if (b != '0' && b != '1' && b != 'x') throw Error(path + ":" + std::to_string(ln) + ": bits must be 0, 1 or x");
        }
        auto [w, fresh] = where.emplace(id, (int)cs.cubes.size());
        if (fresh) {
            cs.ids.push_back(id);
            cs.cubes.emplace_back(p.n);
            for (int i = 0; i < p.n; i++) cs.cubes.back()[i].assign(p.sd.chains[i].length, 'x');
        }
        cs.cubes[w->second][it->second] = bits;
    }
    return cs;
}

struct EdtEncoder {
    const EdtPlan& p;
    int tv = 0, words = 0, vars = 0;
    // x[i][k]: bit m = whether channel k's bit, injected Tv-1-m steps before, reaches chain i.
    std::vector<std::vector<uint64_t>> x;  // [i * c + k] -> tv bits

    explicit EdtEncoder(const EdtPlan& plan) : p(plan) {
        tv = p.T - 1;
        vars = tv * p.c;
        words = (vars + 63) / 64;
        int tw = (tv + 63) / 64;
        x.assign((size_t)p.n * p.c, std::vector<uint64_t>(tw + 1, 0));
        for (int k = 0; k < p.c; k++) {
            uint64_t s = 1ull << p.pos[k];
            for (int d = 0; d < tv; d++) {
                int m = tv - 1 - d;
                for (int i = 0; i < p.n; i++)
                    if (__builtin_popcountll(s & p.phase[i]) & 1) x[(size_t)i * p.c + k][m >> 6] |= 1ull << (m & 63);
                s = p.step(s);
            }
        }
    }
    // dst[dstOff .. dstOff+len) |= src[srcOff .. srcOff+len)
    static void orBits(uint64_t* dst, int dstOff, const uint64_t* src, int srcOff, int len) {
        while (len > 0) {
            int sw = srcOff >> 6, sb = srcOff & 63, dw = dstOff >> 6, db = dstOff & 63;
            int take = std::min({len, 64 - sb, 64 - db});
            uint64_t bits = (src[sw] >> sb) & (take == 64 ? ~0ull : ((1ull << take) - 1));
            dst[dw] |= bits << db;
            srcOff += take, dstOff += take, len -= take;
        }
    }
    // The equation of the cell that chain i loads in shift cycle t.
    void row(int i, int t, uint64_t* out) const {
        std::fill(out, out + words, 0);
        for (int k = 0; k < p.c; k++) orBits(out, k * tv, x[(size_t)i * p.c + k].data(), tv - t, t);
    }

    struct Result {
        bool ok = false;
        int care = 0;
        std::vector<std::string> channels;  // per channel: T bits, first shifted first
    };
    Result encode(const std::vector<std::string>& cube, uint64_t fillSeed, bool randomFill) const {
        Result res;
        std::vector<std::vector<uint64_t>> piv;
        std::vector<int> pcol;
        std::vector<char> prhs;
        std::vector<uint64_t> cur(words);
        for (const std::string& bits : cube)
            for (char b : bits) res.care += b != 'x';
        for (int i = 0; i < p.n; i++) {
            const std::string& bits = cube[i];
            for (int j = 0; j < (int)bits.size(); j++) {
                if (bits[j] == 'x') continue;
                row(i, p.T - 1 - j, cur.data());
                char rhs = bits[j] == '1';
                for (size_t q = 0; q < piv.size(); q++) {
                    if (!((cur[pcol[q] >> 6] >> (pcol[q] & 63)) & 1)) continue;
                    const uint64_t* pr = piv[q].data();
                    for (int w = pcol[q] >> 6; w < words; w++) cur[w] ^= pr[w];
                    rhs ^= prhs[q];
                }
                int col = -1;
                for (int w = 0; w < words && col < 0; w++)
                    if (cur[w]) col = w * 64 + __builtin_ctzll(cur[w]);
                if (col < 0) {
                    if (rhs) return res;  // the care bits contradict each other through the decompressor
                    continue;
                }
                piv.push_back(cur);
                pcol.push_back(col);
                prhs.push_back(rhs);
            }
        }
        std::vector<uint64_t> v(words, 0);
        if (randomFill) {
            std::mt19937_64 rng(fillSeed);
            for (uint64_t& w : v) w = rng();
            if (vars & 63) v[words - 1] &= (1ull << (vars & 63)) - 1;
            for (int col : pcol) v[col >> 6] &= ~(1ull << (col & 63));
        }
        for (size_t q = piv.size(); q-- > 0;) {
            int par = prhs[q];
            const uint64_t* pr = piv[q].data();
            for (int w = pcol[q] >> 6; w < words; w++) par ^= __builtin_popcountll(pr[w] & v[w]) & 1;
            if (par & 1) v[pcol[q] >> 6] |= 1ull << (pcol[q] & 63);
        }
        res.channels.assign(p.c, std::string(p.T, '0'));
        for (int k = 0; k < p.c; k++)
            for (int u = 0; u < tv; u++) {
                int b = k * tv + u;
                if ((v[b >> 6] >> (b & 63)) & 1) res.channels[k][u] = '1';
            }
        res.ok = true;
        return res;
    }
};

struct EdtStats {
    int total = 0, encoded = 0, failed = 0;
    long care = 0, maxCare = 0;
    double seconds = 0;
    int threads = 1;
};

// Encodes every cube; results[i] lines up with cs.cubes[i].
inline std::vector<EdtEncoder::Result> encodeAll(const EdtPlan& p, const CubeSet& cs, const EdtOpts& o, EdtStats& st) {
    EdtEncoder enc(p);
    std::vector<EdtEncoder::Result> out(cs.cubes.size());
    int nt = o.threads > 0 ? o.threads : (int)par::defaultThreads();
    nt = std::max(1, std::min<int>(nt, (int)cs.cubes.size()));
    bool rnd = o.fill != "0" && o.fill != "zero";
    auto t0 = std::chrono::steady_clock::now();
    std::atomic<bool> bad{false};
    par::parallelFor(cs.cubes.size(), (unsigned)nt, [&](size_t i) {
        if (bad) return;
        // The fill depends on the pattern alone, so any thread count gives the same answer.
        out[i] = enc.encode(cs.cubes[i], 0x51ED270B9F3A1C47ull ^ ((uint64_t)cs.ids[i] * 0x9E3779B97F4A7C15ull), rnd);
        if (out[i].ok) {
            std::vector<std::string> ld = p.loads(out[i].channels);
            for (int ch = 0; ch < p.n && !bad; ch++)
                for (size_t j = 0; j < ld[ch].size(); j++)
                    if (cs.cubes[i][ch][j] != 'x' && cs.cubes[i][ch][j] != ld[ch][j]) bad = true;
        }
    });
    if (bad) throw Error("internal error: a decompressed pattern does not match its cube");
    st.total = (int)cs.cubes.size(), st.threads = nt;
    for (const auto& r : out) {
        (r.ok ? st.encoded : st.failed)++;
        st.care += r.care;
        st.maxCare = std::max<long>(st.maxCare, r.care);
    }
    st.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

inline void writeEdtPatterns(const std::string& path, const EdtPlan& p, const CubeSet& cs,
                             const std::vector<EdtEncoder::Result>& res) {
    std::ostringstream os;
    os << "# mdft EDT patterns: " << p.c << " channels, " << p.T << " shift cycles per pattern (" << p.init
       << " init + " << p.len << ")\n# <pattern> in <channel> <bits, first shifted first>\n"
       << "# <pattern> load <chain> <bits, scan-in cell first>\n# <pattern> pi <port input bits, as given with the cubes>\n";
    for (size_t i = 0; i < res.size(); i++) {
        if (!res[i].ok) {
            os << cs.ids[i] << " unencodable\n";
            continue;
        }
        if (auto pi = cs.pi.find(cs.ids[i]); pi != cs.pi.end()) os << cs.ids[i] << " pi " << pi->second << "\n";
        for (int k = 0; k < p.c; k++) os << cs.ids[i] << " in " << k << " " << res[i].channels[k] << "\n";
        std::vector<std::string> ld = p.loads(res[i].channels);
        for (int ch = 0; ch < p.n; ch++) os << cs.ids[i] << " load " << p.sd.chains[ch].name << " " << ld[ch] << "\n";
    }
    writeFile(path, os.str());
}

struct EdtResult {
    std::string rtlPath, tbPath, wrap;
};

// The hardware, and a testbench that plays `tbCh` (channel bits per pattern)
// through the wrapper in shift mode and checks the output channels.
inline EdtResult insertEdt(const std::vector<Port>& ports, const std::string& top, const EdtOpts& o, const EdtPlan& p,
                           const std::vector<std::vector<std::string>>& tbCh) {
    using detail::hex;
    using detail::portBit;
    EdtResult r;
    std::map<std::string, const Port*> byName;
    for (const Port& q : ports) byName[q.name] = &q;
    for (const char* t : {"edt_mode", "edt_update", "edt_channels_in", "edt_channels_out"})
        if (byName.count(t)) throw Error(std::string("the design already has a port named '") + t + "'");
    auto need = [&](const std::string& ref, Port::Dir d, const char* what) -> std::pair<const Port*, int> {
        auto [name, bit] = portBit(ref);
        auto it = byName.find(name);
        if (it == byName.end()) throw Error(std::string(what) + " '" + ref + "' is not a port of " + top);
        const Port& q = *it->second;
        if (q.dir != d) throw Error(std::string(what) + " '" + ref + "' has the wrong direction");
        if (bit < 0 && q.width() != 1) throw Error(std::string(what) + " '" + ref + "' is a vector: name one bit");
        int b = bit < 0 ? 0 : std::abs(bit - q.lsb);
        if (b >= q.width()) throw Error(std::string(what) + " '" + ref + "' is out of range");
        return {&q, b};
    };
    if (p.sd.clocks.empty()) throw Error("the scan description lists no clock");
    auto [ckPort, ckBit] = need(p.sd.clocks[0], Port::In, "the clock");
    auto [sePort, seBit] = need(p.sd.scanEn, Port::In, "the scan enable");
    std::string clk = ckPort->bit(ckBit), se = std::string(p.sd.scanEnHigh ? "" : "~") + sePort->bit(seBit);
    std::map<std::pair<std::string, int>, int> siOf;
    std::vector<std::string> soExpr;
    for (int i = 0; i < p.n; i++) {
        auto [q, b] = need(p.sd.chains[i].scanIn, Port::In, "the scan input");
        siOf[{q->name, b}] = i;
        auto [so, sb] = need(p.sd.chains[i].scanOut, Port::Out, "the scan output");
        soExpr.push_back(so->bit(sb));
    }
    r.wrap = top + "_edt";
    std::ostringstream rtl;
    rtl << "// Scan decompressor, written by mdft: " << p.c << " channels to " << p.n << " chains, " << p.r
        << "-bit ring generator (polynomial low part " << hex(p.poly, p.r) << ")\n"
        << "module " << r.wrap << "_decomp (\n  input  logic clk, shift, update,\n  input  logic [" << p.c - 1
        << ":0] channels,\n  output logic [" << p.n - 1 << ":0] scan_in);\n"
        << "  logic [" << p.r - 1 << ":0] gen;\n  wire [" << p.r - 1 << ":0] inj;\n";
    for (int b = 0; b < p.r; b++) {
        int k = -1;
        for (int q = 0; q < p.c; q++)
            if (p.pos[q] == b) k = q;
        rtl << "  assign inj[" << b << "] = " << (k < 0 ? "1'b0" : "channels[" + std::to_string(k) + "]") << ";\n";
    }
    rtl << "  always_ff @(posedge clk) begin\n    if (update) gen <= '0;\n    else if (shift) gen <= ({gen[" << p.r - 2
        << ":0], 1'b0} ^ (gen[" << p.r - 1 << "] ? " << hex(p.poly, p.r) << " : " << p.r << "'b0)) ^ inj;\n  end\n";
    for (int i = 0; i < p.n; i++) rtl << "  assign scan_in[" << i << "] = ^(gen & " << hex(p.phase[i], p.r) << ");\n";
    rtl << "endmodule\n\n// Scan compactor: chain i goes to output channel i mod " << p.c << "\nmodule " << r.wrap
        << "_comp (\n  input  logic [" << p.n - 1 << ":0] scan_out,\n  output logic [" << p.c - 1 << ":0] channels);\n";
    for (int k = 0; k < p.c; k++) {
        std::string xs;
        for (int i = k; i < p.n; i += p.c) xs += (xs.empty() ? "" : " ^ ") + ("scan_out[" + std::to_string(i) + "]");
        rtl << "  assign channels[" << k << "] = " << (xs.empty() ? "1'b0" : xs) << ";\n";
    }
    rtl << "endmodule\n\n// " << top << " with scan compression. edt_mode 0: the scan pins work as before.\nmodule " << r.wrap
        << " (\n";
    for (const Port& q : ports)
        rtl << "  " << (q.dir == Port::In ? "input  logic " : q.dir == Port::Out ? "output logic " : "inout  wire  ") << q.range()
            << q.name << ",\n";
    rtl << "  input  logic edt_mode, edt_update,\n  input  logic [" << p.c - 1 << ":0] edt_channels_in,\n  output logic ["
        << p.c - 1 << ":0] edt_channels_out);\n  wire [" << p.n - 1 << ":0] dec_si, core_so;\n";
    std::ostringstream conn;
    bool first = true;
    for (const Port& q : ports) {
        conn << (first ? "" : ",\n") << "    ." << q.name << "(";
        first = false;
        bool hasSi = false;
        for (int b = 0; b < q.width(); b++) hasSi |= siOf.count({q.name, b}) > 0;
        if (!hasSi) {
            conn << q.name << ")";
            continue;
        }
        conn << "core_" << q.name << ")";
        rtl << "  wire " << q.range() << "core_" << q.name << ";\n";
        for (int b = 0; b < q.width(); b++) {
            auto it = siOf.find({q.name, b});
            if (it == siOf.end()) rtl << "  assign core_" << q.bit(b) << " = " << q.bit(b) << ";\n";
            else rtl << "  assign core_" << q.bit(b) << " = edt_mode ? dec_si[" << it->second << "] : " << q.bit(b) << ";\n";
        }
    }
    for (int i = 0; i < p.n; i++) rtl << "  assign core_so[" << i << "] = " << soExpr[i] << ";\n";
    rtl << "  " << top << " u_core (\n" << conn.str() << ");\n"
        << "  " << r.wrap << "_decomp u_decomp (.clk(" << clk << "), .shift(" << se
        << "), .update(edt_update), .channels(edt_channels_in), .scan_in(dec_si));\n"
        << "  " << r.wrap << "_comp u_comp (.scan_out(core_so), .channels(edt_channels_out));\nendmodule\n";

    // The testbench: shift mode only. A chain is a shift register, so what
    // leaves it is what the decompressor put in `length` cycles earlier.
    std::ostringstream tb;
    std::string tbName = r.wrap + "_tb";
    int total = p.T + p.len;
    tb << "// " << tbName << " - plays compressed patterns in shift mode and checks the output channels, written by mdft\n"
       << "`timescale 1ns/1ps\nmodule " << tbName << ";\n";
    for (const Port& q : ports) {
        if (q.dir == Port::In) tb << "  logic " << q.range() << q.name << " = '0;\n";
        else tb << "  wire " << q.range() << q.name << ";\n";
    }
    tb << "  logic edt_mode = 1, edt_update = 0;\n  logic [" << p.c - 1 << ":0] edt_channels_in = '0;\n  wire [" << p.c - 1
       << ":0] edt_channels_out;\n  int errors = 0, checks = 0;\n  " << r.wrap << " dut (.*);\n"
       << "  task automatic cyc(input logic [" << p.c - 1 << ":0] in, input logic [" << p.c - 1 << ":0] exp, input logic ["
       << p.c - 1 << ":0] care);\n"
       << "    edt_channels_in = in; #4;\n"
       << "    for (int k = 0; k < " << p.c << "; k++) if (care[k]) begin\n      checks++;\n"
       << "      if (edt_channels_out[k] !== exp[k]) begin\n        errors++;\n"
       << "        if (errors <= 20) $display(\"MISMATCH at %0t: channel %0d = %b, expected %b\", $time, k, edt_channels_out[k], exp[k]);\n"
       << "      end\n    end\n    #1 " << clk << " = 1; #5 " << clk << " = 0;\n  endtask\n  initial begin\n";
    for (auto& [port, high] : p.sd.resets) tb << "    " << port << " = " << (high ? "1'b0" : "1'b1") << ";\n";
    tb << "    " << sePort->bit(seBit) << " = " << (p.sd.scanEnHigh ? "1'b1" : "1'b0") << ";\n";
    for (size_t k = 0; k < tbCh.size(); k++) {
        std::vector<std::string> si = p.expand(tbCh[k], total);
        tb << "    // pattern " << k << "\n    edt_update = 1; cyc('0, '0, '0); edt_update = 0;\n";
        for (int t = 0; t < total; t++) {
            std::string in(p.c, '0'), exp(p.c, '0'), care(p.c, '0');
            for (int c = 0; c < p.c; c++) {
                if (t < p.T && tbCh[k][c][t] == '1') in[c] = '1';
                bool known = false, v = false;
                bool all = true;
                for (int i = c; i < p.n; i += p.c) {
                    int back = t - p.sd.chains[i].length;
                    if (back < 0) all = false;
                    else v ^= si[i][back] == '1';
                    known = true;
                }
                if (known && all) care[c] = '1', exp[c] = v ? '1' : '0';
            }
            tb << "    cyc(" << vlog(in) << ", " << vlog(exp) << ", " << vlog(care) << ");\n";
        }
    }
    tb << "    if (errors == 0 && checks > 0) $display(\"" << tbName << ": PASS (%0d checks)\", checks);\n"
       << "    else $display(\"" << tbName << ": FAIL (%0d mismatches)\", errors);\n    $finish;\n  end\nendmodule\n";
    r.rtlPath = o.outDir + "/" + r.wrap + ".sv";
    r.tbPath = o.outDir + "/" + tbName + ".sv";
    writeFile(r.rtlPath, rtl.str());
    writeFile(r.tbPath, tb.str());
    return r;
}

}  // namespace dft
