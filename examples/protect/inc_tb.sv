// Includes a protected header (encrypted copy found through +incdir+).
module inc_tb;
  `include "secret_params.svh"
  initial $display("k=%0d", SECRET_K + 1);
endmodule
