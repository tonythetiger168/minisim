//! args: -top soc -only soc -abstract $DIR/soc/blk.abs.json
// The top, checked against block blk's abstract without blk's RTL.
module soc (
    input  logic       clk,
    input  logic       rst_n,
    input  logic [7:0] a,
    input  logic       s,
    output logic [7:0] y,
    output logic       z
);
  blk #(.W(8)) u_ok (.clk(clk), .rst_n(rst_n), .d(a), .spare(1'b0), .q(y), .nc()); //~ ABS004
  blk #(.W(8)) u_bad (.clk(1'b0), .rst_n(rst_n), .d(a), .spare(s), .q(), .nc(z)); //~ ABS003 ABS002 ABS001
endmodule
