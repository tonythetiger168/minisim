// Per-step +define+ (tests/msim_mstep.cmake): def_a.sv is analysed with
// +define+ONLY_A, def_b.sv without; each sees only its own.
module ma;
  initial begin
`ifdef ONLY_A
    $display("ma sees ONLY_A=%0d", `ONLY_A);
`else
    $display("ma has no ONLY_A");
`endif
  end
endmodule
