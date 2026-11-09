// IEEE 1800-2023 15.5.4: wait_order passes when events trigger in the
// listed order (15.5.4).
//! xfail: wait_order is an error
//! icarus-skip: Icarus 13 does not support wait_order
module top;
  event a, b;
  initial begin
    fork
      wait_order(a, b) $display("in order"); else $display("out of order");
      begin #1 ->a; #1 ->b; end
    join
  end
endmodule

//> in order
