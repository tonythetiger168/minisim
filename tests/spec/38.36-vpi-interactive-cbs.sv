// IEEE 1800-2023 38.36.3: in interactive mode (msim -do, as UCLI) a
// cbEnterInteractive callback runs when msim starts reading commands, at
// time 0 and again when a run stops; cbExitInteractive when a run goes on
// and when the commands end; cbInteractiveScopeChange when the scope
// command changes the scope, with obj the new scope.
//! icarus-skip: Icarus 13 runs no msim command file
//! args: data/vpi/interact.c -do data/vpi/interact.do
module sub;
endmodule
module top;
  sub u();
  initial begin
    #5 $display("%0t tick", $time);
    #20 $display("%0t done", $time);
  end
endmodule
//> 0: enter interactive
//> 0: scope change to top.u
//> top.u
//> 0: exit interactive
//> 5 tick
//> time 10: ran to time 10 (next event at 25)
//> 10: enter interactive
//> 10: scope change to top
//> top
//> 10: exit interactive
//> 25 done
//> simulation ended at time 25
