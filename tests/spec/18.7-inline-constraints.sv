// IEEE 1800-2023 18.7: randomize() with {...} adds constraints for one call.
// Names resolve in the object first, then in the calling scope; local::
// names the calling scope explicitly; the caller's own class properties
// can be used too.
//! icarus-skip: Icarus 13 does not support randomize() with
module top;
  class Txn;
    rand bit [15:0] addr;
    rand bit [3:0] len;
    int lim = 3;
    constraint c { len > 0; }
  endclass
  class Seq;
    bit [15:0] base = 16'h4000;
    int lim = 9;
    Txn t;
    task run();
      t = new;
      void'(t.randomize() with { addr == base + 16'h10; len <= lim; len > local::lim - 8; });
      $display("%h %0d", t.addr, t.len >= 2 && t.len <= 3);
      void'(t.randomize() with { len == local::lim; });
      $display("%0d", t.len);
    endtask
  endclass
  Txn x;
  Seq s;
  int lim = 2;
  initial begin
    x = new;
    void'(x.randomize() with { len < lim; });   // the object's lim (3)
    $display("%0d", x.len < 3);
    void'(x.randomize() with { len == local::lim; });   // the module's lim (2)
    $display("%0d", x.len);
    s = new;
    s.run();
  end
endmodule

//> 1
//> 2
//> 4010 1
//> 9
