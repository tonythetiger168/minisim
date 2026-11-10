// A design that is a bus slave: four registers, a cycle counter and their sum
// behind one AXI4-Lite port. The host reads and writes it through a
// transactor.
module xreg (input clk, input rst_n,
             input [7:0] awaddr, input awvalid, output awready,
             input [31:0] wdata, input [3:0] wstrb, input wvalid, output wready,
             output [1:0] bresp, output bvalid, input bready,
             input [7:0] araddr, input arvalid, output arready,
             output [31:0] rdata, output [1:0] rresp, output rvalid, input rready);
  reg [31:0] r0, r1, r2, r3, cycles, rdata_r;
  reg        bvalid_r, rvalid_r, berr, rerr;

  // A write needs both halves in the same cycle; a master that holds them
  // (every AXI master must) gets there.
  wire wr = awvalid & wvalid & ~bvalid_r;
  wire rd = arvalid & ~rvalid_r;
  assign awready = wr; assign wready = wr; assign arready = rd;
  assign bvalid = bvalid_r; assign bresp = {berr, 1'b0};
  assign rvalid = rvalid_r; assign rresp = {rerr, 1'b0}; assign rdata = rdata_r;

  wire [31:0] mask = {{8{wstrb[3]}}, {8{wstrb[2]}}, {8{wstrb[1]}}, {8{wstrb[0]}}};
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      r0 <= 32'd0; r1 <= 32'd0; r2 <= 32'd0; r3 <= 32'd0; cycles <= 32'd0; rdata_r <= 32'd0;
      bvalid_r <= 1'b0; rvalid_r <= 1'b0; berr <= 1'b0; rerr <= 1'b0;
    end else begin
      cycles <= cycles + 32'd1;
      if (bvalid_r & bready) bvalid_r <= 1'b0;
      if (rvalid_r & rready) rvalid_r <= 1'b0;
      if (wr) begin
        bvalid_r <= 1'b1;
        berr <= (awaddr[7:4] != 4'd0);
        case (awaddr[7:2])
          6'd0: r0 <= (r0 & ~mask) | (wdata & mask);
          6'd1: r1 <= (r1 & ~mask) | (wdata & mask);
          6'd2: r2 <= (r2 & ~mask) | (wdata & mask);
          6'd3: r3 <= (r3 & ~mask) | (wdata & mask);
          default: ;
        endcase
      end
      if (rd) begin
        rvalid_r <= 1'b1;
        rerr <= (araddr[7:2] > 6'd5);
        case (araddr[7:2])
          6'd0: rdata_r <= r0;
          6'd1: rdata_r <= r1;
          6'd2: rdata_r <= r2;
          6'd3: rdata_r <= r3;
          6'd4: rdata_r <= cycles;
          6'd5: rdata_r <= r0 + r1 + r2 + r3;
          default: rdata_r <= 32'd0;
        endcase
      end
    end
  end
endmodule
