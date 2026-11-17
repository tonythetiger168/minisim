// IEEE 1800-2023 18.4.1 and 18.3: the solver randomizes variables of real
// type too. A constrained rand real is uniform over its range (half of the
// values of (0.0, 2.0) fall below 1.0), constraints may combine reals with
// arithmetic, a real range in a dist takes its weight with :/ (18.5.3),
// and a rand shortreal holds single-precision values. An unconstrained
// rand real takes any finite value, negative ones included.
//! icarus-skip: Icarus 13 has no constraints
class R;
  rand real v;
  rand real a, b;
  rand real d;
  rand shortreal s;
  rand real free;
  rand int n;
  constraint cv { v > 0.0 && v < 2.0; }
  constraint cab { a >= -1.5; a <= 1.5; b == a * 2.0 + 0.25; }
  constraint cd { d dist { [0.0:1.0] :/ 1, [10.0:20.0] :/ 3 }; }
  constraint cs { s > 100.0; s < 100.5; }
  constraint cn { n inside {[1:3]}; real'(n) < v * 4.0; }
endclass

module top;
  initial begin
    R r = new;
    automatic int ok = 1, low = 0, hi = 0, dlow = 0, dhigh = 0, neg = 0, sdiff = 0;
    automatic shortreal prev = 0.0;
    repeat (400) begin
      if (!r.randomize()) ok = 0;
      if (!(r.v > 0.0 && r.v < 2.0)) ok = 0;
      if (r.v < 1.0) low++; else hi++;
      if (r.a < -1.5 || r.a > 1.5 || r.b != r.a * 2.0 + 0.25) ok = 0;
      if (r.d >= 0.0 && r.d <= 1.0) dlow++;
      else if (r.d >= 10.0 && r.d <= 20.0) dhigh++;
      else ok = 0;
      if (!(r.s > 100.0 && r.s < 100.5)) ok = 0;
      if ($bitstoshortreal($shortrealtobits(r.s)) != r.s) ok = 0;
      if (r.s != prev) sdiff++;
      prev = r.s;
      if (!(r.n inside {[1:3]}) || real'(r.n) >= r.v * 4.0) ok = 0;
      if (r.free < 0.0) neg++;
      if (r.free != r.free) ok = 0;   // never a NaN
    end
    // (0, 2): about half below 1.0; dist 1:3; about half of free negative
    $display("ok=%0d uniform=%0d dist=%0d neg=%0d shortreal-varies=%0d",
             ok, low > 140 && hi > 140, dhigh > 2 * dlow, neg > 120 && neg < 280, sdiff > 300);
    // randomize(v) with an inline constraint
    if (!r.randomize(v) with { v > 1.75; }) ok = 0;
    $display("with=%0d", ok && r.v > 1.75 && r.v < 2.0);
  end
endmodule
//> ok=1 uniform=1 dist=1 neg=1 shortreal-varies=1
//> with=1
