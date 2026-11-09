// IEEE 1800-2023 18.5.10: A static constraint block is shared by every
// object of the class: constraint_mode() through one object turns it off
// for all of them.
//! icarus-skip: Icarus 13 does not support constraints
module top;
  class S;
    rand bit [7:0] v;
    static constraint lim { v < 4; }
  endclass
  S a, b;
  int big;
  initial begin
    a = new; b = new;
    $display("%0d", b.lim.constraint_mode());
    a.lim.constraint_mode(0);
    $display("%0d", b.lim.constraint_mode());
    repeat (40) begin void'(b.randomize()); if (b.v >= 4) big++; end
    $display("%0d", big > 30);
    b.lim.constraint_mode(1);
    big = 0;
    repeat (40) begin void'(a.randomize()); if (a.v >= 4) big++; end
    $display("%0d", big);
  end
endmodule

//> 1
//> 0
//> 1
//> 0
