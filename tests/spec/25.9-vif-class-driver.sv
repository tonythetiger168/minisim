// IEEE 1800-2023 25.9: class drivers on two instances of one interface
// through virtual interfaces: a modport virtual interface (25.9.1),
// waits on edges and events through the handle, nonblocking writes, a
// virtual interface array, and null comparisons.
//! icarus-skip: Icarus 13 does not support virtual interfaces
interface bus_if(input logic clk);
  logic valid;
  logic [7:0] data;
  event done;
  modport mst(output valid, output data, input clk);
endinterface

class Driver;
  virtual bus_if vif;
  virtual bus_if.mst mp;
  function new(virtual bus_if v); vif = v; mp = v; endfunction
  task run(int n);
    repeat (n) begin
      @(posedge vif.clk);
      mp.valid <= 1;
      mp.data <= mp.data + 8'h11;
    end
    @(posedge vif.clk) vif.valid <= 0;
    -> vif.done;
  endtask
endclass

module top;
  logic clk = 0;
  always #5 clk = ~clk;
  bus_if b0(clk), b1(clk);
  virtual bus_if vs[2];
  virtual bus_if none;
  Driver d0, d1;
  initial begin
    b0.data = 0;
    b1.data = 8'h80;
    d0 = new(b0);
    d1 = new(b1);
    vs[0] = b0;
    vs[1] = b1;
    $display("%0d %0d %0d", none == null, vs[1] != null, vs[0] == d0.vif);
    fork d0.run(2); d1.run(3); join_none
    @(b0.done) $display("b0 done at %0t: %h", $time, b0.data);
    @(vs[1].done) $display("b1 done at %0t: %h", $time, vs[1].data);
    #1 $display("%b %b", b0.valid, b1.valid);
    $finish;
  end
endmodule

//> 1 1 1
//> b0 done at 25: 22
//> b1 done at 35: b3
//> 0 0
