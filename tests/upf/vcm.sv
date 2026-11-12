// Value conversion methods and UPF libraries (IEEE 1801-2024 4.10, 5.11,
// 6.29, 6.30, 6.36, 6.63): run with vcm.upf.
//   u_ldo.vout  a real regulator output drives VR (real hdl2upf table)
//   u_ana.vr    VR drives a real power pin (real upf2hdl table)
//   u_ts.t      an enum temperature sensor drives VE (enum hdl2upf table)
//   u_ana.vi    VE drives an integer pin (integer upf2hdl table)
//   u_sreg.s    a upfSupplyTypeT output drives VS with no conversion
//   u_ana.vs    VS drives a upfSupplyTypeT input
package temp_pkg;
  typedef enum {FROZEN, COLD, MILD, WARM, HOT} temp_e;
endpackage
module ldo(output real vout);
  real v = 0.0;
  assign vout = v;
endmodule
module tsens import temp_pkg::*; (output temp_e t);
  temp_e tv = FROZEN;
  assign t = tv;
endmodule
module sreg import UPF::*; (output upfSupplyTypeT s);
  upfSupplyTypeT sv = '0;
  assign s = sv;
endmodule
module ana import UPF::*; (input real vr, input integer vi, input upfSupplyTypeT vs);
endmodule
module dut;
  real r;
  temp_pkg::temp_e t;
  UPF::upfSupplyTypeT s;
  ldo u_ldo(.vout(r));
  tsens u_ts(.t);
  sreg u_sreg(.s);
  ana u_ana(.vr(0.0), .vi(0), .vs('0));
endmodule
module tb;
  import UPF::*;
  import temp_pkg::*;
  dut u_dut();
  task show(string what);
    upfSupplyTypeT a, b, c;
    a = get_supply_value("VR");
    b = get_supply_value("VE");
    c = get_supply_value("VS");
    $display("%0t %s: VR=%s %0d VE=%s %0d VS=%s %0d | vr=%g vr_bits=%h vi=%0d vs=%s %0d", $time, what,
             a.state.name(), a.voltage, b.state.name(), b.voltage, c.state.name(), c.voltage,
             u_dut.u_ana.vr, $realtobits(u_dut.u_ana.vr), u_dut.u_ana.vi, u_dut.u_ana.vs.state.name(),
             u_dut.u_ana.vs.voltage);
  endtask
  initial begin
    supply_on("VDD", 1.0);
    supply_on("VSS", 0.0);
    #1 show("start");
    u_dut.u_ldo.v = 1.15;
    u_dut.u_ts.tv = HOT;
    u_dut.u_sreg.sv = '{FULL_ON, 900000};
    #1 show("on");
    u_dut.u_ldo.v = 0.7;
    u_dut.u_ts.tv = MILD;
    u_dut.u_sreg.sv = '{PARTIAL_ON, 450000};
    #1 show("partial");
    u_dut.u_ldo.v = 0.2;
    u_dut.u_ts.tv = COLD;
    u_dut.u_sreg.sv = '{OFF, 0};
    #1 show("off");
    u_dut.u_ts.tv = WARM;
    #1 show("warm: no row");
    $finish;
  end
endmodule
