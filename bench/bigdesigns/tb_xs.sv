// Clock/reset driver for XiangShan SimTop (what difftest's emu.cpp does), plus UART output.
module tb_xs;
  reg clock = 0, reset = 1;
  wire [63:0] exit_, step; wire uv; wire [7:0] uch; wire uiv;
  SimTop dut (.clock(clock), .reset(reset), .difftest_exit(exit_), .difftest_step(step),
    .difftest_perfCtrl_clean(1'b0), .difftest_perfCtrl_dump(1'b0),
    .difftest_logCtrl_begin(64'd0), .difftest_logCtrl_end(64'd0), .difftest_logCtrl_level(64'd0),
    .difftest_uart_out_valid(uv), .difftest_uart_out_ch(uch), .difftest_uart_in_valid(uiv), .difftest_uart_in_ch(8'hff));
  always #5 clock = ~clock;
  longint cycles = 0; longint maxc; int rstc;
  initial begin
    if (!$value$plusargs("max_cycles=%d", maxc)) maxc = 100000;
    if (!$value$plusargs("reset_cycles=%d", rstc)) rstc = 10;
    repeat (rstc) @(posedge clock); @(negedge clock) reset = 0;
  end
  always @(posedge clock) begin
    cycles <= cycles + 1;
    if (uv) begin $write("%c", uch); $fflush(); end
    if (cycles % 5000 == 0) begin $display("[tb_xs] cycle %0d", cycles); $fflush(); end
    if (cycles >= maxc) begin $display("[tb_xs] max cycles %0d reached", maxc); $finish; end
  end
`ifdef XS_MON
  // trace the flash AXI port each cycle it is active (for simulator-vs-simulator diffs)
  always @(posedge clock) if (cycles < 4000) begin
    if (dut.cpu.l_simMMIO.flash.auto_in_ar_valid || dut.cpu.l_simMMIO.flash.auto_in_r_valid)
      $display("[mon] %0d ar v%b r%b a%h len%0d id%0d | r v%b r%b d%h last%b id%0d",
        cycles, dut.cpu.l_simMMIO.flash.auto_in_ar_valid, dut.cpu.l_simMMIO.flash.auto_in_ar_ready,
        dut.cpu.l_simMMIO.flash.auto_in_ar_bits_addr, dut.cpu.l_simMMIO.flash.auto_in_ar_bits_len, dut.cpu.l_simMMIO.flash.auto_in_ar_bits_id,
        dut.cpu.l_simMMIO.flash.auto_in_r_valid, dut.cpu.l_simMMIO.flash.auto_in_r_ready,
        dut.cpu.l_simMMIO.flash.auto_in_r_bits_data, dut.cpu.l_simMMIO.flash.auto_in_r_bits_last,
        dut.cpu.l_simMMIO.flash.auto_in_r_bits_id);
  end
`endif
`ifdef XS_VCD
  // full-design VCD over a cycle window, to diff two simulators signal by signal
  longint vcd_from, vcd_to;
  initial begin
    if (!$value$plusargs("vcd_from=%d", vcd_from)) vcd_from = 160;
    if (!$value$plusargs("vcd_to=%d", vcd_to)) vcd_to = 240;
    $dumpfile("xs.vcd"); $dumpvars(0, dut); $dumpoff;
  end
  always @(posedge clock) begin
    if (cycles == vcd_from) $dumpon;
    if (cycles == vcd_to) begin $dumpoff; $dumpflush; end
  end
`endif
endmodule
