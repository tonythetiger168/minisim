// Corner cases for mpopt: what it must leave alone, and what it must get right.
`define REG(q, d) always @(posedge clk) q <= d;
module edge_cases (
  input              clk,
  input              rst,
  input              arst_n,
  input              scan_en,
  input              en,
  input              en2,
  input        [1:0] op,
  input        [7:0] d,
  input        [7:0] e,
  output reg   [7:0] cnt,
  output reg   [7:0] follow,
  output reg   [7:0] neg_q,
  output reg   [7:0] blk_q,
  output reg   [7:0] lab_a,
  output reg   [7:0] lab_b,
  output reg   [3:0] hi,
  output reg   [3:0] lo,
  output reg   [7:0] casez_q,
  output reg   [7:0] same_q,
  output reg   [7:0] l1, l2,
  output reg   [7:0] mac_q,
  output reg   [7:0] fn_q,
  output       [7:0] arr_o,
  output reg   [7:0] s1, s2, s3, s4, s5, s6,
  output reg  [15:0] wide,
  output reg   [7:0] nb_a, nb_b
);
  function automatic f(input [7:0] x);
    f = ^x;
  endfunction

  // A counter reads its own value: enable gating yes, sequential gating no.
  always @(posedge clk or negedge arst_n)
    if (!arst_n) cnt <= 8'd0;
    else if (en) cnt <= cnt + 8'd1;

  // follow only changes after cnt did.
  always @(posedge clk or negedge arst_n)
    if (!arst_n) follow <= 8'd0;
    else follow <= cnt ^ 8'h5a;

  // A falling-edge register: left alone.
  always @(negedge clk)
    if (en) neg_q <= d;

  // A blocking assignment: left alone.
  reg [7:0] tmp;
  always @(posedge clk) begin
    tmp = d + e;
    if (en) blk_q <= tmp;
  end

  // A named block cannot be split: one gate with the OR of the enables.
  always @(posedge clk) begin : named
    if (en) lab_a <= d;
    if (en2) lab_b <= e;
  end

  // A concatenation on the left ties hi and lo together.
  always @(posedge clk)
    if (rst) {hi, lo} <= 8'd0;
    else if (en & en2) {hi, lo} <= d;

  // casez with a wildcard, and a default that assigns.
  always @(posedge clk)
    casez (op)
      2'b1?: casez_q <= d;
      2'b01: casez_q <= e;
      default: casez_q <= casez_q;
    endcase

  // Both branches assign: no enable, the register loads every cycle.
  always @(posedge clk)
    if (en) same_q <= d;
    else same_q <= e;

  // Two blocks on one line.
  always @(posedge clk) if (en) l1 <= d;  always @(posedge clk) if (en2) l2 <= e;

  // A block that comes from a macro: left alone.
  `REG(mac_q, d)

  // A function call in the condition: no enable can be taken from it.
  always @(posedge clk)
    if (f(d)) fn_q <= e;

  // An array reset in a loop, written under an enable.
  reg [7:0] arr [0:3];
  integer k;
  always @(posedge clk or negedge arst_n)
    if (!arst_n) begin
      for (k = 0; k < 4; k = k + 1) arr[k] <= 8'd0;
    end else if (en2) arr[op] <= d;
  assign arr_o = arr[0] ^ arr[3];

  // A six-stage pipeline behind one enable: the sequential enable follows
  // it for -seq_depth stages.
  always @(posedge clk) if (rst) s1 <= 8'd0; else if (en) s1 <= d;
  always @(posedge clk) if (rst) s2 <= 8'd0; else s2 <= s1 + 8'd1;
  always @(posedge clk) if (rst) s3 <= 8'd0; else s3 <= s2 + 8'd1;
  always @(posedge clk) if (rst) s4 <= 8'd0; else s4 <= s3 + 8'd1;
  always @(posedge clk) if (rst) s5 <= 8'd0; else s5 <= s4 + 8'd1;
  always @(posedge clk) if (rst) s6 <= 8'd0; else s6 <= s5 + 8'd1;

  // A reset branch that is one statement, in a block that is split: the
  // copy for nb_b must not keep nb_a's reset.
  always @(posedge clk or negedge arst_n)
    if (!arst_n) nb_a <= 8'd1;
    else begin
      if (en) nb_a <= d;
      if (en2) nb_b <= e;
    end

  // An enable of its own, from a register, and stable sources: both are used.
  reg take;
  always @(posedge clk) if (rst) take <= 1'b0; else if (en) take <= en2;
  always @(posedge clk)
    if (rst) wide <= 16'd0;
    else if (take) wide <= {s1, s2};
endmodule
