// bind_checker (IEEE 1801-2024 6.12): run with bindchk.upf. The checker
// watches that the core's output is known while the core is powered; the
// power intent binds it into the core instance and into every instance
// of `leaf`, with a parameter and port connections. Nothing in the HDL
// instantiates it.
module pwr_chk #(parameter string NAME = "?", parameter int W = 1)(input logic clk, input logic [W-1:0] v, input logic en);
  always @(posedge clk) if (en && $isunknown(v)) $display("%0t %s: unknown value %b", $time, NAME, v);
endmodule
module leaf(input logic clk, input logic [3:0] d, output logic [3:0] q);
  always_ff @(posedge clk) q <= d;
endmodule
module dut(input logic clk, input logic [3:0] d, input logic on, output logic [3:0] q, r);
  leaf u_core(.clk, .d, .q);
  leaf u_aon(.clk, .d, .q(r));
endmodule
module tb;
  import UPF::*;
  logic clk = 0, on = 1;
  logic [3:0] d = 4'h5, q, r;
  dut u_dut(.*);
  always #5 clk = ~clk;
  initial begin
    supply_on("VDD", 1.0);
    supply_on("VSW", 1.0);
    supply_on("VSS", 0.0);
    #12 $display("%0t q=%h r=%h", $time, q, r);
    supply_off("VSW");
    #20 $display("%0t off: q=%h r=%h", $time, q, r);
    on = 0;
    #20 $display("%0t checker off: q=%h", $time, q);
    $finish;
  end
endmodule
