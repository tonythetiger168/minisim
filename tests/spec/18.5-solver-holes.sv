// IEEE 1800-2023 18.5: randomize() succeeds whenever the constraints can be
// met. A variable solved first (solve-before, 18.5.9, or an array size, 18.4)
// that is tied to one whose `inside` set has holes must take a value the
// other can match: s == d with d inside {512..8192} leaves s only 5 values.
//! icarus-skip: Icarus 13 does not support constraints
module top;
  class c1;
    rand bit [15:0] d; rand bit [16:0] s;
    constraint k1 { d inside {512, 1024, 2048, 4096, 8192}; }
    constraint k2 { s == d; solve s before d; }
  endclass
  class c2;
    rand bit [15:0] len; rand bit [7:0] data[];
    constraint k1 { len inside {512, 1024, 2048, 4096, 8192}; }
    constraint k2 { data.size() == len; }
  endclass
  initial begin
    static c1 a = new; static c2 b = new;
    static int fa = 0, fb = 0, bad = 0;
    bit seen[int];
    repeat (100) begin
      if (!a.randomize()) fa++;
      else if (a.s != a.d) bad++;
    end
    repeat (40) begin
      if (!b.randomize()) fb++;
      else begin
        if (b.data.size() != b.len) bad++;
        seen[b.len] = 1;
      end
    end
    $display("%0d %0d %0d %0d", fa, fb, bad, seen.num() >= 4);
  end
endmodule

//> 0 0 0 1
