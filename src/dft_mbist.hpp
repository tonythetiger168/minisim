#pragma once
// mbist.hpp - memory built-in self-test generation: mdft -mbist.
//
// For one synchronous single-port RAM (en, we, addr, wdata, rdata) this
// writes synthesizable Verilog:
//
//   <name>_bist         a controller that runs a March test at speed, one
//                       memory operation per clock, and compares each read
//                       against what the algorithm expects;
//   <name>_bist_collar  the controller plus the muxes that hand the memory's
//                       pins to it while the test runs.
//
// The March algorithm is one of the well-known ones (MATS+, March X, Y, C-,
// A, B, LR, SS) or any element list given with -march. Word-oriented
// memories repeat it over data backgrounds (0000, 0101, 0011, ...) so that
// coupling between the bits of one word is covered too.
//
// The generator also carries a memory fault simulator: it injects every
// stuck-at, transition, coupling (inversion, idempotent, state) and
// address-decoder fault into a small memory and reports which share the
// chosen algorithm detects. The same fault models are written into the
// behavioural RAM of the self-checking testbench (-tb), so the generated
// hardware is checked against the simulator's predictions.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace mbist {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ----------------------------------------------------------------- algorithms

struct Op {
    bool write = false;
    int data = 0;  // 0: the background, 1: its complement
};
struct Element {
    int dir = 0;   // 0 either (run upwards), 1 up, -1 down
    std::vector<Op> ops;
};
using March = std::vector<Element>;

inline const std::vector<std::pair<std::string, std::string>>& algorithms() {
    static const std::vector<std::pair<std::string, std::string>> a{
        {"mats+", "any(w0); up(r0,w1); down(r1,w0)"},
        {"marchx", "any(w0); up(r0,w1); down(r1,w0); any(r0)"},
        {"marchy", "any(w0); up(r0,w1,r1); down(r1,w0,r0); any(r0)"},
        {"marchc-", "any(w0); up(r0,w1); up(r1,w0); down(r0,w1); down(r1,w0); any(r0)"},
        {"marcha", "any(w0); up(r0,w1,w0,w1); up(r1,w0,w1); down(r1,w0,w1,w0); down(r0,w1,w0)"},
        {"marchb", "any(w0); up(r0,w1,r1,w0,r0,w1); up(r1,w0,w1); down(r1,w0,w1,w0); down(r0,w1,w0)"},
        {"marchlr", "any(w0); down(r0,w1); up(r1,w0,r0,w1); up(r1,w0); up(r0,w1,r1,w0); up(r0)"},
        {"marchss",
         "any(w0); up(r0,r0,w0,r0,w1); up(r1,r1,w1,r1,w0); down(r0,r0,w0,r0,w1); "
         "down(r1,r1,w1,r1,w0); any(r0)"},
    };
    return a;
}

// "any(w0); up(r0,w1); down(r1,w0)"
inline March parseMarch(const std::string& text) {
    March m;
    std::string s;
    for (char c : text)
        if (!std::isspace((unsigned char)c)) s += (char)std::tolower((unsigned char)c);
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == ';') { i++; continue; }
        size_t open = s.find('(', i), close = s.find(')', i);
        if (open == std::string::npos || close == std::string::npos || close < open)
            throw Error("-march: expected dir(op,op,...) at '" + s.substr(i) + "'");
        std::string dir = s.substr(i, open - i);
        Element e;
        if (dir == "up" || dir == "u") e.dir = 1;
        else if (dir == "down" || dir == "d") e.dir = -1;
        else if (dir == "any" || dir == "a") e.dir = 0;
        else throw Error("-march: direction '" + dir + "' is not up, down or any");
        std::stringstream ops(s.substr(open + 1, close - open - 1));
        std::string op;
        while (std::getline(ops, op, ',')) {
            if (op.size() != 2 || (op[0] != 'r' && op[0] != 'w') || (op[1] != '0' && op[1] != '1'))
                throw Error("-march: operation '" + op + "' is not r0, r1, w0 or w1");
            e.ops.push_back({op[0] == 'w', op[1] - '0'});
        }
        if (e.ops.empty()) throw Error("-march: an element has no operations");
        m.push_back(e);
        i = close + 1;
    }
    if (m.empty()) throw Error("-march: no elements");
    return m;
}

inline std::string show(const March& m) {
    std::string s;
    for (const Element& e : m) {
        s += (s.empty() ? "" : "; ") + std::string(e.dir > 0 ? "up" : e.dir < 0 ? "down" : "any") + "(";
        for (size_t i = 0; i < e.ops.size(); i++)
            s += (i ? "," : "") + std::string(e.ops[i].write ? "w" : "r") + char('0' + e.ops[i].data);
        s += ")";
    }
    return s;
}

// Data background k of a `width`-bit word: bit i is bit (k-1) of i, so k = 0
// is all zeros, 1 is ...0101, 2 is ...0011. log2(width)+1 of them tell every
// pair of bits apart. Bit 0 is the first character's far end (LSB).
inline std::string background(int k, int width) {
    std::string s(width, '0');
    for (int i = 0; i < width; i++)
        if (k > 0 && ((i >> (k - 1)) & 1)) s[width - 1 - i] = '1';
    return s;
}
inline int allBackgrounds(int width) {
    int n = 1;
    while ((1 << (n - 1)) < width) n++;
    return n;
}

// ------------------------------------------------------------ fault simulator

struct Fault {
    // 0 none, 1 stuck-at, 2 transition, 3 inversion coupling, 4 idempotent
    // coupling, 5 state coupling, 6 address decoder (faddr reaches aaddr's word)
    int type = 0;
    int faddr = 0, fbit = 0;   // the faulty (victim) cell
    int aaddr = 0, abit = 0;   // the aggressor cell
    int val = 0;               // stuck/forced value
    int dir = 0;               // 1: rising transition (or aggressor state 1)
};

struct Geometry {
    int words = 0, width = 0, backgrounds = 1;
};

struct SimResult {
    bool detected = false;
    int addr = 0;
};

// Run the algorithm on a memory with one fault. Exactly what the generated
// controller and the testbench's RAM model do, operation by operation.
inline SimResult simulate(const March& m, const Geometry& g, const Fault& f, bool initOnes) {
    std::vector<uint64_t> mem(g.words, 0);
    uint64_t mask = g.width >= 64 ? ~0ull : ((1ull << g.width) - 1);
    if (initOnes)
        for (auto& w : mem) w = mask;
    auto fix = [&]() {
        if (f.type == 1) mem[f.faddr] = (mem[f.faddr] & ~(1ull << f.fbit)) | (uint64_t(f.val) << f.fbit);
        if (f.type == 5 && int((mem[f.aaddr] >> f.abit) & 1) == f.dir)
            mem[f.faddr] = (mem[f.faddr] & ~(1ull << f.fbit)) | (uint64_t(f.val) << f.fbit);
    };
    fix();
    SimResult r;
    for (int bg = 0; bg < g.backgrounds; bg++) {
        uint64_t pat = 0;
        for (int i = 0; i < g.width; i++)
            if (bg > 0 && ((i >> (bg - 1)) & 1)) pat |= 1ull << i;
        for (const Element& e : m) {
            for (int n = 0; n < g.words; n++) {
                int addr = e.dir < 0 ? g.words - 1 - n : n;
                int a = (f.type == 6 && addr == f.faddr) ? f.aaddr : addr;
                for (const Op& op : e.ops) {
                    uint64_t data = (op.data ? ~pat : pat) & mask;
                    if (!op.write) {
                        if (mem[a] != data) {
                            r.detected = true;
                            r.addr = addr;
                            return r;
                        }
                        continue;
                    }
                    uint64_t old = mem[a], nw = data;
                    if (f.type == 2 && a == f.faddr && int((old >> f.fbit) & 1) != f.dir &&
                        int((nw >> f.fbit) & 1) == f.dir)
                        nw = (nw & ~(1ull << f.fbit)) | (old & (1ull << f.fbit));
                    mem[a] = nw;
                    if ((f.type == 3 || f.type == 4) && a == f.aaddr && int((old >> f.abit) & 1) != f.dir &&
                        int((nw >> f.abit) & 1) == f.dir) {
                        if (f.type == 3) mem[f.faddr] ^= 1ull << f.fbit;
                        else mem[f.faddr] = (mem[f.faddr] & ~(1ull << f.fbit)) | (uint64_t(f.val) << f.fbit);
                    }
                    fix();
                }
            }
        }
    }
    return r;
}

struct Coverage {
    struct Row {
        std::string name;
        long total = 0, detected = 0;
    };
    std::vector<Row> rows;
    Geometry geom;
    long total() const { long n = 0; for (auto& r : rows) n += r.total; return n; }
    long detected() const { long n = 0; for (auto& r : rows) n += r.detected; return n; }
};

// Every fault of every class on a small memory (the classes do not depend on
// the size), each from both power-up states: a fault counts as detected only
// if the test fails whatever the memory held before.
inline Coverage coverage(const March& m, const Geometry& real, int threads) {
    Geometry g;
    g.words = std::min(real.words, 8);
    g.width = std::min(real.width, 4);
    g.backgrounds = std::min(real.backgrounds, allBackgrounds(g.width));
    static const char* names[] = {
        "stuck-at",
        "transition",
        "inversion coupling, between words",
        "inversion coupling, within a word",
        "idempotent coupling, between words",
        "idempotent coupling, within a word",
        "state coupling, between words",
        "state coupling, within a word",
        "address decoder"};
    std::vector<Fault> faults;
    std::vector<int> row;
    int cells = g.words * g.width;
    for (int c = 0; c < cells; c++) {
        int fa = c / g.width, fb = c % g.width;
        for (int v = 0; v < 2; v++) {
            faults.push_back({1, fa, fb, 0, 0, v, 0});
            row.push_back(0);
            faults.push_back({2, fa, fb, 0, 0, 0, v});
            row.push_back(1);
        }
        for (int a = 0; a < cells; a++) {
            if (a == c) continue;
            int aa = a / g.width, ab = a % g.width;
            int within = aa == fa ? 1 : 0;
            for (int dir = 0; dir < 2; dir++) {
                faults.push_back({3, fa, fb, aa, ab, 0, dir});
                row.push_back(2 + within);
                for (int v = 0; v < 2; v++) {
                    faults.push_back({4, fa, fb, aa, ab, v, dir});
                    row.push_back(4 + within);
                    faults.push_back({5, fa, fb, aa, ab, v, dir});
                    row.push_back(6 + within);
                }
            }
        }
    }
    for (int a = 0; a < g.words; a++)
        for (int b = 0; b < g.words; b++)
            if (a != b) {
                faults.push_back({6, a, 0, b, 0, 0, 0});
                row.push_back(8);
            }

    // Each fault is independent: split the list over the threads. The result
    // is a flag per fault, so it does not depend on how the list was split.
    std::vector<char> hit(faults.size(), 0);
    int nt = std::max(1, std::min<int>(threads, int(faults.size() / 256) + 1));
    std::atomic<size_t> nextChunk{0};
    const size_t chunk = 256;
    auto work = [&]() {
        for (;;) {
            size_t at = nextChunk.fetch_add(chunk);
            if (at >= faults.size()) return;
            size_t end = std::min(faults.size(), at + chunk);
            for (size_t i = at; i < end; i++)
                hit[i] = simulate(m, g, faults[i], false).detected && simulate(m, g, faults[i], true).detected;
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < nt; t++) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();

    Coverage cov;
    cov.geom = g;
    for (const char* n : names) cov.rows.push_back({n, 0, 0});
    for (size_t i = 0; i < faults.size(); i++) {
        cov.rows[row[i]].total++;
        cov.rows[row[i]].detected += hit[i];
    }
    return cov;
}

// ------------------------------------------------------------------ generator

struct Options {
    std::string name = "mem";
    int words = 0, width = 0;
    int latency = 1;
    int backgrounds = 1;
    bool allBg = false;
    std::string algo = "marchc-", march;
    std::string out, tb;
    int threads = 0;
    bool quiet = false;
};

inline int clog2(long n) {
    int b = 0;
    while ((1l << b) < n) b++;
    return std::max(b, 1);
}

inline void writeRtl(std::ostream& os, const Options& o, const March& m) {
    const int aw = clog2(o.words), w = o.width, lat = o.latency, nbg = o.backgrounds;
    struct Flat {
        Op op;
        bool last, down, nextDown, final;
        int first;
    };
    std::vector<Flat> ops;
    for (size_t e = 0; e < m.size(); e++) {
        int first = int(ops.size());
        for (size_t k = 0; k < m[e].ops.size(); k++) {
            Flat f;
            f.op = m[e].ops[k];
            f.last = k + 1 == m[e].ops.size();
            f.down = m[e].dir < 0;
            f.nextDown = e + 1 < m.size() && m[e + 1].dir < 0;
            f.final = f.last && e + 1 == m.size();
            f.first = first;
            ops.push_back(f);
        }
    }
    const int opw = clog2(long(ops.size())), bgw = clog2(nbg), dw = clog2(lat + 1);
    const std::string n = o.name;
    auto vec = [](int bits) { return bits > 1 ? "[" + std::to_string(bits - 1) + ":0] " : std::string(); };
    // The top `bits` bits of a `total`-bit vector, which may be a scalar.
    auto top = [](int total, int bits) {
        if (total == 1) return std::string();
        if (bits == 1) return "[" + std::to_string(total - 1) + "]";
        return "[" + std::to_string(total - 1) + ":" + std::to_string(total - bits) + "]";
    };
    std::string start0 = m[0].dir < 0 ? std::to_string(aw) + "'d" + std::to_string(o.words - 1)
                                      : std::to_string(aw) + "'d0";

    os << "// Memory BIST for " << n << ": " << o.words << " words of " << w << " bits, read latency " << lat
       << ".\n// March algorithm: " << show(m) << "\n// " << nbg << " data background" << (nbg == 1 ? "" : "s")
       << ", " << ops.size() << " operations per address per background.\n"
       << "// Written by mdft -mbist.\n\n";

    os << "module " << n << "_bist (\n"
       << "  input  clk,\n  input  rst_n,\n"
       << "  input  bist_start,          // hold high to run; drop it to rearm\n"
       << "  output bist_busy,           // the controller owns the memory\n"
       << "  output bist_done,\n"
       << "  output reg bist_fail,       // sticky until the next start\n"
       << "  output reg " << vec(aw) << "bist_fail_addr,   // the first failing read\n"
       << "  output reg " << vec(w) << "bist_fail_exp,\n"
       << "  output reg " << vec(w) << "bist_fail_got,\n"
       << "  output reg [7:0] bist_fail_count,   // saturates at 255\n"
       << "  output mem_en,\n  output mem_we,\n"
       << "  output " << vec(aw) << "mem_addr,\n"
       << "  output " << vec(w) << "mem_wdata,\n"
       << "  input  " << vec(w) << "mem_rdata\n);\n";
    os << "  localparam IDLE = 2'd0, RUN = 2'd1, DRAIN = 2'd2, DONE = 2'd3;\n"
       << "  reg [1:0] state;\n"
       << "  reg " << vec(opw) << "op;\n"
       << "  reg " << vec(aw) << "addr;\n"
       << "  reg " << vec(bgw) << "bg;\n"
       << "  reg " << vec(dw) << "drain;\n\n";
    os << "  // The March elements, one operation per entry.\n"
       << "  reg op_we, op_d, op_last, op_down, op_next_down, op_final;\n"
       << "  reg " << vec(opw) << "op_first;\n"
       << "  always @(*) begin\n"
       << "    op_we = 1'b0; op_d = 1'b0; op_last = 1'b1; op_down = 1'b0;\n"
       << "    op_next_down = 1'b0; op_final = 1'b1; op_first = " << opw << "'d0;\n"
       << "    case (op)\n";
    for (size_t i = 0; i < ops.size(); i++) {
        const Flat& f = ops[i];
        os << "      " << opw << "'d" << i << ": begin op_we = 1'b" << f.op.write << "; op_d = 1'b" << f.op.data
           << "; op_last = 1'b" << f.last << "; op_down = 1'b" << f.down << "; op_next_down = 1'b" << f.nextDown
           << "; op_final = 1'b" << f.final << "; op_first = " << opw << "'d" << f.first << "; end  // "
           << (f.down ? "down " : "up ") << (f.op.write ? "w" : "r") << f.op.data << "\n";
    }
    os << "      default: ;\n    endcase\n  end\n\n";
    os << "  reg " << vec(w) << "bg_pat;\n  always @(*) begin\n    case (bg)\n";
    for (int k = 0; k < nbg; k++)
        os << "      " << bgw << "'d" << k << ": bg_pat = " << w << "'b" << background(k, w) << ";\n";
    os << "      default: bg_pat = " << w << "'b" << background(0, w) << ";\n    endcase\n  end\n\n";
    os << "  wire run = state == RUN;\n"
       << "  wire " << vec(w) << "data = op_d ? ~bg_pat : bg_pat;\n"
       << "  wire addr_end = op_down ? addr == " << aw << "'d0 : addr == " << aw << "'d" << o.words - 1 << ";\n"
       << "  assign mem_en = run;\n  assign mem_we = run & op_we;\n"
       << "  assign mem_addr = addr;\n  assign mem_wdata = data;\n"
       << "  assign bist_busy = state == RUN || state == DRAIN;\n"
       << "  assign bist_done = state == DONE;\n\n";
    os << "  always @(posedge clk or negedge rst_n) begin\n"
       << "    if (!rst_n) begin\n"
       << "      state <= IDLE; op <= " << opw << "'d0; addr <= " << aw << "'d0; bg <= " << bgw << "'d0; drain <= "
       << dw << "'d0;\n"
       << "    end else begin\n      case (state)\n"
       << "        IDLE: if (bist_start) begin\n"
       << "          state <= RUN; op <= " << opw << "'d0; bg <= " << bgw << "'d0; addr <= " << start0 << ";\n"
       << "        end\n"
       << "        RUN: begin\n"
       << "          if (!op_last) op <= op + " << opw << "'d1;\n"
       << "          else if (!addr_end) begin\n"
       << "            op <= op_first;\n"
       << "            addr <= op_down ? addr - " << aw << "'d1 : addr + " << aw << "'d1;\n"
       << "          end else if (!op_final) begin\n"
       << "            op <= op + " << opw << "'d1;\n"
       << "            addr <= op_next_down ? " << aw << "'d" << o.words - 1 << " : " << aw << "'d0;\n"
       << "          end else if (bg != " << bgw << "'d" << nbg - 1 << ") begin\n"
       << "            bg <= bg + " << bgw << "'d1; op <= " << opw << "'d0; addr <= " << start0 << ";\n"
       << "          end else begin\n";
    if (lat == 0) os << "            state <= DONE;\n";
    else os << "            state <= DRAIN; drain <= " << dw << "'d" << lat - 1 << ";\n";
    os << "          end\n        end\n"
       << "        DRAIN: if (drain == " << dw << "'d0) state <= DONE; else drain <= drain - " << dw << "'d1;\n"
       << "        default: if (!bist_start) state <= IDLE;\n"
       << "      endcase\n    end\n  end\n\n";

    os << "  // What each read should return, delayed by the memory's read latency.\n";
    if (lat == 0) {
        os << "  wire chk_v = run & ~op_we;\n  wire " << vec(w) << "chk_exp = data;\n  wire " << vec(aw)
           << "chk_addr = addr;\n";
    } else {
        os << "  reg " << vec(lat) << "cmp_v;\n  reg " << vec(lat * w) << "cmp_exp;\n  reg " << vec(lat * aw)
           << "cmp_addr;\n"
           << "  always @(posedge clk or negedge rst_n) begin\n"
           << "    if (!rst_n) begin\n      cmp_v <= " << lat << "'d0; cmp_exp <= " << lat * w << "'d0; cmp_addr <= "
           << lat * aw << "'d0;\n    end else begin\n";
        if (lat == 1)
            os << "      cmp_v <= run & ~op_we; cmp_exp <= data; cmp_addr <= addr;\n";
        else
            os << "      cmp_v <= {cmp_v[" << lat - 2 << ":0], run & ~op_we};\n"
               << "      cmp_exp <= {cmp_exp[" << (lat - 1) * w - 1 << ":0], data};\n"
               << "      cmp_addr <= {cmp_addr[" << (lat - 1) * aw - 1 << ":0], addr};\n";
        os << "    end\n  end\n"
           << "  wire chk_v = cmp_v" << (lat > 1 ? "[" + std::to_string(lat - 1) + "]" : "") << ";\n"
           << "  wire " << vec(w) << "chk_exp = cmp_exp" << top(lat * w, w) << ";\n"
           << "  wire " << vec(aw) << "chk_addr = cmp_addr" << top(lat * aw, aw) << ";\n";
    }
    os << "  always @(posedge clk or negedge rst_n) begin\n"
       << "    if (!rst_n) begin\n"
       << "      bist_fail <= 1'b0; bist_fail_addr <= " << aw << "'d0; bist_fail_exp <= " << w
       << "'d0; bist_fail_got <= " << w << "'d0;\n      bist_fail_count <= 8'd0;\n"
       << "    end else if (state == IDLE && bist_start) begin\n"
       << "      bist_fail <= 1'b0; bist_fail_count <= 8'd0;\n"
       << "    end else if (chk_v && mem_rdata != chk_exp) begin\n"
       << "      bist_fail <= 1'b1;\n"
       << "      if (!bist_fail) begin\n"
       << "        bist_fail_addr <= chk_addr; bist_fail_exp <= chk_exp; bist_fail_got <= mem_rdata;\n"
       << "      end\n"
       << "      if (bist_fail_count != 8'hff) bist_fail_count <= bist_fail_count + 8'd1;\n"
       << "    end\n  end\nendmodule\n\n";

    os << "// The controller and the muxes in front of the memory: the test owns the\n"
       << "// memory pins while bist_busy, the design owns them otherwise.\n"
       << "module " << n << "_bist_collar (\n"
       << "  input  clk,\n  input  rst_n,\n  input  bist_start,\n  output bist_busy,\n  output bist_done,\n"
       << "  output bist_fail,\n"
       << "  output " << vec(aw) << "bist_fail_addr,\n  output " << vec(w) << "bist_fail_exp,\n  output " << vec(w)
       << "bist_fail_got,\n  output [7:0] bist_fail_count,\n"
       << "  input  func_en,\n  input  func_we,\n  input  " << vec(aw) << "func_addr,\n  input  " << vec(w)
       << "func_wdata,\n  output " << vec(w) << "func_rdata,\n"
       << "  output mem_en,\n  output mem_we,\n  output " << vec(aw) << "mem_addr,\n  output " << vec(w)
       << "mem_wdata,\n  input  " << vec(w) << "mem_rdata\n);\n"
       << "  wire b_en, b_we;\n  wire " << vec(aw) << "b_addr;\n  wire " << vec(w) << "b_wdata;\n"
       << "  " << n << "_bist u_bist (\n"
       << "    .clk(clk), .rst_n(rst_n), .bist_start(bist_start), .bist_busy(bist_busy), .bist_done(bist_done),\n"
       << "    .bist_fail(bist_fail), .bist_fail_addr(bist_fail_addr), .bist_fail_exp(bist_fail_exp),\n"
       << "    .bist_fail_got(bist_fail_got), .bist_fail_count(bist_fail_count),\n"
       << "    .mem_en(b_en), .mem_we(b_we), .mem_addr(b_addr), .mem_wdata(b_wdata), .mem_rdata(mem_rdata));\n"
       << "  assign mem_en = bist_busy ? b_en : func_en;\n"
       << "  assign mem_we = bist_busy ? b_we : func_we;\n"
       << "  assign mem_addr = bist_busy ? b_addr : func_addr;\n"
       << "  assign mem_wdata = bist_busy ? b_wdata : func_wdata;\n"
       << "  assign func_rdata = mem_rdata;\nendmodule\n";
}

// The faults the testbench injects: one of each kind, spread over the memory.
inline std::vector<Fault> tbFaults(const Options& o) {
    int n = o.words, w = o.width;
    int a1 = n / 3, a2 = (2 * n) / 3 == a1 ? (a1 + 1) % n : (2 * n) / 3, last = n - 1;
    int b1 = w / 2, b2 = w - 1;
    std::vector<Fault> f;
    f.push_back({0, 0, 0, 0, 0, 0, 0});
    f.push_back({1, a1, b1, 0, 0, 0, 0});
    f.push_back({1, last, b2, 0, 0, 1, 0});
    f.push_back({2, a1, 0, 0, 0, 0, 1});
    f.push_back({2, a2, b2, 0, 0, 0, 0});
    if (n > 1) {
        f.push_back({3, a1, b1, a2, 0, 0, 1});
        f.push_back({3, a2, 0, a1, b2, 0, 0});
        f.push_back({4, a1, b2, a2, b1, 1, 1});
        f.push_back({4, a2, b1, a1, 0, 0, 0});
        f.push_back({5, a1, 0, a2, b2, 1, 0});
        f.push_back({5, a2, b2, a1, b1, 0, 1});
        f.push_back({6, a1, 0, a2, 0, 0, 0});
        f.push_back({6, last, 0, 0, 0, 0, 0});
    }
    if (w > 1) {
        f.push_back({3, a1, 0, a1, b2, 0, 1});
        f.push_back({4, a1, b2, a1, 0, 1, 0});
        f.push_back({5, a1, 0, a1, b2, 0, 1});
    }
    return f;
}

inline void writeTb(std::ostream& os, const Options& o, const March& m) {
    const int aw = clog2(o.words), w = o.width, lat = o.latency;
    const std::string n = o.name;
    auto vec = [](int bits) { return bits > 1 ? "[" + std::to_string(bits - 1) + ":0] " : std::string(); };
    Geometry g{o.words, o.width, o.backgrounds};
    std::vector<Fault> faults = tbFaults(o);

    os << "// Self-checking testbench for " << n << "_bist, written by mdft -mbist.\n"
       << "// Each instance tests a RAM model with one injected fault; the expected\n"
       << "// verdict and first failing address come from mdft's fault simulator.\n"
       << "`timescale 1ns/1ps\n\n"
       << "// A synchronous RAM with one injectable fault. FAULT: 0 none, 1 stuck-at\n"
       << "// FVAL, 2 transition (FDIR=1: cannot rise), 3 inversion coupling, 4\n"
       << "// idempotent coupling (to FVAL), 5 state coupling (aggressor at FDIR forces\n"
       << "// FVAL), 6 address FADDR reaches word AADDR. The victim is bit FBIT of word\n"
       << "// FADDR, the aggressor bit ABIT of word AADDR.\n"
       << "module " << n << "_ram_model (clk, en, we, addr, wdata, rdata);\n"
       << "  parameter FAULT = 0, FADDR = 0, FBIT = 0, AADDR = 0, ABIT = 0, FVAL = 0, FDIR = 0;\n"
       << "  input clk, en, we;\n  input " << vec(aw) << "addr;\n  input " << vec(w) << "wdata;\n  output "
       << vec(w) << "rdata;\n"
       << "  reg [" << w - 1 << ":0] mem [0:" << o.words - 1 << "];\n"
       << "  reg [" << w - 1 << ":0] old, nw, t;\n  integer a, i;\n"
       << "  task fix;\n    begin\n"
       << "      if (FAULT == 1) begin t = mem[FADDR]; t[FBIT] = FVAL; mem[FADDR] = t; end\n"
       << "      if (FAULT == 5) begin\n        t = mem[AADDR];\n        if (t[ABIT] == FDIR) begin t = mem[FADDR]; t[FBIT] = FVAL; mem[FADDR] = t; end\n      end\n"
       << "    end\n  endtask\n"
       << "  initial begin\n    for (i = 0; i < " << o.words << "; i = i + 1) mem[i] = " << w
       << "'d0;\n    fix;\n  end\n";
    if (lat > 0) {
        os << "  reg " << vec(w) << "rd0;\n";
        for (int k = 1; k < lat; k++) os << "  reg " << vec(w) << "rd" << k << ";\n";
    }
    os << "  always @(posedge clk) begin\n";
    for (int k = lat - 1; k >= 1; k--) os << "    rd" << k << " <= rd" << k - 1 << ";\n";
    os << "    if (en) begin\n"
       << "      a = (FAULT == 6 && addr == FADDR) ? AADDR : addr;\n"
       << "      if (we) begin\n"
       << "        old = mem[a]; nw = wdata;\n"
       << "        if (FAULT == 2 && a == FADDR && old[FBIT] != FDIR && nw[FBIT] == FDIR) nw[FBIT] = old[FBIT];\n"
       << "        mem[a] = nw;\n"
       << "        if ((FAULT == 3 || FAULT == 4) && a == AADDR && old[ABIT] != FDIR && nw[ABIT] == FDIR) begin\n"
       << "          t = mem[FADDR];\n"
       << "          if (FAULT == 3) t[FBIT] = ~t[FBIT]; else t[FBIT] = FVAL;\n"
       << "          mem[FADDR] = t;\n        end\n"
       << "        fix;\n"
       << "      end" << (lat > 0 ? " else rd0 <= mem[a];\n" : "\n") << "    end\n  end\n";
    if (lat == 0)
        os << "  wire " << vec(aw) << "ra = (FAULT == 6 && addr == FADDR) ? AADDR : addr;\n"
           << "  assign rdata = mem[ra];\n";
    else
        os << "  assign rdata = rd" << lat - 1 << ";\n";
    os << "endmodule\n\n";

    size_t nf = faults.size();
    os << "module " << n << "_bist_tb;\n"
       << "  reg clk, rst_n, start;\n"
       << "  wire [" << nf - 1 << ":0] done, fail, busy;\n"
       << "  integer errors, cycles;\n";
    for (size_t i = 0; i < nf; i++) {
        const Fault& f = faults[i];
        os << "  wire en" << i << ", we" << i << "; wire " << vec(aw) << "addr" << i << ", fa" << i << "; wire "
           << vec(w) << "wd" << i << ", rd" << i << ";\n"
           << "  " << n << "_bist_collar c" << i << " (.clk(clk), .rst_n(rst_n), .bist_start(start), .bist_busy(busy["
           << i << "]), .bist_done(done[" << i << "]), .bist_fail(fail[" << i << "]), .bist_fail_addr(fa" << i
           << "), .bist_fail_exp(), .bist_fail_got(), .bist_fail_count(),\n"
           << "    .func_en(1'b0), .func_we(1'b0), .func_addr(" << aw << "'d0), .func_wdata(" << w
           << "'d0), .func_rdata(), .mem_en(en" << i << "), .mem_we(we" << i << "), .mem_addr(addr" << i
           << "), .mem_wdata(wd" << i << "), .mem_rdata(rd" << i << "));\n"
           << "  " << n << "_ram_model #(.FAULT(" << f.type << "), .FADDR(" << f.faddr << "), .FBIT(" << f.fbit
           << "), .AADDR(" << f.aaddr << "), .ABIT(" << f.abit << "), .FVAL(" << f.val << "), .FDIR(" << f.dir
           << ")) m" << i << " (.clk(clk), .en(en" << i << "), .we(we" << i << "), .addr(addr" << i
           << "), .wdata(wd" << i << "), .rdata(rd" << i << "));\n";
    }
    os << "  always #5 clk = ~clk;\n"
       << "  task check;\n    input integer idx;\n    input got_fail;\n    input exp_fail;\n    input "
       << (aw > 1 ? "[" + std::to_string(aw - 1) + ":0] " : "") << "got_addr;\n    input integer exp_addr;\n"
       << "    begin\n"
       << "      if (got_fail !== exp_fail) begin\n        errors = errors + 1;\n"
       << "        $display(\"instance %0d: bist_fail=%b, expected %b\", idx, got_fail, exp_fail);\n"
       << "      end else if (exp_fail && got_addr !== exp_addr) begin\n        errors = errors + 1;\n"
       << "        $display(\"instance %0d: first failing address %0d, expected %0d\", idx, got_addr, exp_addr);\n"
       << "      end\n    end\n  endtask\n"
       << "  initial begin\n    clk = 0; rst_n = 0; start = 0; errors = 0; cycles = 0;\n"
       << "    #12 rst_n = 1;\n    @(negedge clk) start = 1;\n"
       << "    while (done !== {" << nf << "{1'b1}}) begin\n      @(negedge clk); cycles = cycles + 1;\n"
       << "      if (cycles > " << (long long)o.words * 64 * o.backgrounds * (long long)m.size() * 8 + 1000
       << ") begin $display(\"MBIST TB FAIL: timeout\"); $finish; end\n    end\n";
    int detected = 0;
    for (size_t i = 0; i < nf; i++) {
        SimResult r = simulate(m, g, faults[i], false);
        detected += r.detected;
        os << "    check(" << i << ", fail[" << i << "], 1'b" << r.detected << ", fa" << i << ", " << r.addr
           << ");\n";
    }
    os << "    if (errors == 0) $display(\"MBIST TB PASS: " << nf << " memories, " << detected
       << " faulty ones caught, %0d cycles\", cycles);\n"
       << "    else $display(\"MBIST TB FAIL: %0d errors\", errors);\n    $finish;\n  end\nendmodule\n";
}

inline void usage() {
    std::cout <<
        "usage: mdft -mbist -name N -words W -width B [options]\n"
        "  generate a March memory BIST controller and collar for one RAM\n\n"
        "  -name N          prefix of the generated modules (N_bist, N_bist_collar)\n"
        "  -words W         number of words          -width B   bits per word\n"
        "  -latency L       clocks from read to data (default 1; 0 = asynchronous read)\n"
        "  -algo A          mats+, marchx, marchy, marchc- (default), marcha, marchb,\n"
        "                   marchlr, marchss\n"
        "  -march 'ELEMS'   your own, e.g. 'any(w0); up(r0,w1); down(r1,w0)'\n"
        "  -backgrounds K   data backgrounds to repeat over (default 1); 'all' = log2(B)+1\n"
        "  -o FILE          the Verilog (default: standard output)\n"
        "  -tb FILE         a self-checking testbench with fault-injecting RAM models\n"
        "  -threads N       threads for the fault simulator (default: every core)\n"
        "  -list            list the algorithms\n"
        "  -quiet           no fault coverage report\n";
}

inline int main(const std::vector<std::string>& args) {
    Options o;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) throw Error(a + " needs a value");
            return args[++i];
        };
        auto num = [&](int least) {
            std::string v = next();
            try {
                size_t used = 0;
                int n = std::stoi(v, &used);
                if (used != v.size() || n < least) throw 0;
                return n;
            } catch (...) {
                throw Error(a + " needs a number of at least " + std::to_string(least) + ", got '" + v + "'");
            }
        };
        if (a == "-h" || a == "-help" || a == "--help") { usage(); return 0; }
        else if (a == "-list") {
            for (auto& [name, text] : algorithms()) std::cout << name << std::string(9 - name.size(), ' ') << text << "\n";
            return 0;
        }
        else if (a == "-name") o.name = next();
        else if (a == "-words") o.words = num(1);
        else if (a == "-width") o.width = num(1);
        else if (a == "-latency") o.latency = num(0);
        else if (a == "-algo") o.algo = next();
        else if (a == "-march") o.march = next();
        else if (a == "-backgrounds") {
            if (i + 1 < args.size() && args[i + 1] == "all") { o.allBg = true; i++; }
            else o.backgrounds = num(1);
        }
        else if (a == "-o") o.out = next();
        else if (a == "-tb") o.tb = next();
        else if (a == "-threads") o.threads = num(1);
        else if (a == "-quiet" || a == "-q") o.quiet = true;
        else throw Error("unknown option '" + a + "' (see mdft -mbist -help)");
    }
    if (o.words < 1 || o.width < 1) throw Error("-mbist needs -words and -width (see mdft -mbist -help)");
    if (o.latency > 16) throw Error("-latency above 16 is not supported");
    for (char c : o.name)
        if (!std::isalnum((unsigned char)c) && c != '_') throw Error("-name must be a plain identifier");
    if (o.name.empty() || std::isdigit((unsigned char)o.name[0])) throw Error("-name must be a plain identifier");
    if (o.allBg) o.backgrounds = allBackgrounds(o.width);
    if (o.backgrounds > allBackgrounds(o.width))
        throw Error("a " + std::to_string(o.width) + "-bit word has only " +
                    std::to_string(allBackgrounds(o.width)) + " data backgrounds");
    March m;
    if (!o.march.empty()) {
        m = parseMarch(o.march);
    } else {
        std::string want;
        for (char c : o.algo) want += (char)std::tolower((unsigned char)c);
        for (auto& [name, text] : algorithms())
            if (name == want) m = parseMarch(text);
        if (m.empty()) throw Error("unknown algorithm '" + o.algo + "' (see mdft -mbist -list)");
    }
    if (o.threads <= 0) o.threads = std::max(1u, std::thread::hardware_concurrency());

    auto put = [&](const std::string& file, const std::string& text) {
        std::ofstream f(file);
        if (!f) throw Error("cannot write '" + file + "'");
        f << text;
    };
    std::ostringstream rtl;
    writeRtl(rtl, o, m);
    if (o.out.empty()) std::cout << rtl.str();
    else put(o.out, rtl.str());
    if (!o.tb.empty()) {
        if (o.width > 64) throw Error("-tb supports words of up to 64 bits");
        std::ostringstream tb;
        writeTb(tb, o, m);
        put(o.tb, tb.str());
    }
    if (!o.quiet) {
        std::ostream& os = o.out.empty() ? std::cerr : std::cout;
        long opsPerAddr = 0;
        for (const Element& e : m) opsPerAddr += long(e.ops.size());
        os << "mdft: " << o.name << "_bist: " << o.words << " x " << o.width << ", " << show(m) << "\n"
           << "  " << opsPerAddr << "N operations, " << o.backgrounds << " background"
           << (o.backgrounds == 1 ? "" : "s") << ": " << opsPerAddr * o.words * o.backgrounds + o.latency + 1
           << " clocks\n";
        Coverage cov = coverage(m, {o.words, o.width, o.backgrounds}, o.threads);
        os << "  fault coverage (every fault on a " << cov.geom.words << " x " << cov.geom.width << " memory):\n";
        for (const Coverage::Row& r : cov.rows) {
            if (!r.total) continue;
            char buf[160];
            std::snprintf(buf, sizeof buf, "    %-36s %6ld / %-6ld %5.1f%%\n", r.name.c_str(), r.detected,
                          r.total, 100.0 * r.detected / r.total);
            os << buf;
        }
        char buf[160];
        std::snprintf(buf, sizeof buf, "    %-36s %6ld / %-6ld %5.1f%%\n", "total", cov.detected(), cov.total(),
                      100.0 * cov.detected() / cov.total());
        os << buf;
    }
    return 0;
}

}  // namespace mbist
