// IEEE 1800-2023 15.5 and 8.9: a static event property of a class is one
// event shared by every object. Like a module's named event it starts not
// triggered, and -> or ->> on it wakes the processes waiting with @, in a
// module and in a static method alike.
//! icarus-skip: Icarus 13 has no class events
class Cls;
  static int dummy;
  static event se;
  static task waiter(); @(se) $display("method woke at %0t", $time); endtask
  static function void fire(); -> se; endfunction
endclass
module top;
  initial Cls::waiter();
  always @(Cls::se) $display("always woke at %0t", $time);
  initial begin
    $display("triggered at 0: %0d", Cls::se.triggered);
    #1 -> Cls::se;
    #1 ->> Cls::se;
    #1 Cls::fire();
    #1 $display("done");
  end
endmodule
//> triggered at 0: 0
//> method woke at 1
//> always woke at 1
//> always woke at 2
//> always woke at 3
//> done
