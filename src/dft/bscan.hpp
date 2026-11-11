// bscan.hpp - IEEE 1149.1 boundary scan for mdft.
//
//   insertion  (mdft -bscan): from the ports of the top module, write a
//              wrapper with a TAP controller, an instruction register, BYPASS
//              and IDCODE registers and a boundary register (one cell per pad
//              bit), and the BSDL file that describes it.
//   BSDL       (mdft -bsdl): read a BSDL file, check it (rules BSD001..009),
//              and write the infrastructure and pin tests for the device as
//              SVF and as a self-checking testbench.
#pragma once
#include <set>

#include "jtag.hpp"

namespace dft {

struct Port {
    std::string name;
    enum Dir { In, Out, InOut } dir = In;
    int msb = 0, lsb = 0;
    bool vec = false;
    int width() const { return std::abs(msb - lsb) + 1; }
    int index(int b) const { return msb >= lsb ? lsb + b : lsb - b; }  // bit b, counted from the lsb end
    std::string bit(int b) const { return vec ? name + "[" + std::to_string(index(b)) + "]" : name; }
    std::string range() const { return vec ? "[" + std::to_string(msb) + ":" + std::to_string(lsb) + "] " : ""; }
};

struct TriGroup {
    std::string pad, out, oe, in;  // the pad to create, and the core's out / enable / in ports
    bool oeLow = false;            // the enable is active low
};

struct BscanOpts {
    std::string top;
    std::string outDir = ".";
    uint32_t idcode = 0;
    bool hasId = true;
    int irLen = 0;                   // 0: as short as the instructions allow, at least 4
    std::set<std::string> clocks;    // observe-only cells: the system clock path stays untouched
    std::set<std::string> exclude;   // no cell at all
    std::vector<TriGroup> tri;
    bool highz = false;              // every 2-state output gets a shared enable cell; adds HIGHZ
    bool intest = false;
    std::vector<std::string> user;   // extra instructions that select an external register (IJTAG)
    bool quiet = false;
};

struct BscanResult {
    std::string rtlPath, bsdlPath;
    int cells = 0, irLen = 0;
};

namespace detail {
struct GenCell {
    std::string func;   // input, clock, observe_only, output2, output3, control
    std::string port;   // BSDL port reference: name or name(i), or * for a control cell
    char safe = 'X';
    int ctrl = -1;      // index of the control cell in the cell list
    int disval = 0;
    std::string cap;    // what the cell captures (RTL expression)
};
inline int ceilLog2(int n) {
    int b = 0;
    while ((1 << b) < n) b++;
    return b;
}
inline std::string opcode(int v, int len) { return toMsbFirst(bitsOf((uint64_t)v, len)); }
}  // namespace detail

// Write <top>_bscan.sv and <top>.bsdl.
inline BscanResult insertBscan(const std::vector<Port>& ports, const BscanOpts& o) {
    using detail::GenCell;
    std::map<std::string, const Port*> byName;
    for (const Port& p : ports) byName[p.name] = &p;
    for (const char* t : {"tck", "tms", "tdi", "tdo", "trst_n"})
        if (byName.count(t)) throw Error(std::string("the design already has a port named '") + t + "'");
    auto need = [&](const std::string& n, Port::Dir d, const char* what) -> const Port& {
        auto it = byName.find(n);
        if (it == byName.end()) throw Error("-bscan_tristate: no port '" + n + "' in " + o.top);
        if (it->second->dir != d) throw Error("-bscan_tristate: '" + n + "' must be " + what);
        return *it->second;
    };
    std::map<std::string, const TriGroup*> triOf;  // core port -> its group
    for (const TriGroup& g : o.tri) {
        const Port& out = need(g.out, Port::Out, "an output");
        const Port& oe = need(g.oe, Port::Out, "an output");
        if (oe.width() != 1 && oe.width() != out.width())
            throw Error("-bscan_tristate: '" + g.oe + "' must be 1 bit or as wide as '" + g.out + "'");
        if (!g.in.empty() && need(g.in, Port::In, "an input").width() != out.width())
            throw Error("-bscan_tristate: '" + g.in + "' and '" + g.out + "' differ in width");
        if (byName.count(g.pad) && g.pad != g.out && g.pad != g.in && g.pad != g.oe)
            throw Error("-bscan_tristate: the pad name '" + g.pad + "' is already a port");
        triOf[g.out] = triOf[g.oe] = &g;
        if (!g.in.empty()) triOf[g.in] = &g;
    }
    for (const std::string& c : o.clocks)
        if (!byName.count(c)) throw Error("-bscan_clock: no port '" + c + "' in " + o.top);
    for (const std::string& c : o.exclude)
        if (!byName.count(c)) throw Error("-bscan_exclude: no port '" + c + "' in " + o.top);

    std::vector<GenCell> cells;
    std::ostringstream decl, body, conn, wports, bports, pinmap;
    int pin = 0;
    auto bsdlBit = [](const Port& p, const std::string& name, int b) {
        return p.vec ? name + "(" + std::to_string(p.index(b)) + ")" : name;
    };
    auto addBsdlPort = [&](const std::string& name, const char* mode, const Port& shape) {
        bports << "    " << name << " : " << mode << " ";
        if (shape.vec)
            bports << "bit_vector(" << shape.msb << (shape.msb >= shape.lsb ? " downto " : " to ") << shape.lsb << ")";
        else bports << "bit";
        bports << ";\n";
        pinmap << "    \"" << name << " : ";
        if (shape.vec) {
            pinmap << "(";
            for (int i = 0; i < shape.width(); i++) pinmap << (i ? ", " : "") << ++pin;
            pinmap << ")";
        } else pinmap << ++pin;
        pinmap << ",\" &\n";
    };
    // Cells are collected first; a cell's number is known once the count is.
    struct Use { int cell; std::string fmt; };  // fmt with %N for the cell number
    std::vector<Use> uses;
    auto use = [&](int cell, const std::string& fmt) { uses.push_back({cell, fmt}); };
    int sharedCtl = -1;
    std::set<const TriGroup*> triDone;
    bool first = true;
    auto connect = [&](const std::string& port, const std::string& expr) {
        conn << (first ? "" : ",\n") << "    ." << port << "(" << expr << ")";
        first = false;
    };
    for (const Port& p : ports) {
        int w = p.width();
        if (auto it = triOf.find(p.name); it != triOf.end()) {
            const TriGroup& g = *it->second;
            connect(p.name, "core_" + p.name);
            decl << "  wire " << p.range() << "core_" << p.name << ";\n";
            if (!triDone.insert(&g).second) continue;
            const Port& out = *byName[g.out];
            const Port& oe = *byName[g.oe];
            wports << "  inout  wire  " << out.range() << g.pad << ",\n";
            addBsdlPort(g.pad, "inout", out);
            decl << "  wire " << out.range() << g.pad << "_en, " << g.pad << "_val;\n";
            int groupCtl = -1, gw = out.width();
            for (int b = gw - 1; b >= 0; b--) {
                std::string padBit = out.vec ? g.pad + "[" + std::to_string(out.index(b)) + "]" : g.pad;
                std::string enBit = out.vec ? g.pad + "_en[" + std::to_string(out.index(b)) + "]" : g.pad + "_en";
                std::string valBit = out.vec ? g.pad + "_val[" + std::to_string(out.index(b)) + "]" : g.pad + "_val";
                std::string oeSig = std::string(g.oeLow ? "~" : "") + "core_" + (oe.width() == 1 ? oe.name : oe.bit(b));
                if (oe.width() != 1 || b == gw - 1) {
                    cells.push_back({"control", "*", '0', -1, 0, oeSig});
                    groupCtl = (int)cells.size() - 1;
                }
                int ctl = groupCtl;
                use(ctl, "  assign " + enBit + " = (mode_out ? bsr_upd[%N] : " + oeSig + ") & ~highz;\n");
                cells.push_back({"output3", bsdlBit(out, g.pad, b), 'X', ctl, 0, "core_" + out.bit(b)});
                use((int)cells.size() - 1, "  assign " + valBit + " = mode_out ? bsr_upd[%N] : core_" + out.bit(b) + ";\n");
                body << "  assign " << padBit << " = " << enBit << " ? " << valBit << " : 1'bz;\n";
                if (!g.in.empty()) {
                    const Port& in = *byName[g.in];
                    cells.push_back({"input", bsdlBit(out, g.pad, b), 'X', -1, 0, padBit});
                    use((int)cells.size() - 1,
                        "  assign core_" + in.bit(b) + " = mode_in ? bsr_upd[%N] : " + padBit + ";\n");
                }
            }
            continue;
        }
        if (o.exclude.count(p.name) || (p.dir == Port::InOut)) {
            // Straight through. A bidirectional pad the tool knows no enable
            // for still gets an observe-only cell.
            wports << "  " << (p.dir == Port::In ? "input  logic " : p.dir == Port::Out ? "output logic " : "inout  wire  ")
                   << p.range() << p.name << ",\n";
            addBsdlPort(p.name, p.dir == Port::In ? "in" : p.dir == Port::Out ? "out" : "inout", p);
            connect(p.name, p.name);
            if (!o.exclude.count(p.name))
                for (int b = w - 1; b >= 0; b--) cells.push_back({"observe_only", bsdlBit(p, p.name, b), 'X', -1, 0, p.bit(b)});
            continue;
        }
        if (p.dir == Port::In) {
            wports << "  input  logic " << p.range() << p.name << ",\n";
            addBsdlPort(p.name, "in", p);
            bool clk = o.clocks.count(p.name) > 0;
            if (clk) connect(p.name, p.name);
            else {
                connect(p.name, "core_" + p.name);
                decl << "  wire " << p.range() << "core_" << p.name << ";\n";
            }
            for (int b = w - 1; b >= 0; b--) {
                cells.push_back({clk ? "clock" : "input", bsdlBit(p, p.name, b), 'X', -1, 0, p.bit(b)});
                if (!clk)
                    use((int)cells.size() - 1,
                        "  assign core_" + p.bit(b) + " = mode_in ? bsr_upd[%N] : " + p.bit(b) + ";\n");
            }
            continue;
        }
        // A 2-state output; with -bscan_highz it shares one enable cell.
        wports << "  output " << (o.highz ? "wire  " : "logic ") << p.range() << p.name << ",\n";
        addBsdlPort(p.name, "out", p);
        connect(p.name, "core_" + p.name);
        decl << "  wire " << p.range() << "core_" << p.name << ";\n";
        if (o.highz && sharedCtl < 0) {
            cells.push_back({"control", "*", '1', -1, 1, "1'b0"});
            sharedCtl = (int)cells.size() - 1;
            use(sharedCtl, "  wire out_dis = highz | (mode_out & bsr_upd[%N]);\n");
        }
        for (int b = w - 1; b >= 0; b--) {
            cells.push_back({o.highz ? "output3" : "output2", bsdlBit(p, p.name, b), 'X', o.highz ? sharedCtl : -1, 1,
                             "core_" + p.bit(b)});
            std::string val = "(mode_out ? bsr_upd[%N] : core_" + p.bit(b) + ")";
            use((int)cells.size() - 1, "  assign " + p.bit(b) + " = " + (o.highz ? "out_dis ? 1'bz : " + val : val) + ";\n");
        }
    }
    int n = (int)cells.size();
    if (n == 0) throw Error("no port of " + o.top + " takes a boundary cell");
    bool anyTri = o.highz || !o.tri.empty();

    // Instructions.
    std::vector<std::string> names{"EXTEST", "SAMPLE", "IDCODE", "CLAMP"};
    if (!o.hasId) names.erase(names.begin() + 2);
    if (anyTri) names.push_back("HIGHZ");
    if (o.intest) names.push_back("INTEST");
    for (const std::string& u : o.user) {
        std::string un = upper(u);
        if (std::find(names.begin(), names.end(), un) != names.end() || un == "BYPASS" || un == "PRELOAD")
            throw Error("-bscan_user: '" + u + "' is a standard instruction name");
        names.push_back(un);
    }
    int irLen = std::max({o.irLen, 2, detail::ceilLog2((int)names.size() + 1)});
    if (o.irLen == 0) irLen = std::max(irLen, 4);
    if (o.irLen && irLen != o.irLen) throw Error("-bscan_ir " + std::to_string(o.irLen) + " is too short for the instructions");
    std::map<std::string, std::string> op;
    for (size_t i = 0; i < names.size(); i++) op[names[i]] = detail::opcode((int)i, irLen);
    op["BYPASS"] = std::string(irLen, '1');
    std::string irCapture = std::string(irLen - 2, '0') + "01";
    uint32_t id = o.idcode | 1u;  // bit 0 of a device identification code is always 1
    std::string idBits = toMsbFirst(bitsOf(id, 32));

    std::string wrap = o.top + "_bscan", tap = o.top + "_bscan_tap";
    std::ostringstream rtl;
    writeTapRtl(rtl, tap, irLen, irCapture, op["BYPASS"], o.hasId ? op["IDCODE"] : "", o.hasId ? idBits : "");
    rtl << "\n// " << o.top << " with its boundary register: " << n << " cells, " << irLen
        << "-bit instruction register.\nmodule " << wrap << " (\n"
        << wports.str();
    for (const std::string& u : o.user)
        rtl << "  output logic " << lower(u) << "_sel,\n  input  logic " << lower(u) << "_so,\n";
    if (!o.user.empty()) rtl << "  output logic ijtag_ce, ijtag_se, ijtag_ue, ijtag_si, ijtag_rst,\n";
    rtl << "  input  logic tck, tms, tdi, trst_n,\n  output wire  tdo);\n";
    rtl << "  logic [" << irLen - 1 << ":0] ir;\n  logic capture_dr, shift_dr, update_dr, tlr, tdo_r, tdo_en;\n";
    auto is = [&](const std::string& nm) { return "(ir == " + std::to_string(irLen) + "'b" + op[nm] + ")"; };
    rtl << "  wire extest = " << is("EXTEST") << ";\n  wire sample = " << is("SAMPLE") << ";\n  wire clamp = "
        << is("CLAMP") << ";\n";
    rtl << "  wire highz = " << (anyTri ? is("HIGHZ") : "1'b0") << ";\n";
    rtl << "  wire intest = " << (o.intest ? is("INTEST") : "1'b0") << ";\n";
    rtl << "  wire mode_out = extest | clamp;\n  wire mode_in = intest;\n  wire sel_bsr = extest | sample | intest;\n";
    std::string extSel = "sel_bsr", extSo = "sel_bsr ? bsr[0] : ";
    for (const std::string& u : o.user) {
        std::string l = lower(u);
        rtl << "  assign " << l << "_sel = " << is(upper(u)) << ";\n";
        extSel += " | " + l + "_sel";
        extSo += l + "_sel ? " + l + "_so : ";
    }
    extSo += "1'b0";
    if (!o.user.empty())
        rtl << "  assign ijtag_ce = capture_dr;\n  assign ijtag_se = shift_dr;\n  assign ijtag_ue = update_dr;\n"
               "  assign ijtag_si = tdi;\n  assign ijtag_rst = tlr;\n";
    rtl << "  logic [" << n - 1 << ":0] bsr, bsr_upd;\n  wire  [" << n - 1 << ":0] bsr_cap;\n";
    rtl << "  " << tap << " u_tap (.tck(tck), .tms(tms), .tdi(tdi), .trst_n(trst_n), .tdo(tdo_r), .tdo_en(tdo_en),\n"
        << "    .ir(ir), .capture_dr(capture_dr), .shift_dr(shift_dr), .update_dr(update_dr), .tlr(tlr),\n"
        << "    .ext_sel(" << extSel << "), .ext_so(" << extSo << "));\n"
        << "  assign tdo = tdo_en ? tdo_r : 1'bz;\n";
    rtl << "  always_ff @(posedge tck) begin\n"
        << "    if (capture_dr && sel_bsr) bsr <= bsr_cap;\n"
        << "    else if (shift_dr && sel_bsr) bsr <= " << (n > 1 ? "{tdi, bsr[" + std::to_string(n - 1) + ":1]}" : "tdi")
        << ";\n  end\n";
    // Control cells come up disabling their drivers.
    std::string rst(n, '0');
    for (int i = 0; i < n; i++)
        if (cells[i].func == "control" && cells[i].safe == '1') rst[i] = '1';  // rst is msb-first: index i = cell n-1-i
    rtl << "  always_ff @(negedge tck or negedge trst_n) begin\n"
        << "    if (!trst_n) bsr_upd <= " << n << "'b" << rst << ";\n"
        << "    else if (update_dr && sel_bsr) bsr_upd <= bsr;\n  end\n";
    rtl << decl.str();
    for (int i = 0; i < n; i++) rtl << "  assign bsr_cap[" << n - 1 - i << "] = " << cells[i].cap << ";\n";
    for (const auto& u : uses) {
        std::string s = u.fmt;
        size_t at = s.find("%N");
        s.replace(at, 2, std::to_string(n - 1 - u.cell));
        rtl << s;
    }
    rtl << body.str();
    rtl << "  " << o.top << " u_core (\n" << conn.str() << ");\nendmodule\n";

    // BSDL.
    std::ostringstream b;
    std::string ent = o.top + "_bscan";
    b << "-- " << ent << ": boundary-scan description, written by mdft\n"
      << "entity " << ent << " is\n  generic (PHYSICAL_PIN_MAP : string := \"PKG\");\n  port (\n"
      << bports.str();
    Port scalar;
    for (const std::string& u : o.user) b << "    " << lower(u) << "_sel : linkage bit;\n    " << lower(u) << "_so : linkage bit;\n";
    if (!o.user.empty())
        for (const char* s : {"ijtag_ce", "ijtag_se", "ijtag_ue", "ijtag_si", "ijtag_rst"}) b << "    " << s << " : linkage bit;\n";
    b << "    tck : in bit;\n    tms : in bit;\n    tdi : in bit;\n    trst_n : in bit;\n    tdo : out bit);\n"
      << "  use STD_1149_1_2001.all;\n"
      << "  attribute COMPONENT_CONFORMANCE of " << ent << " : entity is \"STD_1149_1_2001\";\n"
      << "  attribute PIN_MAP of " << ent << " : entity is PHYSICAL_PIN_MAP;\n"
      << "  constant PKG : PIN_MAP_STRING :=\n"
      << pinmap.str();
    for (const std::string& u : o.user)
        b << "    \"" << lower(u) << "_sel : " << pin + 1 << ",\" &\n    \"" << lower(u) << "_so : " << pin + 2 << ",\" &\n", pin += 2;
    if (!o.user.empty())
        for (const char* s : {"ijtag_ce", "ijtag_se", "ijtag_ue", "ijtag_si", "ijtag_rst"}) b << "    \"" << s << " : " << ++pin << ",\" &\n";
    b << "    \"tck : " << pin + 1 << ",\" &\n    \"tms : " << pin + 2 << ",\" &\n    \"tdi : " << pin + 3
      << ",\" &\n    \"trst_n : " << pin + 4 << ",\" &\n    \"tdo : " << pin + 5 << "\";\n"
      << "  attribute TAP_SCAN_IN of tdi : signal is true;\n"
      << "  attribute TAP_SCAN_MODE of tms : signal is true;\n"
      << "  attribute TAP_SCAN_OUT of tdo : signal is true;\n"
      << "  attribute TAP_SCAN_CLOCK of tck : signal is (10.0e6, BOTH);\n"
      << "  attribute TAP_SCAN_RESET of trst_n : signal is true;\n"
      << "  attribute INSTRUCTION_LENGTH of " << ent << " : entity is " << irLen << ";\n"
      << "  attribute INSTRUCTION_OPCODE of " << ent << " : entity is\n";
    std::vector<std::string> order = names;
    order.insert(order.begin() + 2, "PRELOAD");
    order.push_back("BYPASS");
    op["PRELOAD"] = op["SAMPLE"];
    for (size_t i = 0; i < order.size(); i++)
        b << "    \"" << order[i] << " (" << op[order[i]] << ")" << (i + 1 < order.size() ? ",\" &\n" : "\";\n");
    b << "  attribute INSTRUCTION_CAPTURE of " << ent << " : entity is \"" << irCapture << "\";\n";
    if (o.hasId)
        b << "  attribute IDCODE_REGISTER of " << ent << " : entity is\n    \"" << idBits.substr(0, 4) << "\" & \""
          << idBits.substr(4, 16) << "\" & \"" << idBits.substr(20, 11) << "\" & \"1\";\n";
    b << "  attribute REGISTER_ACCESS of " << ent << " : entity is\n    \"BYPASS (BYPASS, CLAMP" << (anyTri ? ", HIGHZ" : "")
      << "),\" &\n    \"BOUNDARY (EXTEST, SAMPLE, PRELOAD" << (o.intest ? ", INTEST" : "") << ")"
      << (o.hasId ? ",\" &\n    \"DEVICE_ID (IDCODE)\";\n" : "\";\n");
    b << "  attribute BOUNDARY_LENGTH of " << ent << " : entity is " << n << ";\n"
      << "  attribute BOUNDARY_REGISTER of " << ent << " : entity is\n";
    for (int i = n - 1; i >= 0; i--) {
        const GenCell& c = cells[i];
        int num = n - 1 - i;
        b << "    \"" << num << " (" << (c.func == "observe_only" || c.func == "clock" ? "BC_4" : "BC_1") << ", " << c.port
          << ", " << c.func << ", " << c.safe;
        if (c.func == "output3") b << ", " << n - 1 - c.ctrl << ", " << c.disval << ", Z";
        b << ")" << (i ? ",\" &\n" : "\";\n");
    }
    b << "end " << ent << ";\n";

    BscanResult r;
    r.cells = n, r.irLen = irLen;
    r.rtlPath = o.outDir + "/" + wrap + ".sv";
    r.bsdlPath = o.outDir + "/" + ent + ".bsdl";
    writeFile(r.rtlPath, rtl.str());
    writeFile(r.bsdlPath, b.str());
    return r;
}

// ---------------------------------------------------------------------------
// BSDL reader.

struct BsdlPort {
    std::string name, mode;  // in, out, inout, buffer, linkage
    bool vec = false;
    int left = 0, right = 0;
    bool has(int i) const { return vec && i >= std::min(left, right) && i <= std::max(left, right); }
    int width() const { return vec ? std::abs(left - right) + 1 : 1; }
};
struct BsdlCell {
    int num = 0;
    std::string type, port, func;
    int bit = -1;  // -1: scalar or no port
    char safe = 'X';
    int ccell = -1, disval = 0;
    std::string rslt;
};
struct BsdlInstr {
    std::string name;
    std::vector<std::string> opcodes;  // msb first
};
struct BsdlFinding {
    std::string rule, text;
};
struct Bsdl {
    std::string entity;
    std::vector<BsdlPort> ports;
    std::map<std::string, std::string> attr;  // entity attributes, names in upper case
    std::string tdi, tms, tdo, tck, trst;
    int irLen = 0, blen = 0;
    std::vector<BsdlInstr> instrs;
    std::string capture, idcode;  // msb first, may hold X
    std::vector<BsdlCell> cells;  // sorted by number once checked
    std::map<std::string, int> pinCount;
    bool hasPinMap = false;
    std::vector<BsdlFinding> findings;

    const BsdlPort* port(const std::string& n) const {
        for (const BsdlPort& p : ports)
            if (lower(p.name) == lower(n)) return &p;
        return nullptr;
    }
    const BsdlInstr* instr(const std::string& n) const {
        for (const BsdlInstr& i : instrs)
            if (i.name == n) return &i;
        return nullptr;
    }
    void find(const char* rule, std::string text) { findings.push_back({rule, std::move(text)}); }
};

namespace detail {
struct VTok {
    enum K { Id, Num, Str, Punct, End } k = End;
    std::string s;
    int line = 0;
};
inline std::vector<VTok> vhdlTokens(const std::string& src) {
    std::vector<VTok> t;
    int line = 1;
    for (size_t i = 0; i < src.size();) {
        char c = src[i];
        if (c == '\n') { line++, i++; continue; }
        if (std::isspace((unsigned char)c)) { i++; continue; }
        if (c == '-' && i + 1 < src.size() && src[i + 1] == '-') {
            while (i < src.size() && src[i] != '\n') i++;
            continue;
        }
        if (c == '"') {
            size_t j = src.find('"', i + 1);
            if (j == std::string::npos) throw Error("BSDL line " + std::to_string(line) + ": unterminated string");
            t.push_back({VTok::Str, src.substr(i + 1, j - i - 1), line});
            i = j + 1;
            continue;
        }
        if (std::isalpha((unsigned char)c)) {
            size_t j = i;
            while (j < src.size() && (std::isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
            t.push_back({VTok::Id, src.substr(i, j - i), line});
            i = j;
            continue;
        }
        if (std::isdigit((unsigned char)c)) {
            size_t j = i;
            while (j < src.size() && (std::isalnum((unsigned char)src[j]) || src[j] == '.' || src[j] == '_' ||
                                      ((src[j] == '+' || src[j] == '-') && (src[j - 1] == 'e' || src[j - 1] == 'E'))))
                j++;
            t.push_back({VTok::Num, src.substr(i, j - i), line});
            i = j;
            continue;
        }
        if (c == ':' && i + 1 < src.size() && src[i + 1] == '=') {
            t.push_back({VTok::Punct, ":=", line});
            i += 2;
            continue;
        }
        t.push_back({VTok::Punct, std::string(1, c), line});
        i++;
    }
    t.push_back({VTok::End, "", line});
    return t;
}
// Split "a (b, c), d (e)" at top-level commas.
inline std::vector<std::string> splitTop(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char c : s) {
        if (c == '(') depth++;
        if (c == ')') depth--;
        if (c == ',' && depth == 0) {
            out.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    if (cur.find_first_not_of(" \t\r\n") != std::string::npos) out.push_back(cur);
    return out;
}
inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
inline bool isInt(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit((unsigned char)c); });
}
}  // namespace detail

inline Bsdl parseBsdl(const std::string& src) {
    using namespace detail;
    Bsdl d;
    std::vector<VTok> t = vhdlTokens(src);
    size_t i = 0;
    auto isKw = [&](size_t k, const char* w) { return t[k].k == VTok::Id && lower(t[k].s) == w; };
    auto err = [&](const std::string& m) -> Error { return Error("BSDL line " + std::to_string(t[i].line) + ": " + m); };
    auto expect = [&](const char* p) {
        if (!(t[i].k == VTok::Punct && t[i].s == p) && !isKw(i, p)) throw err(std::string("expected '") + p + "' before '" + t[i].s + "'");
        i++;
    };
    // A value: strings joined by &, or anything else up to the ';'.
    auto value = [&]() {
        std::string v;
        while (t[i].k != VTok::End && !(t[i].k == VTok::Punct && t[i].s == ";")) {
            if (t[i].k == VTok::Str) v += t[i].s;
            else if (!(t[i].k == VTok::Punct && t[i].s == "&")) v += t[i].s + " ";
            i++;
        }
        expect(";");
        return trim(v);
    };
    if (!isKw(i, "entity")) throw err("expected 'entity'");
    i++;
    d.entity = t[i++].s;
    expect("is");
    while (t[i].k != VTok::End) {
        if (isKw(i, "generic")) {
            while (t[i].k != VTok::End && !(t[i].k == VTok::Punct && t[i].s == ";")) i++;
            i++;
        } else if (isKw(i, "port")) {
            i++;
            expect("(");
            while (true) {
                std::vector<std::string> names;
                while (true) {
                    if (t[i].k != VTok::Id) throw err("expected a port name");
                    names.push_back(t[i++].s);
                    if (t[i].s == ",") { i++; continue; }
                    break;
                }
                expect(":");
                BsdlPort p;
                p.mode = lower(t[i++].s);
                std::string ty = lower(t[i++].s);
                if (ty == "bit_vector") {
                    expect("(");
                    p.vec = true;
                    p.left = std::atoi(t[i++].s.c_str());
                    if (!isKw(i, "to") && !isKw(i, "downto")) throw err("expected 'to' or 'downto'");
                    i++;
                    p.right = std::atoi(t[i++].s.c_str());
                    expect(")");
                } else if (ty != "bit") throw err("port type '" + ty + "' is not bit or bit_vector");
                for (const std::string& nm : names) {
                    p.name = nm;
                    d.ports.push_back(p);
                }
                if (t[i].s == ";") { i++; continue; }
                break;
            }
            expect(")");
            expect(";");
        } else if (isKw(i, "use")) {
            while (t[i].k != VTok::End && t[i].s != ";") i++;
            i++;
        } else if (isKw(i, "attribute")) {
            i++;
            std::string name = upper(t[i++].s);
            expect("of");
            std::string target = t[i++].s;
            expect(":");
            std::string kind = lower(t[i++].s);
            expect("is");
            std::string v = value();
            if (kind == "signal") {
                if (name == "TAP_SCAN_IN") d.tdi = target;
                else if (name == "TAP_SCAN_MODE") d.tms = target;
                else if (name == "TAP_SCAN_OUT") d.tdo = target;
                else if (name == "TAP_SCAN_CLOCK") d.tck = target;
                else if (name == "TAP_SCAN_RESET") d.trst = target;
            } else d.attr[name] = v;
        } else if (isKw(i, "constant")) {
            i++;
            i++;  // name
            expect(":");
            std::string ty = upper(t[i++].s);
            expect(":=");
            std::string v = value();
            if (ty == "PIN_MAP_STRING") {
                d.hasPinMap = true;
                for (const std::string& e : splitTop(v)) {
                    size_t c = e.find(':');
                    if (c == std::string::npos) continue;
                    std::string pins = trim(e.substr(c + 1));
                    int cnt = 1;
                    if (!pins.empty() && pins[0] == '(') cnt = (int)splitTop(pins.substr(1, pins.rfind(')') - 1)).size();
                    d.pinCount[lower(trim(e.substr(0, c)))] = cnt;
                }
            }
        } else if (isKw(i, "end")) {
            break;
        } else throw err("unexpected '" + t[i].s + "'");
    }

    // The sub-languages inside the attribute strings.
    auto get = [&](const char* n) -> const std::string* {
        auto it = d.attr.find(n);
        return it == d.attr.end() ? nullptr : &it->second;
    };
    if (auto* v = get("INSTRUCTION_LENGTH")) d.irLen = std::atoi(v->c_str());
    if (auto* v = get("BOUNDARY_LENGTH")) d.blen = std::atoi(v->c_str());
    auto bitsOnly = [](const std::string& s) {
        std::string r;
        for (char c : s)
            if (c == '0' || c == '1') r += c;
            else if (c == 'x' || c == 'X') r += 'x';
        return r;
    };
    if (auto* v = get("INSTRUCTION_CAPTURE")) d.capture = bitsOnly(*v);
    if (auto* v = get("IDCODE_REGISTER")) d.idcode = bitsOnly(*v);
    if (auto* v = get("INSTRUCTION_OPCODE")) {
        for (const std::string& e : splitTop(*v)) {
            size_t a = e.find('('), z = e.rfind(')');
            if (a == std::string::npos || z == std::string::npos) {
                d.find("BSD002", "INSTRUCTION_OPCODE: cannot read '" + trim(e) + "'");
                continue;
            }
            BsdlInstr in;
            in.name = upper(trim(e.substr(0, a)));
            for (const std::string& oc : splitTop(e.substr(a + 1, z - a - 1))) in.opcodes.push_back(bitsOnly(oc));
            d.instrs.push_back(in);
        }
    }
    if (auto* v = get("BOUNDARY_REGISTER")) {
        for (const std::string& e : splitTop(*v)) {
            size_t a = e.find('('), z = e.rfind(')');
            std::string num = a == std::string::npos ? "" : trim(e.substr(0, a));
            if (a == std::string::npos || z == std::string::npos || !isInt(num)) {
                d.find("BSD004", "BOUNDARY_REGISTER: cannot read '" + trim(e) + "'");
                continue;
            }
            std::vector<std::string> f = splitTop(e.substr(a + 1, z - a - 1));
            for (std::string& s : f) s = trim(s);
            if (f.size() != 4 && f.size() != 7) {
                d.find("BSD004", "cell " + num + ": expected 4 or 7 fields, found " + std::to_string(f.size()));
                continue;
            }
            BsdlCell c;
            c.num = std::atoi(num.c_str());
            c.type = upper(f[0]);
            c.func = lower(f[2]);
            c.safe = f[3].empty() ? 'X' : (char)std::toupper((unsigned char)f[3][0]);
            size_t p = f[1].find('(');
            if (p != std::string::npos) {
                c.port = trim(f[1].substr(0, p));
                c.bit = std::atoi(f[1].c_str() + p + 1);
            } else c.port = f[1];
            if (f.size() == 7) {
                c.ccell = isInt(f[4]) ? std::atoi(f[4].c_str()) : -2;
                c.disval = std::atoi(f[5].c_str());
                c.rslt = upper(f[6]);
            }
            d.cells.push_back(c);
        }
    }
    return d;
}

// Rules BSD001..BSD009. Returns the number of findings.
inline int checkBsdl(Bsdl& d) {
    for (const char* a : {"INSTRUCTION_LENGTH", "INSTRUCTION_OPCODE", "INSTRUCTION_CAPTURE", "BOUNDARY_LENGTH",
                          "BOUNDARY_REGISTER"})
        if (!d.attr.count(a)) d.find("BSD001", std::string("the attribute ") + a + " is missing");
    if (d.attr.count("INSTRUCTION_LENGTH") && d.irLen < 2) d.find("BSD001", "INSTRUCTION_LENGTH must be at least 2");
    // The TAP.
    auto tapPort = [&](const std::string& n, const char* attr, const char* mode, bool required) {
        if (n.empty()) {
            if (required) d.find("BSD007", std::string("no port carries the attribute ") + attr);
            return;
        }
        const BsdlPort* p = d.port(n);
        if (!p) d.find("BSD007", std::string(attr) + " names '" + n + "', which is not a port");
        else if (p->mode != mode || p->vec)
            d.find("BSD007", "the TAP port '" + n + "' (" + attr + ") must be a single '" + mode + " bit'");
    };
    tapPort(d.tdi, "TAP_SCAN_IN", "in", true);
    tapPort(d.tms, "TAP_SCAN_MODE", "in", true);
    tapPort(d.tck, "TAP_SCAN_CLOCK", "in", true);
    tapPort(d.tdo, "TAP_SCAN_OUT", "out", true);
    tapPort(d.trst, "TAP_SCAN_RESET", "in", false);
    // The instruction register.
    if (d.irLen >= 2) {
        if ((int)d.capture.size() != d.irLen)
            d.find("BSD002", "INSTRUCTION_CAPTURE has " + std::to_string(d.capture.size()) + " bits, the register has " +
                                 std::to_string(d.irLen));
        else if (d.capture.substr(d.irLen - 2) != "01")
            d.find("BSD002", "INSTRUCTION_CAPTURE must end in 01, found '" + d.capture + "'");
        std::map<std::string, std::string> owner;
        for (const BsdlInstr& in : d.instrs) {
            if (in.opcodes.empty()) d.find("BSD002", "the instruction " + in.name + " has no opcode");
            for (const std::string& oc : in.opcodes) {
                if ((int)oc.size() != d.irLen)
                    d.find("BSD002", in.name + ": the opcode '" + oc + "' is not " + std::to_string(d.irLen) + " bits");
                auto [it, fresh] = owner.emplace(oc, in.name);
                bool share = (it->second == "SAMPLE" && in.name == "PRELOAD") || (it->second == "PRELOAD" && in.name == "SAMPLE");
                if (!fresh && it->second != in.name && !share && oc.find('x') == std::string::npos)
                    d.find("BSD002", "the opcode " + oc + " belongs to both " + it->second + " and " + in.name);
            }
        }
        for (const char* m : {"BYPASS", "EXTEST", "SAMPLE", "PRELOAD"})
            if (!d.instr(m)) d.find("BSD002", std::string("the mandatory instruction ") + m + " is missing");
        if (const BsdlInstr* by = d.instr("BYPASS")) {
            if (std::find(by->opcodes.begin(), by->opcodes.end(), std::string(d.irLen, '1')) == by->opcodes.end())
                d.find("BSD002", "BYPASS must include the all-ones opcode");
        }
    }
    // The device identification register.
    if (!d.idcode.empty()) {
        if (d.idcode.size() != 32) d.find("BSD003", "IDCODE_REGISTER has " + std::to_string(d.idcode.size()) + " bits, not 32");
        else if (d.idcode[31] != '1') d.find("BSD003", "bit 0 of IDCODE_REGISTER must be 1");
        if (!d.instr("IDCODE")) d.find("BSD003", "IDCODE_REGISTER is given but there is no IDCODE instruction");
    } else if (d.instr("IDCODE")) d.find("BSD003", "the IDCODE instruction has no IDCODE_REGISTER");
    // The boundary register.
    std::sort(d.cells.begin(), d.cells.end(), [](const BsdlCell& a, const BsdlCell& b) { return a.num < b.num; });
    if (d.attr.count("BOUNDARY_LENGTH")) {
        if (d.blen < 1) d.find("BSD004", "BOUNDARY_LENGTH must be at least 1");
        std::vector<int> seen(std::max(d.blen, 0), 0);
        for (const BsdlCell& c : d.cells) {
            if (c.num < 0 || c.num >= d.blen)
                d.find("BSD004", "cell " + std::to_string(c.num) + " is outside BOUNDARY_LENGTH " + std::to_string(d.blen));
            else seen[c.num]++;
        }
        int missing = 0;
        for (int k = 0; k < d.blen; k++) {
            if (seen[k] == 0 && missing++ < 5) d.find("BSD004", "cell " + std::to_string(k) + " is not described");
            // A second line for one cell is legal only for a merged cell (two functions).
        }
        if (missing > 5) d.find("BSD004", std::to_string(missing - 5) + " more cells are not described");
    }
    static const std::set<std::string> funcs{"input", "output2", "output3", "control", "controlr", "internal",
                                             "clock", "bidir", "observe_only"};
    std::map<int, const BsdlCell*> byNum;
    for (const BsdlCell& c : d.cells) byNum.emplace(c.num, &c);
    std::set<std::string> covered;
    for (const BsdlCell& c : d.cells) {
        std::string id = "cell " + std::to_string(c.num);
        if (!funcs.count(c.func)) {
            d.find("BSD005", id + ": unknown function '" + c.func + "'");
            continue;
        }
        bool noPort = c.port == "*";
        if (c.func == "control" || c.func == "controlr" || c.func == "internal") {
            if (!noPort) d.find("BSD005", id + ": a " + c.func + " cell must name the port '*'");
        } else if (noPort) d.find("BSD005", id + ": a " + c.func + " cell needs a port");
        else {
            const BsdlPort* p = d.port(c.port);
            if (!p) d.find("BSD005", id + ": '" + c.port + "' is not a port");
            else {
                covered.insert(lower(p->name));
                if (p->vec && !p->has(c.bit))
                    d.find("BSD005", id + ": " + c.port + "(" + std::to_string(c.bit) + ") is outside the port's range");
                if (!p->vec && c.bit >= 0) d.find("BSD005", id + ": '" + c.port + "' is not a vector");
                bool isOut = c.func == "output2" || c.func == "output3";
                if (isOut && p->mode == "in") d.find("BSD005", id + ": an " + c.func + " cell on the input port '" + c.port + "'");
                if ((c.func == "input" || c.func == "clock") && (p->mode == "out" || p->mode == "buffer"))
                    d.find("BSD005", id + ": an " + c.func + " cell on the output port '" + c.port + "'");
                if (c.func == "bidir" && p->mode != "inout") d.find("BSD005", id + ": a bidir cell on '" + c.port + "', which is not inout");
                if (p->mode == "linkage") d.find("BSD005", id + ": the linkage port '" + c.port + "' cannot have a cell");
            }
        }
        if (c.func == "output3" || c.func == "bidir") {
            if (c.ccell == -1) d.find("BSD006", id + ": an " + c.func + " cell needs a control cell");
        }
        if (c.ccell != -1) {
            auto it = byNum.find(c.ccell);
            if (it == byNum.end()) d.find("BSD006", id + ": the control cell " + std::to_string(c.ccell) + " does not exist");
            else if (it->second->func != "control" && it->second->func != "controlr" && it->second->func != c.func)
                d.find("BSD006", id + ": cell " + std::to_string(c.ccell) + " is not a control cell");
            if (c.disval != 0 && c.disval != 1) d.find("BSD006", id + ": the disable value must be 0 or 1");
            static const std::set<std::string> rs{"Z", "WEAK0", "WEAK1", "PULL0", "PULL1", "KEEPER"};
            if (!rs.count(c.rslt)) d.find("BSD006", id + ": unknown disable result '" + c.rslt + "'");
        }
    }
    std::set<std::string> tap{lower(d.tdi), lower(d.tms), lower(d.tdo), lower(d.tck), lower(d.trst)};
    for (const BsdlPort& p : d.ports) {
        std::string l = lower(p.name);
        if (p.mode != "linkage" && !tap.count(l) && !covered.count(l))
            d.find("BSD008", "the port '" + p.name + "' has no boundary cell");
        if (d.hasPinMap) {
            auto it = d.pinCount.find(l);
            if (it == d.pinCount.end()) d.find("BSD009", "the port '" + p.name + "' is not in the pin map");
            else if (it->second != p.width())
                d.find("BSD009", "the port '" + p.name + "' has " + std::to_string(p.width()) + " bits but " +
                                     std::to_string(it->second) + " pins");
        }
    }
    return (int)d.findings.size();
}

// The device tests a BSDL file allows: instruction capture, IDCODE, BYPASS,
// the boundary register's length, SAMPLE of every input pin, EXTEST drive of
// every output pin, the disable of every 3-state driver, HIGHZ and CLAMP.
// `tb` receives the matching testbench description.
inline Pattern bsdlPatterns(const Bsdl& d, TbSpec& tb) {
    Pattern p;
    int n = d.blen, L = d.irLen;
    auto opOf = [&](const char* name) -> std::string {
        const BsdlInstr* in = d.instr(name);
        if (!in || in->opcodes.empty()) return "";
        std::string oc = in->opcodes[0];
        if (std::string(name) == "BYPASS") oc = std::string(L, '1');
        for (char& c : oc)
            if (c == 'x') c = '0';
        return fromMsbFirst(oc);
    };
    std::string bypass = opOf("BYPASS"), sample = opOf("SAMPLE"), preload = opOf("PRELOAD"), extest = opOf("EXTEST");
    std::string cap = fromMsbFirst(d.capture);
    auto sig = [&](const BsdlCell& c) {
        const BsdlPort* bp = d.port(c.port);
        return bp->vec ? bp->name + "[" + std::to_string(c.bit) + "]" : bp->name;
    };
    auto drv = [&](const BsdlCell& c) {  // what the testbench assigns to drive the pin
        const BsdlPort* bp = d.port(c.port);
        std::string base = bp->mode == "inout" ? bp->name + "_drv" : bp->name;
        return bp->vec ? base + "[" + std::to_string(c.bit) + "]" : base;
    };
    // The testbench.
    tb.dut = d.entity;
    tb.tap = true;
    tb.trst = !d.trst.empty();
    tb.tck = d.tck, tb.tms = d.tms, tb.tdi = d.tdi, tb.tdo = d.tdo, tb.trstn = d.trst;
    std::set<std::string> tap{lower(d.tdi), lower(d.tms), lower(d.tdo), lower(d.tck), lower(d.trst)};
    for (const BsdlPort& bp : d.ports) {
        if (tap.count(lower(bp.name))) continue;
        std::string rng = bp.vec ? "[" + std::to_string(bp.left) + ":" + std::to_string(bp.right) + "] " : "";
        if (bp.mode == "in") tb.pins.push_back({"logic " + rng + bp.name, bp.name, "'0"});
        else if (bp.mode == "inout") {
            tb.pins.push_back({"logic " + rng + bp.name + "_drv", "", "'z"});
            tb.pins.push_back({"wire " + rng + bp.name + " = " + bp.name + "_drv", bp.name, ""});
        } else tb.pins.push_back({"wire " + rng + bp.name, bp.name, ""});
    }
    // pins with an empty port name are testbench-only drivers
    p.reset();
    p.note("instruction register capture");
    p.ir(bypass, cap);
    if (!d.idcode.empty() && d.idcode.size() == 32) {
        p.note("IDCODE is selected after reset");
        p.reset();
        p.dr(std::string(32, '0'), fromMsbFirst(d.idcode));
        if (std::string id = opOf("IDCODE"); !id.empty()) {
            p.ir(id, cap);
            p.dr(std::string(32, '0'), fromMsbFirst(d.idcode));
        }
    }
    p.note("BYPASS: one bit, captures 0");
    p.ir(bypass, cap);
    {
        std::string in = "1011001110001011", out = "0" + in.substr(0, in.size() - 1);
        p.dr(in, out);
    }
    if (n < 1 || sample.empty() || (int)d.cells.size() < n) return p;
    std::string smp = sample.empty() ? preload : sample;
    p.note("boundary register length: " + std::to_string(n));
    p.ir(smp, cap);
    {
        std::string mark = "110100101", in = mark + std::string(n, '0'), out = std::string(n, 'x') + mark;
        p.dr(in, out);
    }
    // One description per cell number (a merged cell lists two; the first wins).
    std::vector<const BsdlCell*> cell(n, nullptr);
    for (const BsdlCell& c : d.cells)
        if (c.num >= 0 && c.num < n && !cell[c.num]) cell[c.num] = &c;
    for (const BsdlCell* c : cell)
        if (!c) return p;
    auto isIn = [&](const BsdlCell& c) { return c.func == "input" || c.func == "clock" || c.func == "observe_only"; };
    auto isOut = [&](const BsdlCell& c) { return c.func == "output2" || c.func == "output3"; };
    auto isCtl = [&](const BsdlCell& c) { return c.func == "control" || c.func == "controlr"; };
    // Which control cell value disables each control cell's drivers.
    std::map<int, int> disOf;
    for (const BsdlCell* c : cell)
        if (c->ccell >= 0) disOf[c->ccell] = c->disval;
    auto pat = [](int k, int phase) { return ((k * 7 + k / 3 + phase) & 1) ? '1' : '0'; };

    p.note("SAMPLE: every input pin is captured");
    for (int phase = 0; phase < 2; phase++) {
        std::string exp(n, 'x');
        for (int k = 0; k < n; k++) {
            const BsdlCell& c = *cell[k];
            if (!isIn(c) || d.port(c.port)->mode != "in") continue;
            char v = pat(k, phase);
            p.setPin(drv(c), std::string("1'b") + v);
            exp[k] = v;
        }
        p.dr(std::string(n, '0'), exp);
    }
    if (extest.empty()) return p;
    auto loadOutputs = [&](int phase, bool enable) {
        std::string in(n, '0');
        for (int k = 0; k < n; k++) {
            const BsdlCell& c = *cell[k];
            if (isOut(c)) in[k] = pat(k, phase);
            else if (isCtl(c)) {
                int dis = disOf.count(k) ? disOf[k] : (c.safe == '1');
                in[k] = (enable ? !dis : dis) ? '1' : '0';
            }
        }
        return in;
    };
    auto expectOutputs = [&](const std::string& in, bool enabled) {
        for (int k = 0; k < n; k++) {
            const BsdlCell& c = *cell[k];
            if (!isOut(c)) continue;
            if (enabled || c.func == "output2") p.expPin(sig(c), std::string("1'b") + in[k]);
            else if (c.rslt == "Z") p.expPin(sig(c), "1'bz");
        }
    };
    p.note("PRELOAD then EXTEST: the pins take the update latches");
    std::string a = loadOutputs(0, true);
    p.ir(preload.empty() ? smp : preload, cap);
    p.dr(a);
    p.ir(extest, cap);
    expectOutputs(a, true);
    std::string b = loadOutputs(1, true);
    p.dr(b);
    expectOutputs(b, true);
    bool anyCtl = !disOf.empty();
    if (anyCtl) {
        p.note("EXTEST: the control cells disable their drivers");
        std::string z = loadOutputs(0, false);
        p.dr(z);
        expectOutputs(z, false);
        // With the drivers off, a bidirectional pad is an input.
        std::string exp(n, 'x');
        bool any = false;
        for (int k = 0; k < n; k++) {
            const BsdlCell& c = *cell[k];
            if (!isIn(c) || d.port(c.port)->mode != "inout") continue;
            // Only pads whose drivers this register controls.
            bool controlled = false;
            for (const BsdlCell* o2 : cell)
                if (isOut(*o2) && o2->func == "output3" && lower(o2->port) == lower(c.port) && o2->bit == c.bit) controlled = true;
            if (!controlled) continue;
            char v = pat(k, 1);
            p.setPin(drv(c), std::string("1'b") + v);
            exp[k] = v, any = true;
        }
        if (any) {
            p.dr(z, exp);
            for (int k = 0; k < n; k++)
                if (exp[k] != 'x') p.setPin(drv(*cell[k]), "1'bz");
        }
    }
    if (std::string hz = opOf("HIGHZ"); !hz.empty()) {
        p.note("HIGHZ: every 3-state driver is off and BYPASS is selected");
        p.ir(preload.empty() ? smp : preload, cap);
        p.dr(a);
        p.ir(hz, cap);
        for (int k = 0; k < n; k++)
            if (cell[k]->func == "output3" && cell[k]->rslt == "Z") p.expPin(sig(*cell[k]), "1'bz");
        p.dr("1101", "0110");
    }
    if (std::string cl = opOf("CLAMP"); !cl.empty()) {
        p.note("CLAMP: the pins hold the preloaded values and BYPASS is selected");
        p.ir(preload.empty() ? smp : preload, cap);
        p.dr(b);
        p.ir(cl, cap);
        expectOutputs(b, true);
        p.dr("1101", "0110");
        expectOutputs(b, true);
    }
    p.reset();
    return p;
}

}  // namespace dft
