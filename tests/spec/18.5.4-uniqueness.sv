// IEEE 1800-2023 18.5.4: unique {...}: no two of the listed variables and
// array elements are equal.
//! icarus-skip: Icarus 13 does not support unique constraints
module top;
  class U;
    rand bit [2:0] arr[6];
    rand bit [2:0] x, y;
    constraint c { unique {arr, x, y}; }
  endclass
  U u;
  int bad, ok = 1;
  initial begin
    u = new;
    repeat (100) begin
      bit [2:0] all[8];
      ok &= u.randomize();
      foreach (u.arr[i]) all[i] = u.arr[i];
      all[6] = u.x; all[7] = u.y;
      for (int i = 0; i < 8; i++)
        for (int j = i + 1; j < 8; j++)
          if (all[i] == all[j]) bad++;
    end
    $display("%0d %0d", ok, bad);
  end
endmodule

//> 1 0
