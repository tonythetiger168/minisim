// IEEE 1800-2023 19.5: the number of bins in name[N] shall be a positive
// integral expression. One that is 0 when the covergroup is constructed is
// a run-time error that ends the simulation (not a crash).
//! icarus-skip: Icarus has no covergroups
//! exit: 1
module t;
  bit [7:0] v;
  int count = 0;
  covergroup cg;
    coverpoint v { bins b[count] = {[0:3]}; }
  endgroup
  cg c;
  initial begin
    $display("before");
    c = new;
    $display("after");
  end
endmodule
//> before
