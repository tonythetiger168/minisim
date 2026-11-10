// The gate netlist reader (src/gnetlist.hpp): syntax, flattening, aliases,
// constants, buses, positional connections, write and re-read.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <sstream>

#include "gnetlist.hpp"

static const char* text = R"(
// a two-level netlist
`timescale 1ns/1ps
module sub (input [1:0] a, output y, input \esc[0] );
  wire n1;
  NAND2_X1 g0 (.A(a[1]), .B(a[0]), .Y(n1));
  (* keep *) XOR2_X1 \g/1 (.A(n1), .B(\esc[0] ), .Y(y));
endmodule

module top (clk, d, q, k);
  input clk;
  input [3:0] d;
  output [1:0] q;
  output k;
  wire [1:0] w;
  wire t, u, unused;
  supply1 vdd;
  assign k = 1'b0;
  assign u = t;
  sub s0 (.a(d[3:2]), .y(w[1]), .\esc[0] (d[0]));
  sub s1 ({d[1], vdd}, w[0], u);
  INV_X1 i0 (d[0], t);
  DFF_X1 r0 (.CK(clk), .D(w[1]), .Q(q[1])), r1 (.CK(clk), .D(w[0]), .Q(q[0]));
  REG4_X1 big (.CK(clk), .D({2'b01, w}), .Q());
endmodule
)";

int main() {
    gnl::Source src;
    gnl::parse(text, "t.v", src);
    assert(src.modules.size() == 2);
    assert((gnl::tops(src) == std::vector<std::string>{"top"}));
    const gnl::Module& sub = *src.module("sub");
    assert(sub.portOrder.size() == 3 && sub.portOrder[2] == "esc[0]");
    assert(sub.insts[1].name == "g/1" && sub.find("a")->width() == 2);

    auto pinOrder = [](const std::string& cell) {
        return cell == "INV_X1" ? std::vector<std::string>{"A", "Y"} : std::vector<std::string>{};
    };
    gnl::Netlist n = gnl::flatten(src, "top", nullptr, pinOrder);
    assert(n.top == "top" && n.warnings.empty());
    assert(n.ports.size() == 8);   // clk, d[3:0], q[1:0], k
    assert(n.ports[1].name == "d[3]" && n.ports[1].bus == "d" && n.ports[1].bit == 3);
    assert(n.insts.size() == 8);   // 2 x 2 in sub, INV, 2 DFF, REG4
    auto inst = [&](const std::string& name) -> const gnl::FlatInst& {
        for (auto& i : n.insts) if (i.name == name) return i;
        assert(!"no instance");
        return n.insts[0];
    };
    auto net = [&](const gnl::FlatInst& i, const std::string& pin) {
        for (auto& c : i.conns) if (c.pin == pin) return c.net;
        return -1;
    };
    auto port = [&](const std::string& name) {
        for (auto& p : n.ports) if (p.name == name) return p.net;
        return -1;
    };
    // Hierarchy: s0/g0.A is top's d[3]; the name is the top-level one.
    assert(net(inst("s0/g0"), "A") == port("d[3]") && n.nets[port("d[3]")] == "d[3]");
    assert(net(inst("s0/g0"), "B") == port("d[2]"));
    assert(net(inst("s0/g/1"), "B") == port("d[0]"));
    assert(n.nets[net(inst("s0/g0"), "Y")] == "s0/n1");
    assert(net(inst("s0/g/1"), "Y") == net(inst("r0"), "D") && n.nets[net(inst("r0"), "D")] == "w[1]");
    // Positional: s1.a = {d[1], vdd}; vdd is tied to 1.
    assert(net(inst("s1/g0"), "A") == port("d[1]"));
    assert(net(inst("s1/g0"), "B") == n.const1 && n.const1 >= 0);
    // assign u = t: the INV output reaches s1's third port.
    assert(net(inst("i0"), "Y") == net(inst("s1/g/1"), "B") && net(inst("i0"), "A") == port("d[0]"));
    // assign k = 1'b0
    assert(port("k") == n.const0 && n.nets[n.const0] == "1'b0");
    // A bus pin on an unknown cell: D[3] .. D[0], constants msb first.
    const gnl::FlatInst& big = inst("big");
    assert(net(big, "D[3]") == n.const0 && net(big, "D[2]") == n.const1);
    assert(net(big, "D[1]") == net(inst("r0"), "D") && net(big, "D[0]") == net(inst("r1"), "D"));
    assert(net(big, "Q") == -1 && net(inst("r1"), "Q") == port("q[0]"));

    // Write and read back: the same flat netlist.
    std::ostringstream os;
    gnl::write(src, os);
    gnl::Source src2;
    gnl::parse(os.str(), "w.v", src2);
    gnl::Netlist n2 = gnl::flatten(src2, "top", nullptr, pinOrder);
    assert(n2.insts.size() == n.insts.size() && n2.nets.size() == n.nets.size());
    for (size_t i = 0; i < n.insts.size(); i++) {
        assert(n.insts[i].name == n2.insts[i].name && n.insts[i].cell == n2.insts[i].cell);
        assert(n.insts[i].conns.size() == n2.insts[i].conns.size());
        for (size_t k = 0; k < n.insts[i].conns.size(); k++) {
            auto &a = n.insts[i].conns[k], &b = n2.insts[i].conns[k];
            assert(a.pin == b.pin && (a.net < 0 ? b.net < 0 : n.nets[a.net] == n2.nets[b.net]));
        }
    }
    assert(os.str().find("\\g/1  (") != std::string::npos);

    // Errors.
    auto bad = [](const char* t, const char* want) {
        gnl::Source s;
        try { gnl::parse(t, "e.v", s); gnl::flatten(s, "m"); }
        catch (gnl::Error& e) { return std::string(e.what()).find(want) != std::string::npos; }
        return false;
    };
    assert(bad("module m; always @(*) x = 1; endmodule", "e.v:1: 'always'"));
    assert(bad("module m (input a, output y); assign y = a & a; endmodule", "only a net"));
    assert(bad("module m; INV_X1 u (a, b); endmodule", "positional connections need"));
    assert(bad("module m; m u (); endmodule", "instantiates itself"));
    assert(bad("module m; wire [1:0] w; BUF u (.A(w[5])); endmodule", "index 5 is outside w"));
    std::puts("test_gnetlist: ok");
    return 0;
}
