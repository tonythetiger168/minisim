// xsoc against the msim -mvp models, two ways that must print the same line:
//   +define+GOLD  simulated, with mvp_axil_slave and mvp_irq_slave
//   otherwise     emulated (xsoc_emu, written by memu), with the transactor
// UNIT_PS comes from the command line: the unit of emu_time in memu's report.
`timescale 1ps/1ps
module xsoc_tb;
`ifndef UNIT_PS
`define UNIT_PS 1
`endif
  logic rst_n = 1'b0;
  logic [15:0] awaddr, araddr;
  logic [31:0] wdata, rdata, sum;
  logic [3:0]  wstrb;
  logic [1:0]  bresp, rresp;
  logic awvalid, awready, wvalid, wready, bvalid, bready, arvalid, arready, rvalid, rready;
  logic irq, done;
  logic [15:0] waited;
  int cycles = 0;

`ifdef GOLD
  logic clk = 1'b0;
  always #5000 clk = ~clk;
  wire edge_now = 1'b1;
  xsoc dut (.*);
  mvp_axil_slave #(.ADDR_W(16)) slave (.*);
  mvp_irq_slave #(.N(1)) irqs (.clk, .rst_n, .irq(irq));
`else
  logic clk = 1'b0;  // emu_clk
  always #1000 clk = ~clk;
  logic emu_rst = 1'b1;
  wire emu_ready, emu_run, edge_now;
  wire [0:0] emu_next_pos;
  wire [63:0] emu_next_time;
  wire req_valid, req_write, rsp_valid, rsp_err;
  wire [15:0] req_addr;
  wire [31:0] req_wdata, rsp_rdata;
  wire [3:0]  req_strb;
  initial begin repeat (20) @(posedge clk); emu_rst <= 1'b0; end
  xsoc_emu dut (.emu_clk(clk), .emu_rst, .emu_run, .emu_ready, .emu_pos(), .emu_neg(), .emu_time(),
                .emu_next_pos, .emu_next_neg(), .emu_next_time, .rst_n,
                .awaddr, .awvalid, .awready, .wdata, .wstrb, .wvalid, .wready, .bresp, .bvalid, .bready,
                .araddr, .arvalid, .arready, .rdata, .rresp, .rvalid, .rready, .irq, .done, .sum, .waited);
  memu_axil_slave_xtor #(.ADDR_W(16)) xtor (
      .emu_clk(clk), .emu_rst, .emu_ready, .emu_next_pos(emu_next_pos[0]), .emu_run_i(1'b1), .emu_run,
      .emu_edge(edge_now), .rst_n, .*);
  memu_mvp_host #(.ADDR_W(16), .UNIT_PS(`UNIT_PS)) host (
      .emu_clk(clk), .emu_next_time, .emu_edge(edge_now), .rst_n, .irq(irq), .*);
`endif

  // The reset goes away at the third edge of the design's clock, both ways.
  always @(posedge clk) if (edge_now) begin
    cycles++;
    if (cycles == 3) rst_n <= 1'b1;
    if (done) begin
      $display("xsoc: done after %0d cycles, sum %h, waited %0d for the interrupt", cycles, sum, waited);
      $finish;
    end
    if (cycles > 100000) begin
      $display("xsoc: FAIL, never done");
      $finish;
    end
  end
endmodule
