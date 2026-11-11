// A small datapath for mpopt: enables, a pipeline, a multiplier and a memory.
module pipe #(parameter W = 16) (
  input  logic         clk,
  input  logic         rst_n,
  input  logic         in_valid,
  input  logic [W-1:0] a,
  input  logic [W-1:0] b,
  input  logic         acc_en,
  input  logic         we,
  input  logic [3:0]   waddr,
  input  logic [3:0]   raddr_in,
  input  logic         raddr_load,
  output logic [2*W-1:0] prod,
  output logic [W-1:0] sum_q,
  output logic [W-1:0] rdata,
  output logic         flag
);
  logic [W-1:0] a_q, b_q;
  logic [2*W-1:0] prod_d;
  logic [W-1:0] acc;
  logic [3:0] raddr;
  logic [W-1:0] mem [0:15];

  // Stage 1: operands load on in_valid (an enable written in the RTL).
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) begin
      a_q <= '0;
      b_q <= '0;
    end else if (in_valid) begin
      a_q <= a;
      b_q <= b;
    end

  // Stage 2 and 3: no enable in the RTL, but nothing new arrives unless
  // stage 1 loaded: sequential clock gating.
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) prod_d <= '0;
    else prod_d <= a_q * b_q;

  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) prod <= '0;
    else prod <= prod_d + {{W{1'b0}}, a_q};

  // Two registers with different enables in one block, and a flag that
  // loads every cycle: mpopt splits the block.
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      acc   <= '0;
      sum_q <= '0;
      flag  <= 1'b0;
    end else begin
      flag <= in_valid ^ acc_en;
      if (acc_en) acc <= acc + a;
      if (in_valid && !acc_en) sum_q <= a + b;
    end
  end

  // A memory: the write port is gated by we, and the registered read only
  // reads again when the address moved or a write happened.
  always_ff @(posedge clk)
    if (we) mem[waddr] <= a;

  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) raddr <= '0;
    else if (raddr_load) raddr <= raddr_in;

  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) rdata <= '0;
    else rdata <= mem[raddr];
endmodule
