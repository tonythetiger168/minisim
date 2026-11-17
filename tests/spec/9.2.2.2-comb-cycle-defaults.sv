// IEEE 1800-2023 9.2.2.2, 4.7: two always_comb blocks that read each other's
// outputs, each writing a default first and its value after. Every run
// changes an output and back, but no other process runs in between, so the
// reader sees only the final value: the pair settles at one time step
// instead of waking each other forever.
//! icarus-skip: Icarus 13 re-runs the blocks forever on the passing changes
module top;
  logic [1:0] mode;
  logic [3:0] cnt, lim;
  logic [7:0] addr, word;
  always_comb begin : count_up
    cnt = 0;
    word = 0;
    if (mode != 0) begin
      int i;
      i = 0;
      while (i < lim) begin cnt = cnt + 4'd1; i++; end
      word = addr;
    end
  end
  always_comb begin : pick_addr
    addr = 0;
    if (cnt != 0) addr = 8'h40 + 8'(cnt);
  end
  initial begin
    lim = 3;
    mode = 0;
    #10 $display("%0d %h %h", cnt, addr, word);
    mode = 1;
    #10 $display("%0d %h %h", cnt, addr, word);
    lim = 5;
    #10 $display("%0d %h %h", cnt, addr, word);
    mode = 0;
    #10 $display("%0d %h %h", cnt, addr, word);
  end
endmodule
//> 0 00 00
//> 3 43 43
//> 5 45 45
//> 0 00 00
