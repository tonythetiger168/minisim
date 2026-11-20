// msim -compile: a cycle domain with 32 or more islands runs its groups in
// one parallel pass when nothing else runs before the NBA region (the
// testbench clock drives the domain directly, as after wire substitution
// of the clock ports). That pass must also run the @* blocks whose inputs
// it changed (armChk), as the NBA-region commit does: here each cpu's
// la_wstrb / la_wdata, without which no store reached its memory.
module cpu(input logic clk, resetn, input logic ready, input logic [31:0] rdata,
           output logic valid, output logic [3:0] wstrb, output logic [7:0] addr, output logic [31:0] wdata);
  logic [31:0] acc;
  logic [3:0] la_wstrb;
  logic [31:0] la_wdata;
  always @* begin   // an @* block in the cycle domain, as PicoRV32's mem_la_wstrb
    la_wstrb = acc[0] ? 4'hf : 4'h0;
    la_wdata = acc + 1;
  end
  always_ff @(posedge clk) begin
    if (!resetn) begin valid <= 0; wstrb <= 0; addr <= 0; wdata <= 0; acc <= 0; end
    else if (valid && ready) begin valid <= 0; wstrb <= 0; acc <= acc + rdata; end
    else if (!valid) begin valid <= 1; addr <= acc[0] ? 8'd65 : 8'd1; wstrb <= la_wstrb; wdata <= la_wdata; end
  end
endmodule
module core(input logic clk, resetn, output logic [31:0] sum);
  logic [31:0] mem [0:255];
  logic valid, ready;
  logic [3:0] wstrb;
  logic [7:0] addr;
  logic [31:0] wdata, rdata;
  cpu u(.clk(clk), .resetn(resetn), .ready(ready), .rdata(rdata), .valid(valid), .wstrb(wstrb), .addr(addr), .wdata(wdata));
  always_ff @(posedge clk) begin
    ready <= 1'b0;
    if (valid && !ready) begin
      rdata <= mem[addr];
      if (wstrb[0]) mem[addr][7:0] <= wdata[7:0];
      if (wstrb[1]) mem[addr][15:8] <= wdata[15:8];
      ready <= 1'b1;
    end
  end
  assign sum = mem[65];
  initial begin for (int k = 0; k < 256; k++) mem[k] = 0; mem[1] = 1; end
endmodule
module fusepar;
  logic clk = 0, resetn = 0;
  logic [31:0] sum [16];
  core c0(.clk(clk), .resetn(resetn), .sum(sum[0]));
  core c1(.clk(clk), .resetn(resetn), .sum(sum[1]));
  core c2(.clk(clk), .resetn(resetn), .sum(sum[2]));
  core c3(.clk(clk), .resetn(resetn), .sum(sum[3]));
  core c4(.clk(clk), .resetn(resetn), .sum(sum[4]));
  core c5(.clk(clk), .resetn(resetn), .sum(sum[5]));
  core c6(.clk(clk), .resetn(resetn), .sum(sum[6]));
  core c7(.clk(clk), .resetn(resetn), .sum(sum[7]));
  core c8(.clk(clk), .resetn(resetn), .sum(sum[8]));
  core c9(.clk(clk), .resetn(resetn), .sum(sum[9]));
  core c10(.clk(clk), .resetn(resetn), .sum(sum[10]));
  core c11(.clk(clk), .resetn(resetn), .sum(sum[11]));
  core c12(.clk(clk), .resetn(resetn), .sum(sum[12]));
  core c13(.clk(clk), .resetn(resetn), .sum(sum[13]));
  core c14(.clk(clk), .resetn(resetn), .sum(sum[14]));
  core c15(.clk(clk), .resetn(resetn), .sum(sum[15]));
  always #5 clk = ~clk;
  initial begin
    logic [31:0] chk;
    #20 resetn = 1;
    #2000;
    chk = 0;
    for (int c = 0; c < 16; c++) chk = (chk ^ sum[c]) + c + 1;
    $display("chk=%h sum0=%h", chk, sum[0]);
    $finish;
  end
endmodule
