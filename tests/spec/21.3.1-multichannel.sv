// IEEE 1800-2023 21.3.1: $fdisplay to stdout through the 32-bit descriptor
// and to a multichannel descriptor (21.3.1).
module top;
  initial begin
    $fdisplay(32'h8000_0001, "to stdout");
    $fwrite(1, "mcd %0d\n", 1);
  end
endmodule

//> to stdout
//> mcd 1
