// IEEE 1800-2023 25.3: An array of interface instances (25.3).
//! icarus-skip: Icarus 13 does not support interfaces
// An element as an interface port, and an instance array whose interface
// port takes one element of an interface array each, left-most to left-most.
interface ifc; logic a; endinterface
interface bus_if; logic [3:0] d; endinterface
module drv(bus_if b, input logic [3:0] v); assign b.d = v; endmodule
module top;
  ifc i[2]();
  bus_if bi[2]();
  drv w[1:0] (.b(bi), .v(8'h57));       // w[1] drives bi[0] with 5
  bus_if bj[0:1]();
  for (genvar g = 0; g < 2; g++) begin : gen
    drv x(.b(bj[g]), .v(4'(g + 10)));
  end
  initial begin
    i[0].a = 1; i[1].a = 0;
    #1 $display("%b %b", i[0].a, i[1].a);
    $display("%0d %0d %0d %0d", bi[0].d, bi[1].d, bj[0].d, bj[1].d);
  end
endmodule

//> 1 0
//> 5 7 10 11
