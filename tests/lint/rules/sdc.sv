//! args: -top sdc -only sdc -sdc $DIR/rules/sdc.sdc
// The constraints against the design (src/lintsdc.hpp), with sdc.sdc.
//~@ sdc.sdc:9 SDC005
//~@ sdc.sdc:10 SDC005
module sdc_sub (input logic ck, input logic d, output logic q);
  always_ff @(posedge ck) q <= d;
endmodule

module sdc (
  input  logic       clk,          // a clock, defined in the SDC
  input  logic       rst_n,        // an asynchronous reset: needs no input delay
  input  logic       scan_mode,    // set_case_analysis: constant
  input  logic       test_in,      // set_false_path -from
  input  logic [3:0] din,          // constrained
  input  logic       en,           //~ SDC001
  input  logic       spare_clk,    //~ SDC003
  input  logic       arst,         //~ SDC004
  output logic [3:0] dout,         // constrained
  output logic       flag,         //~ SDC002
  output logic       dbg           // set_false_path -to
);
  logic [3:0] r;
  logic f2;
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) r <= '0;
    else if (en && !scan_mode) r <= din;
  always_ff @(posedge clk or posedge arst)
    if (arst) f2 <= 1'b0;
    else f2 <= test_in;
  sdc_sub u_sub (.ck(clk), .d(f2), .q(flag));
  assign dout = r;
  assign dbg = f2;
endmodule
