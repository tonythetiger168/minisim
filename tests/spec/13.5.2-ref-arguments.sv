// IEEE 1800-2023 13.5.2: ref and const ref arguments see the caller's
// variable (13.5.2).
//! icarus-skip: Icarus 13 does not support const ref
module top;
  int v = 1;
  task automatic inc(ref int x);
    x++;
  endtask
  task automatic watch(const ref int x);
    #1 $display("sees %0d", x);
  endtask
  initial begin
    inc(v);
    $display("%0d", v);
    fork
      watch(v);
      v = 7;
    join
  end
endmodule

//> 2
//> sees 7
