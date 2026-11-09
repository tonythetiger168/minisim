// IEEE 1800-2023 18.5.2: A subclass inherits its base class's constraints;
// a constraint block with the same name replaces the base block. The
// object's own class decides which constraints apply, even through a
// base-class handle.
//! icarus-skip: Icarus 13 does not support constraints
module top;
  class A;
    rand int x;
    constraint lim { x >= 0; x < 10; }
    constraint odd { x % 2 == 1; }
  endclass
  class B extends A;
    constraint lim { x >= 100; x < 110; }
  endclass
  A h;
  int bad;
  initial begin
    h = B::new;
    repeat (50) begin
      void'(h.randomize());
      if (h.x < 100 || h.x >= 110 || h.x % 2 != 1) bad++;
    end
    h = new;
    repeat (50) begin
      void'(h.randomize());
      if (h.x < 0 || h.x >= 10 || h.x % 2 != 1) bad++;
    end
    $display("%0d", bad);
  end
endmodule

//> 0
