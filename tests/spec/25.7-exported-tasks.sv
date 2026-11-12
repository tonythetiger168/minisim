// IEEE 1800-2023 25.7: tasks and functions in interfaces. An interface
// declares a task and a function extern; a module exports them through a
// modport (export) and defines them as b.name; another module imports
// them and calls them through the interface. Each interface instance
// calls the module connected to it.
//! icarus-skip: Icarus 13 has no modport task ports
interface port_if;
  logic [7:0] d;
  extern task put(input logic [7:0] v);
  extern function int peek(int k);
  modport tgt (input d, export put, export peek);
  modport ini (output d, import task put(input logic [7:0] v), import function int peek(int k));
endinterface
module sink #(parameter int ID = 0) (interface b);
  int count;
  task b.put(input logic [7:0] v);
    count++;
    $display("%0d: put %h with d=%h count=%0d", ID, v, b.d, count);
  endtask
  function int b.peek(int k);
    return ID * 100 + k + count;
  endfunction
endmodule
module source #(parameter int T = 1) (interface b);
  initial begin
    #T b.d = 8'h10 + T; b.put(8'h3c);
    #2 b.put(8'h3d);
    $display("peek %0d", b.peek(5));
  end
endmodule
module top;
  port_if x(), y();
  sink #(1) t1(x.tgt);
  sink #(2) t2(y.tgt);
  source #(1) s1(x.ini);
  source #(2) s2(y.ini);
endmodule
//> 1: put 3c with d=11 count=1
//> 2: put 3c with d=12 count=1
//> 1: put 3d with d=11 count=2
//> peek 107
//> 2: put 3d with d=12 count=2
//> peek 207
