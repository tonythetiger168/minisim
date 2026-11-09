// IEEE 1800-2023 18.5: Constraint blocks: relational constraints, inside
// sets and ranges, constraints between variables, and external (extern)
// constraint blocks declared in the class and defined outside it (18.5.1).
//! icarus-skip: Icarus 13 does not support constraints
module top;
  class Bus;
    rand bit [15:0] addr;
    rand bit [31:0] data;
    rand bit [3:0] len;
    constraint word_align { addr[1:0] == 2'b0; }
    constraint ranges { addr inside {[16'h1000:16'h1fff], 16'h8000}; len > 0; len <= 8; }
    constraint rel { data < addr * 2; data > len; }
    extern constraint ext;
  endclass
  constraint Bus::ext { len != 5; }
  Bus b;
  int bad, hiAddr, ok = 1;
  initial begin
    b = new;
    repeat (300) begin
      ok &= b.randomize();
      if (b.addr[1:0] != 0 || !(b.addr inside {[16'h1000:16'h1fff], 16'h8000}) || b.len == 0 || b.len > 8 ||
          b.len == 5 || !(b.data < b.addr * 2) || !(b.data > b.len)) bad++;
      if (b.addr == 16'h8000) hiAddr++;
    end
    $display("%0d %0d %0d", ok, bad, hiAddr < 50);
  end
endmodule

//> 1 0 1
