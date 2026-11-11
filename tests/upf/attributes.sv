// Attributes, equivalent supplies, composite domains and explicit supply
// connections (IEEE 1801-2024 5.6, 6.8, 6.15, 6.17, 6.44, 6.47, 6.50, 6.51,
// 6.56): run with attributes.upf.
//   u_a, u_b  two domains on equivalent supplies, in one composite domain
//             that carries the isolation strategy
//   u_m       a hard macro: only its outputs follow its supply
//   u_i       a power-aware model: the simulator leaves it alone
//   u_e       in the top domain, but connected to its own supply set
module leaf(input logic [3:0] d, output logic [3:0] q);
  assign q = d;
endmodule
module macro(input logic [3:0] d, output logic [3:0] q);
  logic [3:0] keep;
  assign keep = ~d;
  assign q = keep;
endmodule
module ip(input logic [3:0] d, output logic [3:0] q);
  assign q = d + 4'd1;
endmodule
module dut(input logic iso, input logic [3:0] d, output logic [3:0] qa, qb, qm, qi, qe);
  leaf u_a(.d, .q(qa));
  leaf u_b(.d, .q(qb));
  macro u_m(.d, .q(qm));
  ip u_i(.d, .q(qi));
  leaf u_e(.d, .q(qe));
endmodule
module tb;
  import UPF::*;
  logic iso = 0;
  logic [3:0] d = 4'h5, qa, qb, qm, qi, qe;
  dut u_dut(.*);
  task show(string what);
    $display("%0t %s: qa=%h qb=%h qm=%h keep=%h qi=%h qe=%h", $time, what, qa, qb, qm, u_dut.u_m.keep, qi, qe);
  endtask
  initial begin
    supply_on("VDD", 1.0);
    supply_on("VDD_SW", 1.0);
    supply_on("VDD_E", 1.0);
    supply_on("VDD_EXT", 1.0);
    supply_on("VSS", 0.0);
    #1 show("on");
    iso = 1;
    #1 show("isolated");
    iso = 0;
    supply_off("VDD_SW");
    #1 show("VDD_SW off");
    supply_partial_on("VDD_SW", 1.0);
    #1 show("VDD_SW partial on");
    supply_on("VDD_SW", 1.0);
    supply_off("VDD_E");
    #1 show("VDD_E off");
    supply_on("VDD_E", 1.0);
    supply_off("VDD_EXT");
    #1 show("driver of d off");
    supply_on("VDD_EXT", 1.0);
    #1 show("on again");
    $finish;
  end
endmodule
