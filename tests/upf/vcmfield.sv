// Table VCMs on fields of struct ports (IEEE 1801-2024 6.30 -field):
// run with vcmfield.upf.
//   u_reg.r     field v (real) of an unpacked struct drives VR
//   u_load.ri   VR drives fields v (real) and en (one bit) of the same struct
//   u_sens.s    field mode of a packed struct drives VS
package pw_pkg;
  typedef struct { real v; bit en; } rail_t;
  typedef struct packed { logic [1:0] mode; logic ok; } stat_t;
endpackage
module reg_m(output pw_pkg::rail_t r);
  pw_pkg::rail_t rv = '{0.0, 1'b0};
  assign r = rv;
endmodule
module sens(output pw_pkg::stat_t s);
  pw_pkg::stat_t sv = '{2'd0, 1'b1};
  assign s = sv;
endmodule
module load_m(input pw_pkg::rail_t ri);
endmodule
module spare(output pw_pkg::stat_t s2);   // for the error cases in tests/upf.cmake
  assign s2 = '0;
endmodule
module dut;
  pw_pkg::rail_t r;
  pw_pkg::stat_t s;
  reg_m u_reg(.r);
  sens u_sens(.s);
  load_m u_load(.ri('{0.0, 1'b0}));
  pw_pkg::stat_t s2;
  spare u_spare(.s2);
endmodule
module tb;
  import UPF::*;
  dut u_dut();
  task show(string what);
    upfSupplyTypeT a, b;
    a = get_supply_value("VR");
    b = get_supply_value("VS");
    $display("%0t %s: VR=%s %0d VS=%s %0d | ri.v=%g ri.en=%b", $time, what, a.state.name(), a.voltage,
             b.state.name(), b.voltage, u_dut.u_load.ri.v, u_dut.u_load.ri.en);
  endtask
  initial begin
    supply_on("VDD", 1.0);
    supply_on("VSS", 0.0);
    #1 show("start");
    u_dut.u_reg.rv.v = 1.1;
    u_dut.u_sens.sv.mode = 2'd3;
    #1 show("on");
    u_dut.u_reg.rv.v = 0.8;
    u_dut.u_sens.sv.mode = 2'd1;
    #1 show("partial");
    u_dut.u_reg.rv.v = 0.1;
    u_dut.u_sens.sv.mode = 2'd0;
    #1 show("off");
    $finish;
  end
endmodule
