// IEEE 1800-2023 18.5.9: solve s before d picks s first, so with
// "s -> d == 0" s is 1 about half the time instead of almost never.
//! icarus-skip: Icarus 13 does not support constraints
module top;
  class B;
    rand bit s;
    rand bit [31:0] d;
    constraint c { s -> d == 0; }
    constraint order { solve s before d; }
  endclass
  B b;
  int ones, bad;
  initial begin
    b = new;
    repeat (1000) begin
      void'(b.randomize());
      if (b.s) begin ones++; if (b.d != 0) bad++; end
    end
    $display("%0d %0d", bad, ones > 400 && ones < 600);
  end
endmodule

//> 0 1
