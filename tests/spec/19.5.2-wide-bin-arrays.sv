// IEEE 1800-2023 19.5.1 / 19.5.2: bin arrays of a coverpoint wider than 64
// bits: a wildcard name[] makes one bin per value its x/? bits allow, a
// transition name[] one bin per sequence of single values, and a wildcard
// transition matches each value its pattern allows.
//! icarus-skip: Icarus 13 does not support covergroups
module t;
  bit [71:0] v;
  localparam bit [71:0] B = 72'h10_0000_0000_0000_0000;   // 2^68
  covergroup cg;
    w: coverpoint v {
      wildcard bins pat[] = {72'h10_0000_0000_0000_00?1};   // 16 values
    }
    tr: coverpoint v {
      bins steps[] = (B => B + 1, B + 2);                  // 2 bins
      wildcard bins wt = (72'h10_0000_0000_0000_000? => B + 7);
    }
  endgroup
  cg c = new;
  task automatic hit(bit [71:0] x);
    v = x; c.sample();
    $display("%.2f %.2f", c.w.get_coverage(), c.tr.get_coverage());
  endtask
  initial begin
    hit(B + 'h01);   // pat[...01]
    hit(B + 'h21);   // pat[...21]
    hit(B);
    hit(B + 2);      // steps[B=>B+2]
    hit(B + 5);
    hit(B + 7);      // wt
    hit(B);
    hit(B + 1);      // steps[B=>B+1]
    hit(B + 'h21);   // already hit
  end
endmodule
//> 6.25 0.00
//> 12.50 0.00
//> 12.50 0.00
//> 12.50 33.33
//> 12.50 33.33
//> 12.50 66.67
//> 12.50 66.67
//> 12.50 100.00
//> 12.50 100.00
