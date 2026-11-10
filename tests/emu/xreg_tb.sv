// xreg driven from the host through memu_axil_master_xtor, two ways that must
// print the same lines:
//   +define+GOLD  the RTL, simulated, the transactor an ordinary bus master
//   otherwise     emulated (xreg_emu, written by memu)
// The driver is a class holding a virtual interface, the way a UVM driver
// reaches the transactor.
`timescale 1ns/1ps
class reg_driver;
  virtual memu_host_if #(8, 32) bus;
  int errors = 0;

  function new(virtual memu_host_if #(8, 32) bus);
    this.bus = bus;
  endfunction

  task wr(input logic [7:0] addr, input logic [31:0] data, input logic [3:0] strb = 4'hf);
    logic [1:0] resp;
    bus.write(addr, data, resp, strb);
    $display("xreg: write %h = %h strb %b -> resp %0d", addr, data, strb, resp);
  endtask

  task rd(input logic [7:0] addr, input logic [31:0] want, input bit check = 1);
    logic [31:0] data;
    logic [1:0] resp;
    bus.read(addr, data, resp);
    $display("xreg: read  %h = %h resp %0d", addr, data, resp);
    if (check && data !== want) begin
      errors++;
      $display("xreg: FAIL, expected %h", want);
    end
  endtask

  task run();
    bus.idle(4);
    wr(8'h00, 32'h11111111);
    wr(8'h04, 32'h22222222);
    wr(8'h08, 32'h0000beef);
    wr(8'h0c, 32'hffffffff, 4'b1000);
    rd(8'h00, 32'h11111111);
    rd(8'h0c, 32'hff000000);
    rd(8'h14, 32'h11111111 + 32'h22222222 + 32'h0000beef + 32'hff000000);
    rd(8'h10, 0, 0);     // the cycle counter: the same in both runs
    bus.idle(37);
    rd(8'h10, 0, 0);
    wr(8'h40, 32'h1);    // nothing there
    rd(8'h40, 0);
    rd(8'h10, 0, 0);
  endtask
endclass

module xreg_tb;
  logic emu_clk = 1'b0, emu_rst = 1'b1, rst_n = 1'b0;
  always #1 emu_clk = ~emu_clk;
  logic [7:0]  awaddr, araddr;
  logic [31:0] wdata, rdata;
  logic [3:0]  wstrb;
  logic [1:0]  bresp, rresp;
  logic awvalid, awready, wvalid, wready, bvalid, bready, arvalid, arready, rvalid, rready;
  wire emu_ready, emu_run, emu_edge, next_pos;

  memu_host_if #(.ADDR_W(8), .DATA_W(32)) host (.emu_clk);
  memu_axil_master_xtor #(.ADDR_W(8)) xtor (
      .emu_clk, .emu_rst, .emu_ready, .emu_next_pos(next_pos), .emu_run_i(1'b1), .emu_run, .emu_edge,
      .awaddr, .awvalid, .awready, .wdata, .wstrb, .wvalid, .wready, .bresp, .bvalid, .bready,
      .araddr, .arvalid, .arready, .rdata, .rresp, .rvalid, .rready,
      .cmd_valid(host.cmd_valid), .cmd_op(host.cmd_op), .cmd_addr(host.cmd_addr), .cmd_data(host.cmd_data),
      .cmd_strb(host.cmd_strb), .rsp_valid(host.rsp_valid), .rsp_data(host.rsp_data),
      .rsp_resp(host.rsp_resp));

`ifdef GOLD
  wire clk;
  assign emu_ready = 1'b1;
  assign next_pos = 1'b1;
  memu_sim_clock sim_clock (.emu_clk, .emu_edge, .clk);
  xreg dut (.*);
`else
  wire [0:0] emu_next_pos;
  assign next_pos = emu_next_pos[0];
  xreg_emu dut (.emu_clk, .emu_rst, .emu_run, .emu_ready, .emu_pos(), .emu_neg(), .emu_time(),
                .emu_next_pos, .emu_next_neg(), .emu_next_time(), .rst_n,
                .awaddr, .awvalid, .awready, .wdata, .wstrb, .wvalid, .wready, .bresp, .bvalid, .bready,
                .araddr, .arvalid, .arready, .rdata, .rresp, .rvalid, .rready);
`endif

  // The design's reset goes away at its second clock edge.
  int edges = 0;
  always @(posedge emu_clk) if (emu_edge) begin
    edges++;
    if (edges == 2) rst_n <= 1'b1;
  end

  initial begin
    reg_driver drv;
    repeat (20) @(posedge emu_clk);
    emu_rst <= 1'b0;
    drv = new(host);
    drv.run();
    if (drv.errors == 0) $display("xreg: PASS");
    $finish;
  end
endmodule
