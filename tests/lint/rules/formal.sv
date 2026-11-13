//! args: -only formal -top fml
// Formal-assisted checks: each finding is proved by enumerating the values
// the signals can take; the clean lines next to them are the same code made
// safe.
module fml (
    input  logic       clk,
    input  logic       rst_n,
    input  logic [2:0] sel,
    input  logic       a,
    input  logic       b,
    input  logic [7:0] d,
    output logic [7:0] q,
    output logic [7:0] y,
    output logic [7:0] z,
    output logic       o
);
  logic [7:0] mem[0:5];
  logic [2:0] cnt;  // wraps at 5: always a valid index
  logic [2:0] wild;  // free running: reaches 6 and 7
  logic [1:0] stuck; //~ FML003
  logic [2:0] idx;
  assign idx = sel;
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) begin
      cnt   <= 0;
      wild  <= 0;
      stuck <= 2'd1;
    end else begin
      cnt <= (cnt == 3'd5) ? 3'd0 : cnt + 3'd1;
      wild <= wild + 3'd1;
      if (a) stuck <= stuck;
      mem[cnt] <= d;
      mem[wild] <= d; //~ FML002
    end
  always_comb begin
    y = mem[idx]; //~ FML002
    o = 1'b0;
    if (a) begin
      if (!a) o = 1'b1; //~ FML001
      else if (b) o = 1'b0;
    end
    case (sel)
      3'd1, 3'd2: o = 1'b1;
      3'd2: o = 1'b0; //~ FML001
      default: o = 1'b0;
    endcase
    if (sel < 3'd6) z = mem[sel];
    else z = 8'd0;
    q = d;
    q[sel+:2] = 2'b11; //~ FML002
    if (sel < 3'd7) q[sel+:2] = 2'b01;
  end
endmodule
