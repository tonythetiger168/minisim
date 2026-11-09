// IEEE 1800-2023 16.3: Immediate assert, assume and cover with pass and
// fail actions (16.3).
//! xfail: immediate assertions are errors
//! icarus-skip: Icarus 13 ignores cover statements
module top;
  int a = 1;
  initial begin
    assert (a == 1) $display("pass 1");
    assert (a == 2) else $display("fail 2");
    assert (a == 1) $display("pass 3"); else $display("fail 3");
    cover (a == 1) $display("covered");
  end
endmodule

//> pass 1
//> fail 2
//> pass 3
//> covered
