// IEEE 1800-2023 9.4.2: an event control on a class member or virtual
// interface member (@(h.e), @(v.x)) in a process that starts while the
// handle is still null waits for the handle to be set, then for the event
// or change it names (msim; the standard leaves the null case open).
//! icarus-skip: Icarus has no events as class members
interface sig_if;
  logic x = 0;
endinterface
class Box;
  event e;
endclass
module t;
  sig_if u();
  Box b;
  virtual sig_if v;
  int ne = 0, nx = 0;
  always @(b.e) ne++;
  always @(v.x) nx++;
  initial begin
    #1 b = new;
    v = u;
    #1 ->b.e;
    u.x = 1;
    #1 ->b.e;
    u.x = 0;
    #1 $display("%0d %0d", ne, nx);
  end
endmodule
//> 2 2
