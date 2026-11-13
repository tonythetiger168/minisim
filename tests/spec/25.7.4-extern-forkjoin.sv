// IEEE 1800-2023 25.7.4: an extern forkjoin task exported by several
// modules. A call through the interface starts every exporter's task at
// once and returns when the slowest one is done.
//! icarus-skip: Icarus 13 has no modport task ports
interface bus_if;
  extern forkjoin task probe(input int tag, output int hits);
  modport slave (export probe);
  modport master (import task probe(input int tag, output int hits));
endinterface
module responder #(parameter int ID = 0, parameter int LAT = 1) (interface b);
  int seen;
  task b.probe(input int tag, output int hits);
    #LAT seen += tag;
    hits = seen;
    $display("%0t: responder %0d done seen=%0d", $time, ID, seen);
  endtask
endmodule
module requester (interface b);
  int h;
  initial begin
    #1 b.probe(5, h);
    $display("%0t: first probe returned", $time);
    b.probe(7, h);
    $display("%0t: second probe returned", $time);
  end
endmodule
module top;
  bus_if bus();
  responder #(1, 3) r1(bus.slave);
  responder #(2, 1) r2(bus.slave);
  requester q(bus.master);
endmodule
//> 2: responder 2 done seen=5
//> 4: responder 1 done seen=5
//> 4: first probe returned
//> 5: responder 2 done seen=12
//> 7: responder 1 done seen=12
//> 7: second probe returned
