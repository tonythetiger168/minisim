// IEEE 1800-2023 19.6.1.3: a cross bin given by a set of value tuples
// (a CrossQueueType value) over a coverpoint wider than 64 bits. A tuple
// of the cross belongs to the bin when its bins hold one of the value
// tuples; with `matches $`, when they hold only such tuples.
//! icarus-skip: Icarus 13 does not support covergroups
module t;
  bit [71:0] a;
  bit [1:0] b;
  localparam bit [71:0] B = 72'h80_0000_0000_0000_0000;   // 2^71
  covergroup cg;
    ca: coverpoint a { bins lo = {[B : B + 1]}; bins one = {B + 5}; }
    cb: coverpoint b { bins v[] = {[0:1]}; }
    x: cross ca, cb {
      // (lo, v[1]) through its first value tuple; (one, v[0]) through the second
      bins picked = '{ '{B + 1, 2'd1}, '{B + 5, 2'd0} };
      // (one, v[1]): its only value tuple is in the set
      bins whole = '{ '{B + 5, 2'd1} } matches $;
      // (lo, v[0]) holds two value tuples, only one is listed: not taken
      bins part = '{ '{B, 2'd0} } matches $;
    }
  endgroup
  cg c = new;
  task automatic hit(bit [71:0] x, bit [1:0] y);
    a = x; b = y; c.sample();
    $display("%.2f", c.x.get_coverage());
  endtask
  // bins: picked, whole, and the automatic (lo, v[0]) since part is empty
  initial begin
    hit(B, 1);       // picked
    hit(B + 5, 1);   // whole
    hit(B + 1, 0);   // (lo, v[0])
  end
endmodule
//> 33.33
//> 66.67
//> 100.00
