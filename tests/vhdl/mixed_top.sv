module mixed_top(input logic clk, rst, output logic [3:0] cnt);
  wire [3:0] vhdl_count;
  vhdl_counter u_counter(.clk(clk), .rst(rst), .cnt(vhdl_count));
  // A real language boundary: VHDL sequential output feeds SV arithmetic.
  assign cnt = vhdl_count + 4'd1;
endmodule
