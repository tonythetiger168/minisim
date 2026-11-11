// HDL ports on supply nets through value conversion tables (IEEE 1801-2024
// 4.10, 6.18, 6.28, Annex B): run with vct.upf.
//   u_ldo.vout  a regulator model's output drives supply net VREG (SV_LOGIC2UPF)
//   u_mac.VDD   VREG drives a macro's power pin (UPF2SV_LOGIC)
//   u_mac.VSSN  VSS drives an active-low ground pin (UPF_GNDZERO2SV_LOGIC)
//   u_mac.PGOOD VREG through a table of the power intent's own (PG)
//   u_core      a domain powered by VREG
module ldo(input logic en, output logic vout);
  assign vout = en;
endmodule
module macro(input logic VDD, VSSN, PGOOD, input logic [3:0] a, output logic [3:0] y);
  assign y = (VDD === 1'b1 && VSSN === 1'b0) ? ~a : 'x;
endmodule
module core(input logic clk, input logic [3:0] a, output logic [3:0] q);
  always_ff @(posedge clk) q <= a;
endmodule
module dut(input logic clk, en, input logic [3:0] a, output logic [3:0] y, q, output logic pgood);
  logic vreg;
  ldo u_ldo(.en, .vout(vreg));
  macro u_mac(.VDD(1'b1), .VSSN(1'b0), .PGOOD(1'b0), .a, .y);
  core u_core(.clk, .a, .q);
  assign pgood = u_mac.PGOOD;
endmodule
module tb;
  import UPF::*;
  logic clk = 0, en = 0;
  logic [3:0] a = 4'h3, y, q;
  logic pgood;
  dut u_dut(.*);
  always #5 clk = ~clk;
  task show(string what);
    upfSupplyTypeT sv;
    sv = get_supply_value("VREG");
    $display("%0t %s: VREG=%s VDD=%b VSSN=%b PGOOD=%b y=%h q=%h", $time, what, sv.state.name(),
             u_dut.u_mac.VDD, u_dut.u_mac.VSSN, pgood, y, q);
  endtask
  initial begin
    #1 show("all off");
    supply_on("VDD", 1.0);
    #1 show("VDD on, no ground");
    supply_on("VSS", 0.0);
    #10 show("regulator disabled");
    en = 1;
    #10 show("regulator enabled");
    a = 4'h6;
    #10 show("running");
    en = 1'bz;
    #1 show("enable floats");
    en = 1;
    supply_off("VDD");
    #1 show("regulator's own supply off");
    supply_on("VDD", 1.0);
    #10 show("back on");
    en = 0;
    #10 show("regulator disabled");
    $finish;
  end
endmodule
