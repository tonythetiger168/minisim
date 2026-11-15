// The parallel cycle domain with public registers: each unit has a register
// file read at a run-time index (its elements stay in S.sigs) and a memory
// written a byte at a time at a run-time address (NBAs through the kernel's
// list). 40 units give the domain enough islands to run in groups; the
// result must match the interpreter.
module unit #(parameter logic [31:0] SEED = 1) (input logic clk, rst, output logic [31:0] sum);
  logic [31:0] rf [0:7];
  logic [31:0] mem [0:255];
  logic [31:0] x, rd;
  logic [2:0] ra, wa;
  logic [7:0] ma;
  assign rd = rf[ra];
  always_ff @(posedge clk) begin
    if (rst) begin
      x <= SEED; ra <= 0; wa <= 1; ma <= 0;
    end else begin
      x <= {x[30:0], x[31] ^ x[21] ^ x[1] ^ x[0]};
      ra <= x[2:0];
      wa <= x[5:3];
      ma <= x[13:6];
      rf[wa] <= rd + x;
      if (x[10]) mem[ma][7:0] <= rd[7:0];
      if (x[11]) mem[ma][15:8] <= rd[15:8] ^ x[15:8];
      if (x[12]) mem[ma][31:16] <= x[31:16];
    end
  end
  assign sum = mem[3] ^ mem[9] ^ rf[5];
  initial begin
    for (int k = 0; k < 8; k++) rf[k] = 32'd0;
    for (int k = 0; k < 256; k++) mem[k] = 32'd0;
  end
endmodule

// Instance names without [ ]: generate-loop names keep a signal in S.sigs.
module quad #(parameter logic [31:0] B = 1) (input logic clk, rst, output logic [31:0] s0, s1, s2, s3);
  unit #(.SEED(B * 32'h9e3779b9)) u0(.clk(clk), .rst(rst), .sum(s0));
  unit #(.SEED(B * 32'h3c6ef372)) u1(.clk(clk), .rst(rst), .sum(s1));
  unit #(.SEED(B * 32'hdaa66d2b)) u2(.clk(clk), .rst(rst), .sum(s2));
  unit #(.SEED(B * 32'h78dde6e4)) u3(.clk(clk), .rst(rst), .sum(s3));
endmodule

module parpub;
  logic clk = 0, rst = 1;
  logic [31:0] sum [40];
  quad #(.B(1)) q0(.clk(clk), .rst(rst), .s0(sum[0]), .s1(sum[1]), .s2(sum[2]), .s3(sum[3]));
  quad #(.B(2)) q1(.clk(clk), .rst(rst), .s0(sum[4]), .s1(sum[5]), .s2(sum[6]), .s3(sum[7]));
  quad #(.B(3)) q2(.clk(clk), .rst(rst), .s0(sum[8]), .s1(sum[9]), .s2(sum[10]), .s3(sum[11]));
  quad #(.B(4)) q3(.clk(clk), .rst(rst), .s0(sum[12]), .s1(sum[13]), .s2(sum[14]), .s3(sum[15]));
  quad #(.B(5)) q4(.clk(clk), .rst(rst), .s0(sum[16]), .s1(sum[17]), .s2(sum[18]), .s3(sum[19]));
  quad #(.B(6)) q5(.clk(clk), .rst(rst), .s0(sum[20]), .s1(sum[21]), .s2(sum[22]), .s3(sum[23]));
  quad #(.B(7)) q6(.clk(clk), .rst(rst), .s0(sum[24]), .s1(sum[25]), .s2(sum[26]), .s3(sum[27]));
  quad #(.B(8)) q7(.clk(clk), .rst(rst), .s0(sum[28]), .s1(sum[29]), .s2(sum[30]), .s3(sum[31]));
  quad #(.B(9)) q8(.clk(clk), .rst(rst), .s0(sum[32]), .s1(sum[33]), .s2(sum[34]), .s3(sum[35]));
  quad #(.B(10)) q9(.clk(clk), .rst(rst), .s0(sum[36]), .s1(sum[37]), .s2(sum[38]), .s3(sum[39]));
  always #5 clk = ~clk;
  initial begin
    logic [31:0] chk;
    #20 rst = 0;
    for (int r = 0; r < 4; r++) begin
      #2000;
      chk = 0;
      for (int i = 0; i < 40; i++) chk = (chk ^ sum[i]) + 32'h1234567 * i;
      $display("round %0d checksum=%h u7=%h", r, chk, sum[7]);
    end
    $finish;
  end
endmodule
