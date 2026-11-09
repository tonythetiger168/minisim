// IEEE 1800-2023 25.3: An array of interface instances (25.3).
//! xfail: interface arrays are errors
//! icarus-skip: Icarus 13 does not support interfaces
interface ifc; logic a; endinterface
module top;
  ifc i[2]();
  initial begin
    i[0].a = 1; i[1].a = 0;
    #1 $display("%b %b", i[0].a, i[1].a);
  end
endmodule

//> 1 0
