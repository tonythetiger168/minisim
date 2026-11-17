// IEEE 1800-2023 18.4 and 11.4.10: a signed rand variable wider than 64
// bits keeps its sign in constraints: an arithmetic shift right fills with
// its sign bit, by a constant or a random amount, and signed compares see
// negative values.
//! icarus-skip: Icarus 13 has no constraints
class W;
  rand bit signed [79:0] s;
  rand bit [5:0] k;
  bit signed [79:0] want;
endclass
module top;
  W w = new;
  initial begin
    automatic int good = 1;
    w.want = 80'sd0 - 80'sd3;
    if (!w.randomize() with { s == -80'sd37; (s >>> 4) == want; }) good = 0;
    if (w.s != -80'sd37) good = 0;
    if (!w.randomize() with { k == 40; s == -80'sd37 <<< 40; (s >>> k) == -80'sd37; }) good = 0;
    if (w.k != 40 || (w.s >>> 40) != -80'sd37) good = 0;
    repeat (20) begin
      if (!w.randomize() with { s < 0; s > -80'sd1000; }) good = 0;
      if (!(w.s < 0 && w.s > -80'sd1000)) good = 0;
    end
    $display("good=%0d", good);
  end
endmodule
//> good=1
