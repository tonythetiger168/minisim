// IEEE 1800-2023 16.4.1, 16.4.2: A deferred assertion's report is dropped when
// its process runs again in the same time step (a flush point), and its
// action's arguments keep the values they had when the assertion ran.
//! icarus-skip: Icarus 13 does not support deferred assertions
module top;
  logic a = 0;
  logic not_a;
  logic [3:0] v = 0, w = 0;
  int covered = 0;
  assign not_a = !a;
  always_comb begin : b1
    a2: assert #0 (not_a != a) else $display("%0t a2 fail", $time);
  end
  always @(v) begin : b2
    a3: assert #0 (v != 5) else $display("%0t a3 v=%0d", $time, v);
    a4: assert #0 (w == 0) else $display("%0t a4 w=%0d", $time, w);
  end
  initial begin
    #1 a = 1;
    #1 a = 0;
    #1 v = 5; v = 6;        // b2 runs once, with v = 6: no report
    #1 v = 5;               // reported
    #1 v = 7; #0 v = 5; #0 v = 8;   // each rerun flushes the last: no report
    #1 w = 3; v = 1; #0 w = 4;      // a4 fails with w = 3; the report keeps 3
    #1 $finish;
  end
endmodule

//> 4 a3 v=5
//> 6 a4 w=3
