// IEEE 1800-2023 15.5.4: wait_order passes when events trigger in the
// listed order. An earlier event may trigger again; a later event triggering
// before its turn fails.
//! icarus-skip: Icarus 13 does not support wait_order
module t;
  event a, b, c;
  initial begin
    fork
      wait_order(a, b, c) $display("in order %0t", $time); else $display("out of order %0t", $time);
      begin #1 ->a; #1 ->a; #1 ->b; #1 ->c; end
    join
    fork
      wait_order(a, b, c) $display("in order %0t", $time); else $display("out of order %0t", $time);
      begin #1 ->a; #1 ->c; #1 ->b; end
    join
  end
endmodule
//> in order 4
//> out of order 6
