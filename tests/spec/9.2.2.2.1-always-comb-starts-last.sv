// IEEE 1800-2023 9.2.2.2.1: always_comb and always_latch run once at time
// zero after every initial and always procedure has started, so an event
// they trigger at time zero wakes an always @(e) declared after them.
module t;
  logic i = 0, o, q;
  event ec, el;
  int nc = 0, nl = 0;
  always_comb begin
    o = i;
    -> ec;
  end
  always_latch begin
    if (i) q = i;
    -> el;
  end
  always @(ec) nc++;
  always @(el) nl++;
  initial begin
    #1 i = 1;
    #1 $display("%0d %0d %b %b", nc, nl, o, q);
  end
endmodule
//> 2 2 1 1
