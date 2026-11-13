// IEEE 1800-2023 25.9: virtual interfaces that point at the elements of an
// interface instance array, held in an array of virtual interfaces, and an
// unpacked array member written and read through a virtual interface.
//! icarus-skip: Icarus 13 has no virtual interfaces
interface bus_if(input logic clk);
  logic [7:0] data [4];
  logic [3:0] be;
endinterface
class drv;
  virtual bus_if vif;
  function new(virtual bus_if v); vif = v; endfunction
  task run(int k);
    vif.be = k;
    vif.data[k] = 8'h10 + k;
    foreach (vif.data[i]) if (i != k) vif.data[i] = 0;
  endtask
endclass
module top;
  logic clk = 0;
  bus_if arr[2](clk);
  virtual bus_if vs[2];
  initial begin
    drv d0, d1;
    vs[0] = arr[0]; vs[1] = arr[1];
    d0 = new(vs[0]); d1 = new(arr[1]);
    d0.run(1); d1.run(2);
    $display("%h %h %h", arr[0].be, arr[0].data[1], arr[0].data[2]);
    $display("%h %h %h", arr[1].be, arr[1].data[1], arr[1].data[2]);
    $display("%h", vs[1].data[2]);
  end
endmodule

//> 1 11 00
//> 2 00 12
//> 12
