// Logic ports and nets made by UPF, and repeaters (IEEE 1801-2024 6.13,
// 6.19, 6.20, 6.52, 9.10): run with logic.upf. The power controller's
// outputs are not connected in the HDL; the UPF routes them: iso to the
// isolation strategy and to a new output port of the design, mode to an
// input of the core that the HDL ties to 0. The core's output `r` goes
// through a repeater that has its own supply.
module pmu(input logic req, input logic m, output logic iso, output logic mode);
  assign iso = req;
  assign mode = m;
endmodule
module core(input logic [3:0] d, input logic mode, output logic [3:0] q, output logic [3:0] r);
  assign q = mode ? ~d : d;
  assign r = d;
endmodule
module dut(input logic req, input logic m, input logic [3:0] d, output logic [3:0] q, output logic [3:0] r);
  pmu u_pmu(.req, .m, .iso(), .mode());
  core u_core(.d, .mode(1'b0), .q, .r);
endmodule
module tb;
  import UPF::*;
  logic req = 0, m = 0;
  logic [3:0] d = 4'h5, q, r;
  dut u_dut(.*);
  task show(string what);
    $display("%0t %s: q=%h r=%h", $time, what, q, r);
  endtask
  initial begin
    supply_on("VDD", 1.0);
    supply_on("VDD_REP", 1.0);
    supply_on("VSS", 0.0);
    #1 show("on");
    m = 1;
    #1 show("mode from the pmu");
    req = 1;
    #1 show("isolated by the pmu");
    req = 0;
    supply_off("VDD_REP");
    #1 show("repeater supply off");
    supply_on("VDD_REP", 1.0);
    #1 show("repeater supply on");
    $finish;
  end
endmodule
