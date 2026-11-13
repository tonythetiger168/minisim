// HDL ports of VHDL models on supply nets through the VHDL tables of IEEE
// 1801-2024 Annex B: run with vct_vhdl.vhd and vct_vhdl.upf.
//   u_ldo.vout  a VHDL regulator's std_logic output drives VREG (VHDL_SL2UPF, B.2)
//   u_mac.VDD   VREG drives a VHDL macro's power pin (UPF2VHDL_SL, B.3)
//   u_mac.VSSN  VSS drives an active-low ground pin (UPF_GNDZERO2VHDL_SL, B.5)
//   u_ldo.gnd_n its active-low ground output drives VSSR (VHDL_SL2UPF_GNDZERO, B.4)
//   u_mac.TIEH  tied high while VREG is on (VHDL_TIED_HI, B.10)
//   u_mac.TIEL  tied low while VREG is on (VHDL_TIED_LO, B.12)
module core(input logic clk, input logic [3:0] a, output logic [3:0] q);
  always_ff @(posedge clk) q <= a;
endmodule
module dut(input logic clk, en, input logic [3:0] a, output logic [3:0] y, q, output logic pgood);
  logic vreg;
  ldo u_ldo(.en, .vout(vreg), .gnd_n());
  macro u_mac(.VDD(1'b1), .VSSN(1'b0), .TIEH(1'b0), .TIEL(1'b1), .a, .y);
  core u_core(.clk, .a, .q);
  assign pgood = u_mac.TIEH;
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
    upfSupplyTypeT gv;
    sv = get_supply_value("VREG");
    gv = get_supply_value("VSSR");
    $display("%0t %s: VREG=%s VSSR=%s VDD=%b VSSN=%b TIEH=%b TIEL=%b y=%h q=%h", $time, what, sv.state.name(), gv.state.name(),
             u_dut.u_mac.VDD, u_dut.u_mac.VSSN, pgood, u_dut.u_mac.TIEL, y, q);
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
