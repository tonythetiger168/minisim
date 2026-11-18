// 15.5, 7.5, 7.8, 7.10, 6.17: named events as elements of associative
// arrays (integral and string index), dynamic arrays and queues, and of
// containers whose elements are fixed-size event arrays or containers.
// An element names its own event; push_back, insert and assignment make an
// element name an existing event.
//! icarus-skip: Icarus does not support containers of events
module top;
  event byId[int];
  event byName[string];
  event dyn[];
  event q[$];
  event pairs[][2];
  event nest[][int];
  event solo;
  int hits;

  initial begin
    dyn = new[3];
    pairs = new[2];
    nest = new[2];
    q.push_back(solo);
    q.insert(0, solo);
    fork
      begin @(byId[5]); $display("%0t byId[5]", $time); end
      begin @(byName["run"]); $display("%0t byName[run]", $time); end
      begin @(dyn[1]); $display("%0t dyn[1]", $time); end
      begin @(solo); $display("%0t solo", $time); end
      begin @(pairs[1][0]); $display("%0t pairs[1][0]", $time); end
      begin @(pairs[1][1]); $display("%0t pairs[1][1]", $time); end
      begin @(nest[1][7]); $display("%0t nest[1][7]", $time); end
      begin wait (byId[9].triggered); $display("%0t byId[9] triggered", $time); end
    join_none
    #1 ->byId[5];
    #1 ->byName["run"];
    #1 ->dyn[1];
    ->dyn[2];   // nobody waits on this one
    #1 ->q[1];
    #1 ->pairs[1][1];
    #1 ->pairs[1][0];
    #1 ->nest[1][7];
    #1 ->byId[9];
    #1 byId[3] = solo;
    fork begin @(solo); $display("%0t solo via byId[3]", $time); end join_none
    #1 ->byId[3];
    #1 $display("done %0d", byId.exists(3));
  end
endmodule
//> 1 byId[5]
//> 2 byName[run]
//> 3 dyn[1]
//> 4 solo
//> 5 pairs[1][1]
//> 6 pairs[1][0]
//> 7 nest[1][7]
//> 8 byId[9] triggered
//> 10 solo via byId[3]
//> 11 done 1
