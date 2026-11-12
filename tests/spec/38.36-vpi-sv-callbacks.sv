// IEEE 1800-2023 38.36.1: cbCreateObj fires as a class object is made (obj
// NULL: any class; a class definition: objects of that class), with the
// new object as obj; cbSizeChange fires when a queue or associative array
// changes size (push, pop, delete, a new key; not a write to an existing
// element). cbStartOfSave registers but never fires (msim has no save);
// cbSizeChange on a class variable is an error.
//! icarus-skip: Icarus 13 has no SystemVerilog classes through VPI
//! args: data/vpi/svcb.c
module top;
  class Base; int a; endclass
  class Pkt extends Base; int b; endclass
  Base h;
  Pkt p;
  int q[$];
  int aa[string];
  initial begin
    #1 h = new;
    #1 p = new;
    #1 q.push_back(1);
    q.push_back(2);
    q[0] = 5;
    #1 void'(q.pop_front());
    #1 aa["x"] = 1;
    aa["x"] = 2;
    aa["y"] = 3;
    #1 aa.delete("x");
    #1 q.delete();
  end
endmodule
//> bad: handle 0 error 1
//> any: created vpiClassObj (Base) at 1
//> any: created vpiClassObj (Pkt) at 2
//> Pkt: created vpiClassObj (Pkt) at 2
//> q: size of q now 1 at 3
//> q: size of q now 2 at 3
//> q: size of q now 1 at 4
//> aa: size of aa now 1 at 5
//> aa: size of aa now 2 at 5
//> aa: size of aa now 1 at 6
//> q: size of q now 0 at 7
