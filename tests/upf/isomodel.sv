// use_interface_cell -force_function (IEEE 1801-2024 6.62): run with
// isomodel.upf. The isolation cells of u_a.y are simulated by the model
// iso_and (one per bit): it isolates while EN is low, which -port_map ties
// to ~isolation_signal. u_a.z is excluded and keeps the 9.8 algorithm.
module blk(input logic [3:0] a, output logic [3:0] y, output logic z);
  assign y = a;
  assign z = a[0];
endmodule
// An AND-type isolation cell: Y = A & EN; x when its supply is off.
module iso_and(input logic A, input logic EN, input logic VDD, output logic Y);
  assign Y = VDD === 1'b1 ? A & EN : 1'bx;
endmodule
module tb;
  import UPF::*;
  logic en_a = 0, iso = 0;
  logic [3:0] a = 4'b0101;
  logic [3:0] y;
  logic z;
  blk u_a(.a, .y, .z);
  task show(string what);
    $display("%0t %s: y=%b z=%b", $time, what, y, z);
  endtask
  initial begin
    supply_on("VDD", 1.0); supply_on("VSS", 0.0); supply_on("VAON", 1.0);
    en_a = 1;
    #1 show("on");
    a = 4'b1100;
    #1 show("on, new data");
    iso = 1;
    #1 show("isolating");
    en_a = 0;
    #1 show("off, isolated");
    en_a = 1; iso = 0; a = 4'b0101;
    #1 show("on again");
    iso = 1'bx;
    #1 show("unknown isolation signal");
    iso = 1;
    supply_off("VAON");
    #1 show("isolation supply off");
    $finish;
  end
endmodule
