// IEEE 1800-2023 20.11: $asserton, $assertoff, $assertkill, the action
// controls and $assertcontrol with assertion types, directive types, levels,
// scopes and locks. An assert with no else calls $error (16.3).
//! icarus-skip: Icarus 13 does not support assertion control
module top;
  int n = 0;
  task automatic chk(int k);
    c1: assert (n == k) $display("pass %0d", k); else $display("fail %0d", k);
  endtask
  initial begin
    chk(0); chk(1);
    $assertoff;                  chk(1);
    $asserton;                   chk(1);
    $assertpassoff;              chk(0);
    $assertfailoff;              chk(1);
    $assertpasson; $assertfailon; chk(0);
    $assertcontrol(4, 2);        chk(1);
    $assertcontrol(3);           chk(1);
    $assertcontrol(4, 1);        chk(1);
    $assertcontrol(4, 2, 2);     chk(1);
    $assertcontrol(1, 255, 7, 0, top.chk.c1); $assertoff; chk(1);
    $assertcontrol(2); $assertoff(0, top.chk.c1); chk(1);
    $asserton(0, top); chk(0);
    $assertkill(1, top.chk); chk(0);
    $asserton(1, top.chk); chk(0);
    a9: assert (0);
  end
endmodule
//> pass 0
//> fail 1
//> fail 1
//> pass 0
//> fail 1
//> fail 1
//> fail 1
//> fail 1
//> pass 0
//> pass 0
//> Error: "20.11-assertion-control.sv", 26: top.a9: at time 0
