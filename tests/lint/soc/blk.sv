// A block with an input it never reads and an output it never drives,
// signed off on its own (tests/mlint.cmake) into blk.abs.json.
module blk #(parameter int W = 8) (
    input  logic         clk,
    input  logic         rst_n,
    input  logic [W-1:0] d,
    input  logic         spare,
    output logic [W-1:0] q,
    output logic         nc
);
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) q <= '0;
    else q <= d;
endmodule
