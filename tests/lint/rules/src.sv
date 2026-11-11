//! args: -only format,suspicious,STY004,STY005,STY006,STY007,STY008,STY009,NAME014,W009
`define lower_case 1 //~ NAME014
`define UPPER_CASE 1
`ifdef UPPER_CASE
//~v STY004
`endif
`ifdef UPPER_CASE
`endif // UPPER_CASE
package other_pkg; //~ STY005
  function int f(int a); //~ STY006
    return a;
  endfunction
  function automatic int g(int a);
    return a;
  endfunction
endpackage
module src #(
    parameter P = 1, //~ STY007
    parameter int Q = 1
) (
    input logic clk,
    input logic [0:7] up, //~ STY008
    input logic [7:0] d,
    output logic [7:0] q
);
  logic [7:0] mem[3:0]; //~ STY009
  logic [7:0] ok[0:3];
  logic [7:0] ok2[4];
	logic tabbed; //~ FMT002
  //~v FMT003
  logic trail;   
  logic [7:0] a_long_name_for_a_signal_1, a_long_name_for_a_signal_2, a_long_name_for_a_signal_3; //~ FMT001
  always_ff @(posedge clk) begin
    q <= d;
    if (d[1]) q <= 8'b0;
    q <= 8'b101;; //~ W009 SUS001
    q <= 8'h0f;
    q <= 8'hx;
    mem[0] <= d;
    ok[0] <= d;
    ok2[0] <= d;
  end
endmodule
function int h(); endfunction; //~ STY006 SUS001 FMT004