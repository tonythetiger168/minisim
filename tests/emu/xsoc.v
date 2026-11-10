// A design that is the bus master: it fills a memory, reads it back, pokes a
// UART and waits for its interrupt, all through one AXI4-Lite port. The
// memory and the UART are C models on the host (msim -mvp); in the emulation
// a transactor carries the accesses to them.
module xsoc (input clk, input rst_n,
             output [15:0] awaddr, output awvalid, input awready,
             output [31:0] wdata, output [3:0] wstrb, output wvalid, input wready,
             input [1:0] bresp, input bvalid, output bready,
             output [15:0] araddr, output arvalid, input arready,
             input [31:0] rdata, input [1:0] rresp, input rvalid, output rready,
             input irq, output done, output [31:0] sum, output [15:0] waited);
  localparam [1:0] S_GO = 2'd0, S_BUS = 2'd1, S_IRQ = 2'd2, S_END = 2'd3;
  reg [1:0]  st;
  reg [4:0]  step;
  reg [31:0] lfsr, sum_r;
  reg [15:0] waited_r;
  reg        awv, wv, arv;

  // The program: one bus access per step.
  reg        op_wr, op_irq;
  reg [15:0] op_addr;
  reg [31:0] op_data;
  reg [3:0]  op_strb;
  always @* begin
    op_wr = 1'b0; op_irq = 1'b0; op_addr = 16'h0; op_data = lfsr; op_strb = 4'hf;
    if (step < 5'd8) begin op_wr = 1'b1; op_addr = {11'd0, step[2:0], 2'b00}; end
    else if (step < 5'd16) op_addr = {11'd0, step[2:0], 2'b00};
    else case (step)
      5'd16: begin op_wr = 1'b1; op_addr = 16'h0004; op_data = 32'hffffffff; op_strb = 4'b0101; end
      5'd17: op_addr = 16'h0004;
      5'd18: op_addr = 16'h3000;                                   // nothing there
      5'd19: begin op_wr = 1'b1; op_addr = 16'h3000; end
      5'd20: begin op_wr = 1'b1; op_addr = 16'h1008; op_data = 32'h1; end   // UART IER
      5'd21: begin op_wr = 1'b1; op_addr = 16'h1000; op_data = 32'h48; op_irq = 1'b1; end  // 'H'
      5'd22: begin op_wr = 1'b1; op_addr = 16'h100c; op_data = 32'h0; end   // clear it
      5'd23: op_addr = 16'h1004;                                   // LSR
      default: op_addr = 16'h0000;
    endcase
  end

  assign awaddr = op_addr; assign araddr = op_addr;
  assign wdata = op_data;  assign wstrb = op_strb;
  assign awvalid = awv;    assign wvalid = wv;   assign arvalid = arv;
  assign bready = (st == S_BUS) & op_wr;
  assign rready = (st == S_BUS) & ~op_wr;
  assign done = (st == S_END);
  assign sum = sum_r;
  assign waited = waited_r;

  wire last = (step == 5'd24);
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      st <= S_GO; step <= 5'd0; lfsr <= 32'h1badf00d; sum_r <= 32'd0; waited_r <= 16'd0;
      awv <= 1'b0; wv <= 1'b0; arv <= 1'b0;
    end else case (st)
      S_GO: if (last) st <= S_END;
            else begin
              awv <= op_wr; wv <= op_wr; arv <= ~op_wr;
              st <= S_BUS;
            end
      S_BUS: begin
        if (awv & awready) awv <= 1'b0;
        if (wv & wready) wv <= 1'b0;
        if (arv & arready) arv <= 1'b0;
        if (op_wr ? bvalid : rvalid) begin
          sum_r <= {sum_r[30:0], sum_r[31]} ^ (op_wr ? {30'd0, bresp} : rdata ^ {30'd0, rresp});
          lfsr <= {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};
          if (op_irq) st <= S_IRQ;
          else begin step <= step + 5'd1; st <= S_GO; end
        end
      end
      S_IRQ: if (irq) begin step <= step + 5'd1; st <= S_GO; end
             else waited_r <= waited_r + 16'd1;
      default: ;
    endcase
  end
endmodule
