// -app toggle: bits that can never be 0 or never be 1 after the reset.
module toggle(input logic clk, input logic rst, input logic en, input logic [3:0] d);
  logic [3:0] cnt;      // counts 0..9: bit 3 and the others all toggle
  logic [2:0] st;       // one-hot-ish: never reaches 3'b111, bit patterns: 1,2,4 -> every bit toggles
  logic [3:0] even;     // always even: bit 0 never 1
  logic       tied;     // constant 1: never 0
  logic [1:0] sat;      // saturates at 2: bit pattern 11 never seen, but each bit toggles
  logic       dead;     // set only when cnt == 12, which never happens: never 1
  assign tied = 1'b1;
  always_ff @(posedge clk) begin
    if (rst) begin
      cnt <= '0; st <= 3'b001; even <= '0; sat <= '0; dead <= 1'b0;
    end else begin
      if (en) cnt <= cnt == 4'd9 ? 4'd0 : cnt + 4'd1;
      st <= {st[1:0], st[2]};
      even <= {d[3:1], 1'b0};
      if (sat != 2'd2) sat <= sat + 2'd1;
      if (cnt == 4'd12) dead <= 1'b1;
    end
  end
endmodule
