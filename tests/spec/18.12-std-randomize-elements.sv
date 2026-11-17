// IEEE 1800-2023 18.12 and 18.11: the variables std::randomize() and
// randomize() take may be elements and members: an element of a fixed-size
// array with an index known only at run time, an element of a dynamic
// array or of a container inside a class object, a member of a fixed-size
// array property. Only that element changes. Wider than 64 bits, the
// constraints on such variables (and on whole arrays of them) still hold.
//! icarus-skip: Icarus 13 has no constraints
class Holder;
  logic [3:0] fixed[5];
  logic [7:0] dyn[];
  logic [7:0] grid[][];
  function new();
    dyn = new[3];
    grid = new[2];
    foreach (grid[i]) grid[i] = new[2];
  endfunction
endclass

module top;
  int arr[4];
  bit [94:0] wide[3];
  bit [94:0] w1;
  initial begin
    automatic Holder h = new;
    automatic int ok = 1;
    foreach (arr[i]) arr[i] = 100 + i;
    repeat (20) begin
      for (int i = 0; i < 4; i++) begin
        if (!std::randomize(arr[i]) with { arr[i] inside {[10 * i : 10 * i + 5]}; }) ok = 0;
        if (!(arr[i] inside {[10 * i : 10 * i + 5]})) ok = 0;
      end
      h.fixed[0] = 4'hf;
      if (!std::randomize(h.fixed[3]) with { h.fixed[3] < 3; }) ok = 0;
      if (h.fixed[3] >= 3 || h.fixed[0] != 4'hf) ok = 0;
      h.dyn[0] = 8'h11;
      if (!std::randomize(h.dyn[2]) with { h.dyn[2] inside {[200:210]}; }) ok = 0;
      if (!(h.dyn[2] inside {[200:210]}) || h.dyn[0] != 8'h11) ok = 0;
      if (!std::randomize(h.grid[1][0]) with { h.grid[1][0] == 8'h5a; }) ok = 0;
      if (h.grid[1][0] != 8'h5a) ok = 0;
      if (!std::randomize(w1) with { w1 < 95'd1000; w1 > 95'd10; }) ok = 0;
      if (!(w1 < 95'd1000 && w1 > 95'd10)) ok = 0;
      if (!std::randomize(wide) with { foreach (wide[i]) wide[i] < 95'd1000000000000; }) ok = 0;
      foreach (wide[i]) if (wide[i] >= 95'd1000000000000) ok = 0;
    end
    // a failed randomize() leaves the element alone
    arr[1] = 7;
    if (std::randomize(arr[1]) with { arr[1] > 5; arr[1] < 3; }) ok = 0;
    $display("ok=%0d kept=%0d", ok, arr[1]);
  end
endmodule
//> ok=1 kept=7
