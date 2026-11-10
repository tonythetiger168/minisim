// IEEE 1800-2023 24.3: A program block runs its initial blocks in the
// reactive region and ends the run when they finish (24.3, 24.7).
//! icarus-skip: Icarus 13 has no reactive region, so the program reads q before the module updates it
program tb(input logic [3:0] q);
  initial begin
    #1 $display("program sees %0d", q);
  end
endprogram
module top;
  logic [3:0] q = 0;
  always #1 q = q + 1;
  tb t(q);
endmodule

//> program sees 1
