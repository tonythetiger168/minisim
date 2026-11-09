// IEEE 1800-2023 16.4.1, 4.4.2: An observed deferred assertion (#0) matures in
// the observed region and its action runs in the reactive region; a final
// one matures in the postponed region, after changes made by reactive code.
// The action of a5 sets b from the reactive region, so a7 reports both a
// fail and a pass and a6 only the pass (the example in 16.4.2).
//! icarus-skip: Icarus 13 does not support deferred assertions
module top;
  logic go = 0, b = 0;
  task setb(); b = 1; endtask
  a5: assert #0 (go == 0) else setb();
  a6: assert final (!(go && !b)) $display("%0t a6 pass", $time); else $display("%0t a6 fail", $time);
  a7: assert #0 (!(go && !b)) $display("%0t a7 pass", $time); else $display("%0t a7 fail", $time);
  initial begin
    #1 go = 1;
    #1 $finish;
  end
endmodule

//> 0 a7 pass
//> 0 a6 pass
//> 1 a7 fail
//> 1 a7 pass
//> 1 a6 pass
