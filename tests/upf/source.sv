// create_abstract_power_source (IEEE 1801-2024 6.16): run with source.upf.
// An abstract source switches the core's supply set between two inputs:
// the high rail when hi is 1, the low rail when lo is 1, off otherwise; it
// acknowledges being on (and off), two time units later, as an inertial
// delay.
module core(input logic [3:0] d, output logic [3:0] q);
  assign q = ~d;
endmodule
module dut(input logic hi, input logic lo, input logic [3:0] d, output logic [3:0] q, output logic ack);
  core u_core(.d, .q);
endmodule
module tb;
  import UPF::*;
  logic hi = 0, lo = 0, ack;
  logic [3:0] d = 4'h3, q;
  dut u_dut(.*);
  task show(string what);
    $display("%0t %s: q=%h ack=%b", $time, what, q, ack);
  endtask
  initial begin
    supply_on("VHI", 1.0);
    supply_on("VLO", 0.7);
    supply_on("VSS", 0.0);
    #1 show("off");
    hi = 1;
    #1 show("hi");
    #2 show("hi, acknowledged");
    hi = 0;
    lo = 1;
    #3 show("lo");
    lo = 0;
    #3 show("off again");
    $finish;
  end
endmodule
