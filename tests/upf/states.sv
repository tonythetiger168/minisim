// Power states and simstates (IEEE 1801-2024 6.5, 9.3 to 9.6), legacy PSTs
// (6.4, 6.24) and the package UPF functions (11.2): run with states.upf.
module blk(input logic clk, input logic [3:0] a, output logic [3:0] y, output logic [3:0] r);
  assign y = ~a;
  always_ff @(posedge clk) r <= a;
endmodule
module tb;
  import UPF::*;
  logic clk = 0, mode = 0;
  logic [3:0] a = 4'h3, y, r;
  upfSupplyTypeT sv;
  blk u_b(.*);
  always #5 clk = ~clk;
  task show(string what);
    $display("%0t %s: y=%h r=%h", $time, what, y, r);
  endtask
  initial begin
    supply_on("VSS", 0.0);
    supply_on("/VDD_B", 0.9);
    #11 show("HV");
    sv = get_supply_value("VDD_B");
    $display("VDD_B: %s %0d uV on=%0d", sv.state.name(), sv.voltage, get_supply_on_state(sv));
    // low voltage: CORRUPT_ON_ACTIVITY corrupts whatever is written
    supply_on("VDD_B", 0.6);
    #1 show("LV, no activity");
    a = 4'h5;
    #1 show("LV, input changed");
    // retention voltage: CORRUPT_STATE_ON_CHANGE keeps combinational logic
    supply_on("VDD_B", 0.75);
    #1 show("RV");
    #10 show("RV, a flop that changes is corrupted");
    supply_on("VDD_B", 0.9);
    a = 4'h6;
    #10 show("HV again");
    // not a state of the PST: an error once the time step settles
    supply_on("VDD_B", 1.2);
    #1 supply_on("VDD_B", 0.9);
    // an illegal domain state
    mode = 1;
    #1 mode = 0;
    // set_power_state on the supply set
    void'(set_power_state("PD_B.primary", "OFF"));
    #1 show("set_power_state OFF");
    void'(set_power_state("PD_B.primary", "HV"));
    #1 show("set_power_state HV");
    sv = get_supply_value("PD_B.primary.power");
    $display("PD_B.primary.power: %s %0d uV", sv.state.name(), sv.voltage);
    supply_partial_on("VDD_B", 0.9);
    #1 show("partial on");
    void'(set_supply_state("VDD_B", FULL_ON));
    #1 show("set_supply_state FULL_ON");
    if (!supply_on("NO_SUCH", 1.0)) $display("supply_on of an unknown supply returns 0");
    $finish;
  end
endmodule
