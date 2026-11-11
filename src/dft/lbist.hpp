// lbist.hpp - logic built-in self-test for mdft (STUMPS architecture).
//
// From a scan-inserted design and its chain description (the JSON that
// mdft -insert_scan -scandef writes), mdft -lbist writes
//   <top>_lbist_ctrl  a PRPG (LFSR on a primitive polynomial), a phase
//                     shifter (one XOR of three PRPG bits per chain), a space
//                     compactor and a MISR, and the controller that shifts
//                     and captures a fixed number of patterns;
//   <top>_lbist       the design with the controller around it. While
//                     lbist_start is high every clock is the BIST clock, the
//                     resets are held off, the other inputs are held at 0 and
//                     the chains are fed by the PRPG;
//   <top>_lbist.icl / .pdl   IJTAG access: start, done, pass and signature.
// The golden signature comes from simulating the wrapper (-lbist_sign runs
// msim and writes the value back into the RTL). A signature with unknown
// bits means an X source reaches the chains.
//
// The polynomials are found, not tabulated: a candidate is kept when x has
// order 2^n - 1 modulo it, which makes it primitive.
#pragma once
#include <array>
#include <cstdio>
#include <random>

#include "bscan.hpp"

namespace dft {

// --- a minimal JSON reader ---------------------------------------------------
namespace json {
struct Value {
    enum K { Null, Bool, Num, Str, Arr, Obj } k = Null;
    double num = 0;
    bool b = false;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;
    const Value* get(const std::string& key) const {
        for (auto& [n, v] : obj)
            if (n == key) return &v;
        return nullptr;
    }
    std::string s(const std::string& key, const std::string& def = "") const {
        const Value* v = get(key);
        return v && v->k == Str ? v->str : def;
    }
    long n(const std::string& key, long def = 0) const {
        const Value* v = get(key);
        return v && v->k == Num ? (long)v->num : def;
    }
};
struct Parser {
    const std::string& t;
    size_t i = 0;
    Error err(const std::string& m) const {
        int line = 1;
        for (size_t k = 0; k < i && k < t.size(); k++)
            if (t[k] == '\n') line++;
        return Error("JSON line " + std::to_string(line) + ": " + m);
    }
    void ws() {
        while (i < t.size() && std::isspace((unsigned char)t[i])) i++;
    }
    Value value() {
        ws();
        if (i >= t.size()) throw err("unexpected end");
        Value v;
        char c = t[i];
        if (c == '{') {
            v.k = Value::Obj;
            i++;
            ws();
            if (i < t.size() && t[i] == '}') { i++; return v; }
            while (true) {
                ws();
                Value key = value();
                if (key.k != Value::Str) throw err("expected a string key");
                ws();
                if (i >= t.size() || t[i] != ':') throw err("expected ':'");
                i++;
                v.obj.push_back({key.str, value()});
                ws();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == '}') { i++; return v; }
                throw err("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.k = Value::Arr;
            i++;
            ws();
            if (i < t.size() && t[i] == ']') { i++; return v; }
            while (true) {
                v.arr.push_back(value());
                ws();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == ']') { i++; return v; }
                throw err("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.k = Value::Str;
            i++;
            while (i < t.size() && t[i] != '"') {
                if (t[i] == '\\' && i + 1 < t.size()) {
                    char e = t[++i];
                    v.str += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                } else v.str += t[i];
                i++;
            }
            if (i >= t.size()) throw err("unterminated string");
            i++;
            return v;
        }
        if (t.compare(i, 4, "true") == 0) { v.k = Value::Bool, v.b = true, i += 4; return v; }
        if (t.compare(i, 5, "false") == 0) { v.k = Value::Bool, i += 5; return v; }
        if (t.compare(i, 4, "null") == 0) { i += 4; return v; }
        char* end = nullptr;
        v.num = std::strtod(t.c_str() + i, &end);
        if (end == t.c_str() + i) throw err(std::string("unexpected '") + c + "'");
        v.k = Value::Num;
        i = end - t.c_str();
        return v;
    }
};
inline Value parse(const std::string& text) {
    Parser p{text};
    Value v = p.value();
    p.ws();
    if (p.i < text.size()) throw p.err("text after the value");
    return v;
}
}  // namespace json

// --- primitive polynomials over GF(2) ------------------------------------------
namespace gf2 {
using u64 = uint64_t;
using u128 = unsigned __int128;
inline u64 mulmod(u64 a, u64 b, u64 m) { return (u64)((u128)a * b % m); }
inline u64 powmod(u64 a, u64 e, u64 m) {
    u64 r = 1;
    for (a %= m; e; e >>= 1, a = mulmod(a, a, m))
        if (e & 1) r = mulmod(r, a, m);
    return r;
}
inline bool isPrime(u64 n) {
    if (n < 2) return false;
    for (u64 p : {2ull, 3ull, 5ull, 7ull, 11ull, 13ull, 17ull, 19ull, 23ull, 29ull, 31ull, 37ull}) {
        if (n % p == 0) return n == p;
    }
    u64 d = n - 1;
    int s = 0;
    while (!(d & 1)) d >>= 1, s++;
    for (u64 a : {2ull, 3ull, 5ull, 7ull, 11ull, 13ull, 17ull, 19ull, 23ull, 29ull, 31ull, 37ull}) {
        u64 x = powmod(a, d, n);
        if (x == 1 || x == n - 1) continue;
        bool comp = true;
        for (int r = 1; r < s && comp; r++) {
            x = mulmod(x, x, n);
            if (x == n - 1) comp = false;
        }
        if (comp) return false;
    }
    return true;
}
inline u64 rho(u64 n) {
    if (!(n & 1)) return 2;
    for (u64 c = 1;; c++) {
        u64 x = 2, y = 2, d = 1;
        auto f = [&](u64 v) { return (u64)(((u128)v * v + c) % n); };
        while (d == 1) {
            x = f(x), y = f(f(y));
            u64 a = x > y ? x - y : y - x, b = n;
            while (b) { u64 t = a % b; a = b; b = t; }
            d = a;
        }
        if (d != n) return d;
    }
}
inline void factor(u64 n, std::set<u64>& out) {
    if (n == 1) return;
    if (isPrime(n)) { out.insert(n); return; }
    u64 d = rho(n);
    factor(d, out);
    factor(n / d, out);
}
// Polynomials of degree n are held without the x^n term: bit k = x^k.
struct Field {
    int n;
    u64 low, mask;
    u64 mul(u64 a, u64 b) const {
        u64 r = 0;
        while (b) {
            if (b & 1) r ^= a;
            b >>= 1;
            bool carry = (a >> (n - 1)) & 1;
            a = (a << 1) & mask;
            if (carry) a ^= low;
        }
        return r;
    }
    u64 pow(u64 a, u64 e) const {
        u64 r = 1;
        for (; e; e >>= 1, a = mul(a, a))
            if (e & 1) r = mul(r, a);
        return r;
    }
};
inline bool primitive(int n, u64 low, const std::set<u64>& primes) {
    if (!(low & 1)) return false;
    Field f{n, low, n == 64 ? ~0ull : ((1ull << n) - 1)};
    u64 order = f.mask;  // 2^n - 1
    if (f.pow(2, order) != 1) return false;
    for (u64 q : primes)
        if (f.pow(2, order / q) == 1) return false;
    return true;
}
// The low part of a primitive polynomial of degree n (2..64) with as few
// terms as possible: trinomials first, then pentanomials, smallest exponents
// first. The result does not depend on the machine.
inline u64 primitivePoly(int n) {
    if (n < 2 || n > 64) throw Error("an LFSR must be 2 to 64 bits wide");
    std::set<u64> primes;
    factor(n == 64 ? ~0ull : ((1ull << n) - 1), primes);
    for (int k = 1; k < n; k++) {
        u64 low = 1ull | (1ull << k);
        if (primitive(n, low, primes)) return low;
    }
    for (int a = 3; a < n; a++)
        for (int b = 2; b < a; b++)
            for (int c = 1; c < b; c++) {
                u64 low = 1ull | (1ull << a) | (1ull << b) | (1ull << c);
                if (primitive(n, low, primes)) return low;
            }
    throw Error("no primitive polynomial of degree " + std::to_string(n) + " found");
}
}  // namespace gf2

struct ScanChain {
    std::string name, scanIn, scanOut;
    int length = 0;
};
struct ScanDef {
    std::string top, scanEn;
    bool scanEnHigh = true;
    std::vector<std::string> clocks;
    std::vector<std::pair<std::string, bool>> resets;  // port, active high
    std::vector<ScanChain> chains;
    std::vector<std::string> unscanned;
};
inline ScanDef readScanDef(const std::string& path) {
    json::Value v = json::parse(readFile(path));
    ScanDef d;
    d.top = v.s("top");
    if (const json::Value* se = v.get("scan_enable")) {
        d.scanEn = se->s("port");
        if (const json::Value* a = se->get("active")) d.scanEnHigh = a->k == json::Value::Num ? a->num != 0 : a->k == json::Value::Str ? (a->str != "low" && a->str != "0") : true;
    }
    auto act = [](const json::Value& o) {
        const json::Value* a = o.get("active");
        if (!a) return false;
        return a->k == json::Value::Num ? a->num != 0 : a->k == json::Value::Str ? (a->str == "high" || a->str == "1") : a->b;
    };
    if (const json::Value* c = v.get("clocks"))
        for (const json::Value& e : c->arr) d.clocks.push_back(e.k == json::Value::Str ? e.str : e.s("port"));
    if (const json::Value* c = v.get("resets"))
        for (const json::Value& e : c->arr) d.resets.push_back({e.s("port"), act(e)});
    if (const json::Value* c = v.get("chains"))
        for (const json::Value& e : c->arr) {
            ScanChain ch;
            ch.name = e.s("name"), ch.scanIn = e.s("scan_in"), ch.scanOut = e.s("scan_out");
            ch.length = (int)e.n("length");
            if (const json::Value* cells = e.get("cells"); cells && ch.length == 0) ch.length = (int)cells->arr.size();
            d.chains.push_back(ch);
        }
    if (const json::Value* c = v.get("unscanned"))
        for (const json::Value& e : c->arr) d.unscanned.push_back(e.k == json::Value::Str ? e.str : e.s("inst"));
    if (d.chains.empty()) throw Error(path + ": no scan chains");
    if (d.scanEn.empty()) throw Error(path + ": no scan_enable port");
    for (const ScanChain& c : d.chains)
        if (c.scanIn.empty() || c.scanOut.empty() || c.length < 1) throw Error(path + ": chain '" + c.name + "' lacks scan_in, scan_out or length");
    return d;
}

struct LbistOpts {
    std::string scandef, outDir = ".", clock;
    int patterns = 1024, prpg = 0, misr = 32;
    uint64_t seed = 0;
    bool sign = false;
    std::string msim;  // the simulator -lbist_sign runs
};

struct LbistPlan {
    ScanDef sd;
    int n = 0, len = 0, patterns = 0, pw = 0, mw = 0;
    uint64_t prpgPoly = 0, misrPoly = 0, seed = 0, taps = 0;
    std::vector<std::array<int, 3>> phase;  // per chain: three PRPG bits
    std::string clock;
    uint64_t cycles() const { return (uint64_t)(patterns + 1) * len + patterns + 1; }

    void plan(const LbistOpts& o) {
        n = (int)sd.chains.size();
        for (const ScanChain& c : sd.chains) len = std::max(len, c.length);
        patterns = o.patterns;
        if (patterns < 1) throw Error("-lbist_patterns must be at least 1");
        // A PRPG wide enough that every chain gets its own three taps.
        pw = o.prpg ? o.prpg : std::max(32, std::min(64, n + 8));
        mw = o.misr;
        if (pw < 8 || pw > 64) throw Error("-lbist_prpg must be 8 to 64");
        if (mw < 8 || mw > 64) throw Error("-lbist_misr must be 8 to 64");
        prpgPoly = gf2::primitivePoly(pw);
        misrPoly = gf2::primitivePoly(mw);
        // Fibonacci form: the new bit 0 is the XOR of the bits at exponent - 1.
        taps = (prpgPoly >> 1) | (1ull << (pw - 1));
        uint64_t mask = pw == 64 ? ~0ull : ((1ull << pw) - 1);
        seed = (o.seed ? o.seed : 0x1D872B41C5A3F96Bull) & mask;
        if (seed == 0) seed = 1;
        clock = o.clock.empty() ? (sd.clocks.empty() ? "" : sd.clocks[0]) : o.clock;
        if (clock.empty()) throw Error("no clock: the scan description lists none; name one with -lbist_clock");
        // Phase shifter: distinct three-bit tap sets from a fixed generator.
        std::mt19937_64 rng(0x9E3779B97F4A7C15ull ^ (uint64_t)pw);
        std::set<std::array<int, 3>> used;
        for (int c = 0; c < n; c++) {
            for (int tries = 0;; tries++) {
                std::array<int, 3> t{(int)(rng() % pw), (int)(rng() % pw), (int)(rng() % pw)};
                std::sort(t.begin(), t.end());
                if (t[0] == t[1] || t[1] == t[2]) continue;
                if (used.insert(t).second || tries > 1000) {
                    phase.push_back(t);
                    break;
                }
            }
        }
    }
    uint64_t next(uint64_t s) const {
        uint64_t mask = pw == 64 ? ~0ull : ((1ull << pw) - 1);
        return ((s << 1) | (uint64_t)(__builtin_popcountll(s & taps) & 1)) & mask;
    }
    // What the controller loads: for each pattern, each chain's cells from
    // scan-in to scan-out, as '0'/'1'.
    std::vector<std::vector<std::string>> loads(int count) const {
        std::vector<std::vector<std::string>> out;
        uint64_t s = seed;
        for (int p = 0; p < count; p++) {
            std::vector<std::string> shifted(n, std::string(len, '0'));  // by shift cycle
            for (int c = 0; c < len; c++) {
                for (int ch = 0; ch < n; ch++)
                    shifted[ch][c] = (((s >> phase[ch][0]) ^ (s >> phase[ch][1]) ^ (s >> phase[ch][2])) & 1) ? '1' : '0';
                s = next(s);
            }
            std::vector<std::string> pat(n);
            for (int ch = 0; ch < n; ch++) {
                int li = sd.chains[ch].length;
                // The cell next to scan-in holds the last bit shifted.
                for (int j = 0; j < li; j++) pat[ch] += shifted[ch][len - 1 - j];
            }
            out.push_back(std::move(pat));
        }
        return out;
    }
};

namespace detail {
inline std::string hex(uint64_t v, int bits) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d'h%llx", bits, (unsigned long long)v);
    return buf;
}
inline int clog2(uint64_t v) {
    int b = 1;
    while ((1ull << b) < v) b++;
    return b;
}
// "scan_in[3]" -> port and bit (-1 for a scalar).
inline std::pair<std::string, int> portBit(const std::string& s) {
    size_t b = s.find('[');
    if (b == std::string::npos) return {s, -1};
    return {s.substr(0, b), std::atoi(s.c_str() + b + 1)};
}
}  // namespace detail

inline void writeLbistCtrl(std::ostream& os, const LbistPlan& p, const std::string& name, const std::string& golden) {
    using detail::hex;
    int sw = detail::clog2((uint64_t)p.len + 1), cw = detail::clog2((uint64_t)p.patterns + 2);
    os << "// Logic BIST controller, written by mdft: " << p.n << " chains, longest " << p.len << ", " << p.patterns
       << " patterns, " << p.cycles() << " clock cycles\n"
       << "//   PRPG " << p.pw << " bits, polynomial low part " << hex(p.prpgPoly, p.pw) << "; MISR " << p.mw
       << " bits, low part " << hex(p.misrPoly, p.mw) << "\n"
       << "module " << name << " #(parameter logic [" << p.mw - 1 << ":0] GOLDEN = " << golden << ") (\n"
       << "  input  logic clk, rst_n, start,\n  output logic busy, done, pass,\n  output logic [" << p.mw - 1
       << ":0] signature,\n  output logic scan_en,\n  output logic [" << p.n - 1 << ":0] scan_in,\n  input  logic ["
       << p.n - 1 << ":0] scan_out);\n"
       << "  localparam int L = " << p.len << ", P = " << p.patterns << ";\n"
       << "  typedef enum logic [1:0] {IDLE, SHIFT, CAPTURE, DONE} st_t;\n  st_t st;\n"
       << "  logic [" << p.pw - 1 << ":0] prpg;\n  logic [" << p.mw - 1 << ":0] misr;\n"
       << "  logic [" << sw - 1 << ":0] sc;\n  logic [" << cw - 1 << ":0] pc;\n"
       << "  wire fb = ^(prpg & " << hex(p.taps, p.pw) << ");\n";
    for (int c = 0; c < p.n; c++)
        os << "  assign scan_in[" << c << "] = prpg[" << p.phase[c][0] << "] ^ prpg[" << p.phase[c][1] << "] ^ prpg["
           << p.phase[c][2] << "];\n";
    // Space compactor: chain c goes to MISR bit c mod width.
    os << "  wire [" << p.mw - 1 << ":0] cin;\n";
    for (int b = 0; b < p.mw; b++) {
        std::string x;
        for (int c = b; c < p.n; c += p.mw) x += (x.empty() ? "" : " ^ ") + ("scan_out[" + std::to_string(c) + "]");
        os << "  assign cin[" << b << "] = " << (x.empty() ? "1'b0" : x) << ";\n";
    }
    os << "  wire [" << p.mw - 1 << ":0] misr_next = ({misr[" << p.mw - 2 << ":0], 1'b0} ^ (misr[" << p.mw - 1 << "] ? "
       << hex(p.misrPoly, p.mw) << " : " << p.mw << "'b0)) ^ cin;\n"
       << "  always_ff @(posedge clk or negedge rst_n) begin\n"
       << "    if (!rst_n) begin\n      st <= IDLE; prpg <= '0; misr <= '0; sc <= '0; pc <= '0;\n    end else case (st)\n"
       << "      IDLE: if (start) begin\n        st <= SHIFT; prpg <= " << hex(p.seed, p.pw)
       << "; misr <= '0; sc <= '0; pc <= '0;\n      end\n"
       << "      SHIFT: begin\n"
       << "        prpg <= {prpg[" << p.pw - 2 << ":0], fb};\n"
       << "        if (pc != 0) misr <= misr_next;  // the first load only fills the chains\n"
       << "        if (sc == L - 1) begin\n          sc <= '0;\n          st <= (pc == P) ? DONE : CAPTURE;\n"
       << "        end else sc <= sc + 1'b1;\n      end\n"
       << "      CAPTURE: begin\n        pc <= pc + 1'b1;\n        st <= SHIFT;\n      end\n"
       << "      DONE: if (!start) st <= IDLE;\n"
       << "      default: st <= IDLE;\n    endcase\n  end\n"
       << "  assign scan_en = (st == SHIFT);\n  assign busy = (st == SHIFT) || (st == CAPTURE);\n"
       << "  assign done = (st == DONE);\n  assign signature = misr;\n  assign pass = done && (misr == GOLDEN);\n"
       << "endmodule\n";
}

struct LbistResult {
    std::string rtlPath, tbPath, iclPath, pdlPath, wrap;
    LbistPlan plan;
};

inline LbistResult insertLbist(const std::vector<Port>& ports, const std::string& top, const LbistOpts& o,
                               const std::string& golden) {
    using detail::portBit;
    LbistResult r;
    LbistPlan& p = r.plan;
    p.sd = readScanDef(o.scandef);
    p.plan(o);
    std::map<std::string, const Port*> byName;
    for (const Port& q : ports) byName[q.name] = &q;
    for (const char* t : {"lbist_start", "lbist_rst_n", "lbist_done", "lbist_pass", "lbist_busy", "lbist_signature"})
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
    // What each input bit is while BIST runs. Default: held at 0.
    std::map<std::pair<std::string, int>, std::string> during;
    std::set<std::string> direct;  // inputs that pass straight through
    auto [ckPort, ckBit] = need(p.clock, Port::In, "the BIST clock");
    std::string bclk = ckPort->bit(ckBit);
    for (const std::string& c : p.sd.clocks) {
        auto [q, b] = need(c, Port::In, "the clock");
        if (q == ckPort && b == ckBit) {
            if (q->width() == 1) direct.insert(q->name);
            else during[{q->name, b}] = bclk;
        } else during[{q->name, b}] = bclk;
    }
    if (ckPort->width() == 1) direct.insert(ckPort->name);
    else during[{ckPort->name, ckBit}] = bclk;
    for (auto& [port, high] : p.sd.resets) {
        auto [q, b] = need(port, Port::In, "the reset");
        during[{q->name, b}] = high ? "1'b0" : "1'b1";
    }
    {
        auto [q, b] = need(p.sd.scanEn, Port::In, "the scan enable");
        during[{q->name, b}] = p.sd.scanEnHigh ? "ctl_se" : "~ctl_se";
    }
    std::vector<std::string> soExpr;
    for (int c = 0; c < p.n; c++) {
        auto [q, b] = need(p.sd.chains[c].scanIn, Port::In, "the scan input");
        during[{q->name, b}] = "ctl_si[" + std::to_string(c) + "]";
        auto [so, sb] = need(p.sd.chains[c].scanOut, Port::Out, "the scan output");
        soExpr.push_back(so->bit(sb));
    }

    r.wrap = top + "_lbist";
    std::string ctrl = top + "_lbist_ctrl";
    std::ostringstream rtl;
    writeLbistCtrl(rtl, p, ctrl, golden);
    rtl << "\n// " << top << " with its logic BIST. lbist_start is a level: hold it high until lbist_done.\n"
        << "module " << r.wrap << " (\n";
    for (const Port& q : ports)
        rtl << "  " << (q.dir == Port::In ? "input  logic " : q.dir == Port::Out ? "output logic " : "inout  wire  ") << q.range()
            << q.name << ",\n";
    rtl << "  input  logic lbist_start, lbist_rst_n,\n  output logic lbist_busy, lbist_done, lbist_pass,\n  output logic ["
        << p.mw - 1 << ":0] lbist_signature);\n"
        << "  wire bist = lbist_start;\n  wire ctl_se;\n  wire [" << p.n - 1 << ":0] ctl_si, ctl_so;\n";
    std::ostringstream conn;
    bool first = true;
    for (const Port& q : ports) {
        conn << (first ? "" : ",\n") << "    ." << q.name << "(";
        first = false;
        if (q.dir != Port::In || direct.count(q.name)) {
            conn << q.name << ")";
            continue;
        }
        conn << "core_" << q.name << ")";
        rtl << "  wire " << q.range() << "core_" << q.name << ";\n";
        for (int b = 0; b < q.width(); b++) {
            auto it = during.find({q.name, b});
            rtl << "  assign core_" << q.bit(b) << " = bist ? " << (it == during.end() ? "1'b0" : it->second) << " : " << q.bit(b)
                << ";\n";
        }
    }
    for (int c = 0; c < p.n; c++) rtl << "  assign ctl_so[" << c << "] = " << soExpr[c] << ";\n";
    rtl << "  " << top << " u_core (\n" << conn.str() << ");\n"
        << "  " << ctrl << " u_lbist (.clk(" << bclk << "), .rst_n(lbist_rst_n), .start(lbist_start), .busy(lbist_busy),\n"
        << "    .done(lbist_done), .pass(lbist_pass), .signature(lbist_signature), .scan_en(ctl_se), .scan_in(ctl_si),\n"
        << "    .scan_out(ctl_so));\nendmodule\n";

    // The testbench: run once, print the signature.
    std::ostringstream tb;
    std::string tbName = r.wrap + "_tb";
    tb << "// " << tbName << " - runs the logic BIST once, written by mdft\n`timescale 1ns/1ps\nmodule " << tbName << ";\n";
    for (const Port& q : ports) {
        if (q.dir == Port::In) tb << "  logic " << q.range() << q.name << " = '0;\n";
        else tb << "  wire " << q.range() << q.name << ";\n";
    }
    tb << "  logic lbist_start = 0, lbist_rst_n = 0;\n  wire lbist_busy, lbist_done, lbist_pass;\n  wire [" << p.mw - 1
       << ":0] lbist_signature;\n  " << r.wrap << " dut (.*);\n"
       << "  always #5 " << bclk << " = ~" << bclk << ";\n"
       << "  initial begin\n    #22 lbist_rst_n = 1;\n    @(negedge " << bclk << ") lbist_start = 1;\n"
       << "    wait (lbist_done);\n    @(negedge " << bclk << ");\n"
       << "    $display(\"LBIST signature = %h\", lbist_signature);\n"
       << "    if (lbist_pass) $display(\"" << tbName << ": PASS\");\n    else $display(\"" << tbName << ": FAIL\");\n"
       << "    $finish;\n  end\n"
       << "  initial begin\n    #" << (p.cycles() + 100) * 10 << ";\n    $display(\"" << tbName
       << ": FAIL (lbist_done never rose)\");\n    $finish;\n  end\nendmodule\n";

    // IJTAG access.
    std::ostringstream icl, pdl;
    int tw = p.mw + 3;
    icl << "// IJTAG access to the logic BIST of " << top << ", written by mdft\n"
        << "Module " << r.wrap << "_instrument {\n  DataInPort start;\n  DataOutPort done;\n  DataOutPort pass;\n  DataOutPort signature["
        << p.mw - 1 << ":0];\n}\n"
        << "Module " << r.wrap << "_access {\n  ScanInPort si;\n  ScanOutPort so { Source tdr[0]; }\n"
        << "  SelectPort sel; ShiftEnPort se; CaptureEnPort ce; UpdateEnPort ue; TCKPort tck; ResetPort rst;\n"
        << "  ScanRegister tdr[" << tw - 1 << ":0] {\n    ScanInSource si;\n"
        << "    CaptureSource bist.signature, bist.pass, bist.done, tdr[0];\n    ResetValue " << tw << "'b0;\n  }\n"
        << "  Instance bist Of " << r.wrap << "_instrument { InputPort start = tdr[0]; }\n}\n";
    pdl << "# Runs the logic BIST of " << top << " and checks its signature. Written by mdft.\n"
        << "# iRunLoop counts TCK cycles: the BIST clock must be at least as fast as TCK.\n"
        << "iProcsForModule " << r.wrap << "_instrument\n"
        << "iProc run {} {\n  iWrite start 1\n  iApply\n  iRunLoop " << p.cycles() + 8 << "\n  iRead done 1\n  iRead pass 1\n";
    if (golden.find('x') == std::string::npos && golden.find("'h") != std::string::npos)
        pdl << "  iRead signature 0x" << golden.substr(golden.find("'h") + 2) << "\n";
    pdl << "  iApply\n  iWrite start 0\n  iApply\n}\n";

    r.rtlPath = o.outDir + "/" + r.wrap + ".sv";
    r.tbPath = o.outDir + "/" + tbName + ".sv";
    r.iclPath = o.outDir + "/" + r.wrap + ".icl";
    r.pdlPath = o.outDir + "/" + r.wrap + ".pdl";
    writeFile(r.rtlPath, rtl.str());
    writeFile(r.tbPath, tb.str());
    writeFile(r.iclPath, icl.str());
    writeFile(r.pdlPath, pdl.str());
    return r;
}

}  // namespace dft
