// -compile stores an NBA at once (codegen.hpp, direct NBAs) also when only
// comb logic reads the register: the comb pass runs after the commit. An
// input the testbench changes on the same clock edge must not let that pass
// run early and see the new register values (DAHEAD). Against the interpreter.
module dircomb_core (input logic clk, input logic [7:0] in, output logic [7:0] z, output logic [7:0] w);
  logic [7:0] r = 0, s = 0, t;
  assign z = r ^ in;             // an assign reads r
  always @* t = s + 8'd3;        // an @* block reads s
  assign w = t ^ r;
  always @(posedge clk) begin
    r <= r + 8'd5;
    s <= s ^ 8'h21;
  end
endmodule
module dircomb;
  logic clk = 0;
  logic [7:0] in = 0;
  wire [7:0] z, w;
  dircomb_core u (.clk, .in, .z, .w);
  always #5 clk = ~clk;
  initial forever begin
    @(posedge clk);
    in = in + 8'd1;              // the same time step as the NBAs
  end
  initial begin
    repeat (8) @(negedge clk) $display("z=%h w=%h", z, w);
    $finish;
  end
endmodule
