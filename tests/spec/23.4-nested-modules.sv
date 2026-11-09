// IEEE 1800-2023 23.4: A module declared inside another module (23.4).
//! icarus-skip: Icarus 13 does not support nested modules
module top;
  module inner;
    initial $display("inner %m");
  endmodule
  inner i();
endmodule

//> inner top.i
