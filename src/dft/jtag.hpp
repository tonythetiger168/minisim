// jtag.hpp - what the boundary-scan (IEEE 1149.1) and IJTAG (IEEE 1687)
// flows of mdft share: scan bit strings, a test pattern (IR/DR scans, pin
// drives and checks), the SVF and self-checking testbench writers, and the
// generator for the TAP controller RTL.
//
// A scan string holds one character per bit, '0', '1' or 'x', and index 0 is
// the bit that is shifted first: it goes in at TDI first and comes out at TDO
// first. That is the SVF convention (the least significant bit moves first).
#pragma once
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace dft {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
inline std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// value -> n scan bits, bit 0 first.
inline std::string bitsOf(uint64_t v, int n) {
    std::string s(n, '0');
    for (int i = 0; i < n && i < 64; i++)
        if ((v >> i) & 1) s[i] = '1';
    return s;
}
// "0101" written most significant bit first -> scan string.
inline std::string fromMsbFirst(const std::string& msb) { return std::string(msb.rbegin(), msb.rend()); }
inline std::string toMsbFirst(const std::string& s) { return std::string(s.rbegin(), s.rend()); }

// A scan string as a Verilog literal: 12'b0000xx010101.
inline std::string vlog(const std::string& s) {
    if (s.empty()) return "1'b0";
    return std::to_string(s.size()) + "'b" + toMsbFirst(s);
}

// Scan string -> SVF hex (most significant digit first); x bits read as 0.
inline std::string hexOf(const std::string& s, char one = '1') {
    static const char* dig = "0123456789ABCDEF";
    std::string out;
    int n = (int)s.size();
    for (int i = ((n + 3) / 4) * 4 - 4; i >= 0; i -= 4) {
        int v = 0;
        for (int b = 0; b < 4; b++)
            if (i + b < n && s[i + b] == one) v |= 1 << b;
        out += dig[v];
    }
    return out.empty() ? "0" : out;
}
// The care mask of an expected string: 1 where the bit is 0 or 1.
inline std::string careOf(const std::string& s) {
    std::string m(s.size(), '0');
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] != 'x') m[i] = '1';
    return m;
}
inline bool anyCare(const std::string& s) { return s.find_first_of("01") != std::string::npos; }

// One step of a test.
struct Op {
    enum Kind { Reset, Ir, Dr, Run, Note, SetPin, ExpPin } kind = Note;
    std::string tdi, tdo;  // Ir, Dr: what goes in, what must come out (x = don't care)
    int n = 0;             // Run: TCK cycles
    std::string name, val; // Note text; SetPin/ExpPin: signal and Verilog value
};

struct Pattern {
    std::vector<Op> ops;
    void reset() { ops.push_back({Op::Reset}); }
    void ir(std::string tdi, std::string tdo = "") {
        Op o{Op::Ir};
        if (tdo.empty()) tdo.assign(tdi.size(), 'x');
        o.tdi = std::move(tdi), o.tdo = std::move(tdo);
        ops.push_back(std::move(o));
    }
    void dr(std::string tdi, std::string tdo = "") {
        Op o{Op::Dr};
        if (tdo.empty()) tdo.assign(tdi.size(), 'x');
        o.tdi = std::move(tdi), o.tdo = std::move(tdo);
        ops.push_back(std::move(o));
    }
    void run(int n) {
        Op o{Op::Run};
        o.n = n;
        ops.push_back(o);
    }
    void note(std::string t) {
        Op o{Op::Note};
        o.name = std::move(t);
        ops.push_back(std::move(o));
    }
    void setPin(std::string sig, std::string v) {
        Op o{Op::SetPin};
        o.name = std::move(sig), o.val = std::move(v);
        ops.push_back(std::move(o));
    }
    void expPin(std::string sig, std::string v) {
        Op o{Op::ExpPin};
        o.name = std::move(sig), o.val = std::move(v);
        ops.push_back(std::move(o));
    }
    size_t maxScan() const {
        size_t m = 1;
        for (const Op& o : ops) m = std::max(m, o.tdi.size());
        return m;
    }
    // TCK cycles the pattern takes on a tester (TAP protocol).
    uint64_t cycles() const {
        uint64_t c = 0;
        for (const Op& o : ops) {
            if (o.kind == Op::Reset) c += 6;
            else if (o.kind == Op::Ir) c += o.tdi.size() + 6;
            else if (o.kind == Op::Dr) c += o.tdi.size() + 5;
            else if (o.kind == Op::Run) c += o.n;
        }
        return c;
    }
};

// Serial Vector Format. Pin drives and checks have no portable SVF form, so
// they are written as comments.
inline void writeSvf(std::ostream& os, const Pattern& p, const std::string& title, bool trst) {
    os << "! " << title << "\n! written by mdft\n";
    os << (trst ? "TRST OFF;\n" : "TRST ABSENT;\n") << "ENDIR IDLE;\nENDDR IDLE;\nSTATE RESET;\nSTATE IDLE;\n";
    for (const Op& o : p.ops) {
        switch (o.kind) {
        case Op::Reset: os << "STATE RESET;\nSTATE IDLE;\n"; break;
        case Op::Ir:
        case Op::Dr:
            os << (o.kind == Op::Ir ? "SIR " : "SDR ") << o.tdi.size() << " TDI (" << hexOf(o.tdi) << ")";
            if (anyCare(o.tdo)) os << " TDO (" << hexOf(o.tdo) << ") MASK (" << hexOf(careOf(o.tdo)) << ")";
            os << ";\n";
            break;
        case Op::Run: os << "RUNTEST " << o.n << " TCK;\n"; break;
        case Op::Note: os << "! " << o.name << "\n"; break;
        case Op::SetPin: os << "! drive " << o.name << " = " << o.val << "\n"; break;
        case Op::ExpPin: os << "! expect " << o.name << " == " << o.val << "\n"; break;
        }
    }
}

// The device under test as the testbench sees it.
struct TbPin {
    std::string decl;  // "logic [3:0] a" or "wire y"
    std::string name;  // the port it connects to
    std::string init;  // an initial value for a driven pin, or empty
};
struct TbSpec {
    std::string dut;             // module name
    std::vector<TbPin> pins;     // everything but the scan interface
    bool tap = true;             // true: TCK/TMS/TDI/TDO; false: a bare 1687 scan interface
    bool trst = true;            // TAP has TRST_N
    // Port names of the scan interface.
    std::string tck = "tck", tms = "tms", tdi = "tdi", tdo = "tdo", trstn = "trst_n";
    std::string sel = "sel", ce = "ce", se = "se", ue = "ue", rst = "rst";
};

// A self-checking SystemVerilog testbench that plays the pattern and prints
// "<name>: PASS" or the mismatches and "<name>: FAIL".
inline void writeTb(std::ostream& os, const Pattern& p, const TbSpec& s, const std::string& name) {
    size_t w = p.maxScan();
    os << "// " << name << " - self-checking testbench, written by mdft\n`timescale 1ns/1ps\nmodule " << name
       << ";\n  localparam int W = " << w << ";\n";
    os << "  logic tck = 0, tdi = 0;\n  wire tdo;\n  int errors = 0;\n";
    if (s.tap) os << "  logic tms = 1, trst_n = 1;\n";
    else os << "  logic sel = 0, ce = 0, se = 0, ue = 0, rst = 0;\n";
    for (const TbPin& pin : s.pins) {
        os << "  " << pin.decl;
        if (!pin.init.empty()) os << " = " << pin.init;
        os << ";\n";
    }
    os << "  " << s.dut << " dut (";
    os << "." << s.tck << "(tck), ." << s.tdi << "(tdi), ." << s.tdo << "(tdo)";
    if (s.tap) {
        os << ", ." << s.tms << "(tms)";
        if (s.trst) os << ", ." << s.trstn << "(trst_n)";
    } else {
        os << ", ." << s.sel << "(sel), ." << s.ce << "(ce), ." << s.se << "(se), ." << s.ue << "(ue), ." << s.rst
           << "(rst)";
    }
    for (const TbPin& pin : s.pins) os << ",\n      ." << pin.name << "(" << pin.name << ")";
    os << ");\n";
    os << "  task automatic tick(input logic d, input logic e, input logic chk);\n"
          "    tdi = d; #5;\n"
          "    if (chk && tdo !== e) begin\n"
          "      errors++;\n"
          "      if (errors <= 20) $display(\"MISMATCH at %0t: tdo=%b expected %b\", $time, tdo, e);\n"
          "    end\n"
          "    tck = 1; #5; tck = 0;\n"
          "  endtask\n";
    if (s.tap) {
        os << "  task automatic step(input logic m);\n    tms = m; tick(1'b0, 1'b0, 1'b0);\n  endtask\n"
              "  task automatic scan(input bit is_ir, input int n, input logic [W-1:0] di,\n"
              "                      input logic [W-1:0] exp, input logic [W-1:0] care);\n"
              "    step(1); if (is_ir) step(1); step(0); step(0);\n"
              "    for (int i = 0; i < n; i++) begin\n"
              "      tms = (i == n - 1); tick(di[i], exp[i], care[i]);\n"
              "    end\n"
              "    step(1); step(0);\n"
              "  endtask\n"
              "  task automatic tap_reset();\n";
        if (s.trst) os << "    trst_n = 0; #10; trst_n = 1; #10;\n";
        os << "    repeat (5) step(1);\n    step(0);\n  endtask\n";
    } else {
        os << "  task automatic scan(input bit is_ir, input int n, input logic [W-1:0] di,\n"
              "                      input logic [W-1:0] exp, input logic [W-1:0] care);\n"
              "    sel = 1; ce = 1; tick(1'b0, 1'b0, 1'b0); ce = 0; se = 1;\n"
              "    for (int i = 0; i < n; i++) tick(di[i], exp[i], care[i]);\n"
              "    se = 0; ue = 1; tick(1'b0, 1'b0, 1'b0); ue = 0;\n"
              "  endtask\n"
              "  task automatic tap_reset();\n"
              "    rst = 1; #10; rst = 0; #10;\n"
              "  endtask\n";
    }
    os << "  task automatic expect_pin(input string what, input logic ok);\n"
          "    if (!ok) begin\n"
          "      errors++;\n"
          "      if (errors <= 20) $display(\"MISMATCH at %0t: %s\", $time, what);\n"
          "    end\n"
          "  endtask\n";
    os << "  initial begin\n    tap_reset();\n";
    auto lit = [&](const std::string& bits) {
        std::string padded = bits;
        padded.resize(w, '0');
        return vlog(padded);
    };
    for (const Op& o : p.ops) {
        switch (o.kind) {
        case Op::Reset: os << "    tap_reset();\n"; break;
        case Op::Ir:
        case Op::Dr: {
            std::string di = o.tdi, ex = o.tdo;
            for (char& c : di)
                if (c == 'x') c = '0';
            std::string care = careOf(ex);
            for (char& c : ex)
                if (c == 'x') c = '0';
            os << "    scan(" << (o.kind == Op::Ir ? 1 : 0) << ", " << o.tdi.size() << ", " << lit(di) << ", " << lit(ex)
               << ", " << lit(care) << ");\n";
            break;
        }
        case Op::Run:
            if (s.tap) os << "    repeat (" << o.n << ") step(0);\n";
            else os << "    repeat (" << o.n << ") tick(1'b0, 1'b0, 1'b0);\n";
            break;
        case Op::Note: os << "    // " << o.name << "\n"; break;
        case Op::SetPin: os << "    " << o.name << " = " << o.val << ";\n"; break;
        case Op::ExpPin:
            os << "    #1 expect_pin(\"" << o.name << " should be " << o.val << "\", " << o.name << " === " << o.val
               << ");\n";
            break;
        }
    }
    os << "    #10;\n    if (errors == 0) $display(\"" << name << ": PASS\");\n    else $display(\"" << name
       << ": FAIL (%0d mismatches)\", errors);\n    $finish;\n  end\nendmodule\n";
}

// One instruction of a TAP.
struct Instr {
    std::string name;
    std::string opcode;  // most significant bit first, IR length characters
};

// The TAP controller: the 16-state machine, the instruction register, and
// the BYPASS and IDCODE registers. Every other data register lives outside
// and hands its serial output in on ext_so, with ext_sel high while the
// current instruction selects it.
//   capture_dr / shift_dr : act on the rising edge of tck
//   update_dr             : act on the falling edge of tck
inline void writeTapRtl(std::ostream& os, const std::string& name, int irLen, const std::string& irCapture,
                        const std::string& bypassOp, const std::string& idcodeOp, const std::string& idcode) {
    bool hasId = !idcode.empty();
    os << "// IEEE 1149.1 TAP controller, written by mdft\n"
       << "module " << name << " (\n"
       << "  input  logic tck, tms, tdi, trst_n,\n"
       << "  output logic tdo, tdo_en,\n"
       << "  output logic [" << irLen - 1 << ":0] ir,\n"
       << "  output logic capture_dr, shift_dr, update_dr, tlr,\n"
       << "  input  logic ext_sel, ext_so);\n"
       << "  typedef enum logic [3:0] {TLR, RTI, SEL_DR, CAP_DR, SH_DR, EX1_DR, PAU_DR, EX2_DR, UPD_DR,\n"
       << "                            SEL_IR, CAP_IR, SH_IR, EX1_IR, PAU_IR, EX2_IR, UPD_IR} st_t;\n"
       << "  st_t st;\n"
       << "  always_ff @(posedge tck or negedge trst_n) begin\n"
       << "    if (!trst_n) st <= TLR;\n"
       << "    else case (st)\n"
       << "      TLR:    st <= tms ? TLR : RTI;\n"
       << "      RTI:    st <= tms ? SEL_DR : RTI;\n"
       << "      SEL_DR: st <= tms ? SEL_IR : CAP_DR;\n"
       << "      CAP_DR: st <= tms ? EX1_DR : SH_DR;\n"
       << "      SH_DR:  st <= tms ? EX1_DR : SH_DR;\n"
       << "      EX1_DR: st <= tms ? UPD_DR : PAU_DR;\n"
       << "      PAU_DR: st <= tms ? EX2_DR : PAU_DR;\n"
       << "      EX2_DR: st <= tms ? UPD_DR : SH_DR;\n"
       << "      UPD_DR: st <= tms ? SEL_DR : RTI;\n"
       << "      SEL_IR: st <= tms ? TLR : CAP_IR;\n"
       << "      CAP_IR: st <= tms ? EX1_IR : SH_IR;\n"
       << "      SH_IR:  st <= tms ? EX1_IR : SH_IR;\n"
       << "      EX1_IR: st <= tms ? UPD_IR : PAU_IR;\n"
       << "      PAU_IR: st <= tms ? EX2_IR : PAU_IR;\n"
       << "      EX2_IR: st <= tms ? UPD_IR : SH_IR;\n"
       << "      UPD_IR: st <= tms ? SEL_DR : RTI;\n"
       << "      default: st <= TLR;\n"
       << "    endcase\n"
       << "  end\n"
       << "  assign tlr = (st == TLR);\n"
       << "  assign capture_dr = (st == CAP_DR);\n"
       << "  assign shift_dr = (st == SH_DR);\n"
       << "  assign update_dr = (st == UPD_DR);\n"
       << "  localparam logic [" << irLen - 1 << ":0] IR_RESET = " << irLen << "'b" << (hasId ? idcodeOp : bypassOp)
       << ";\n"
       << "  logic [" << irLen - 1 << ":0] ir_sr;\n"
       << "  always_ff @(posedge tck) begin\n"
       << "    if (st == CAP_IR) ir_sr <= " << irLen << "'b" << irCapture << ";\n"
       << "    else if (st == SH_IR) ir_sr <= " << (irLen > 1 ? "{tdi, ir_sr[" + std::to_string(irLen - 1) + ":1]}" : "tdi")
       << ";\n"
       << "  end\n"
       << "  always_ff @(negedge tck or negedge trst_n) begin\n"
       << "    if (!trst_n) ir <= IR_RESET;\n"
       << "    else if (st == TLR) ir <= IR_RESET;\n"
       << "    else if (st == UPD_IR) ir <= ir_sr;\n"
       << "  end\n"
       << "  logic bypass;\n"
       << "  always_ff @(posedge tck) begin\n"
       << "    if (st == CAP_DR) bypass <= 1'b0;\n"
       << "    else if (st == SH_DR) bypass <= tdi;\n"
       << "  end\n";
    if (hasId) {
        os << "  logic [31:0] id_sr;\n"
           << "  always_ff @(posedge tck) begin\n"
           << "    if (st == CAP_DR) id_sr <= 32'b" << idcode << ";\n"
           << "    else if (st == SH_DR) id_sr <= {tdi, id_sr[31:1]};\n"
           << "  end\n"
           << "  wire sel_id = (ir == " << irLen << "'b" << idcodeOp << ");\n";
    }
    os << "  wire dr_so = ext_sel ? ext_so : " << (hasId ? "sel_id ? id_sr[0] : " : "") << "bypass;\n"
       << "  always_ff @(negedge tck or negedge trst_n) begin\n"
       << "    if (!trst_n) begin tdo <= 1'b0; tdo_en <= 1'b0; end\n"
       << "    else begin\n"
       << "      tdo <= (st == SH_IR) ? ir_sr[0] : dr_so;\n"
       << "      tdo_en <= (st == SH_IR) || (st == SH_DR);\n"
       << "    end\n"
       << "  end\n"
       << "endmodule\n";
}

inline void writeFile(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw Error("cannot write " + path);
    f << text;
}
inline std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw Error("cannot read " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace dft
