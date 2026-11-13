// IEEE 1800-2023 19.6.1.3: cross bins from a set expression, a queue of
// value tuples (CrossQueueType). A tuple of bins is selected when at least
// `matches` of the set's value tuples fall in it (1 by default). The set may
// be written as an assignment pattern or come from a constant function.
//! icarus-skip: Icarus 13 has no covergroups
module top;
  bit [3:0] a, b;
  covergroup cg;
    ca: coverpoint a { bins lo = {[0:3]}; bins hi = {[4:7]}; }
    cb: coverpoint b { bins lo = {[0:3]}; bins hi = {[4:7]}; }
    x: cross ca, cb {
      function CrossQueueType spread();
        spread.push_back('{1, 5});
        spread.push_back('{2, 6});
        spread.push_back('{6, 1});
      endfunction
      bins same = '{ '{1, 1}, '{5, 6} };   // lo x lo and hi x hi
      bins up = spread() matches 2;         // lo x hi only (hi x lo has one)
    }
  endgroup
  cg c = new;
  initial begin
    a = 1; b = 1; c.sample();
    $display("%0.2f", c.x.get_inst_coverage());
    a = 6; b = 2; c.sample();   // hi x lo: an automatic bin
    $display("%0.2f", c.x.get_inst_coverage());
    a = 0; b = 7; c.sample();
    $display("%0.2f", c.x.get_inst_coverage());
  end
endmodule
//> 33.33
//> 66.67
//> 100.00
