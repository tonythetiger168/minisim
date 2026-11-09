// IEEE 1800-2023 18.14 / 18.15: Each object has its own random generator,
// so the values an object's randomize() gives do not depend on other
// objects' calls. srandom(seed) reseeds it: the same seed repeats the same
// values. get_randstate() / set_randstate() save and restore it.
//! icarus-skip: Icarus 13 does not support randomize()
module top;
  class C;
    rand bit [31:0] v;
  endclass
  C a, b;
  bit [31:0] first[3], again[3], other[3];
  string st;
  int same = 1;
  initial begin
    a = new; b = new;
    a.srandom(7);
    for (int i = 0; i < 3; i++) begin void'(a.randomize()); first[i] = a.v; end
    a.srandom(7);
    for (int i = 0; i < 3; i++) begin void'(b.randomize()); void'(a.randomize()); again[i] = a.v; end
    foreach (first[i]) if (first[i] != again[i]) same = 0;
    $display("%0d", same);
    st = a.get_randstate();
    for (int i = 0; i < 3; i++) begin void'(a.randomize()); other[i] = a.v; end
    a.set_randstate(st);
    for (int i = 0; i < 3; i++) begin void'(a.randomize()); if (a.v != other[i]) same = 0; end
    $display("%0d", same);
    b.srandom(8);
    void'(b.randomize());
    $display("%0d", b.v != first[0]);
  end
endmodule

//> 1
//> 1
//> 1
