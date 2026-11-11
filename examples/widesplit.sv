// Wide signals (packed structs, concatenated buses) that msim -compile keeps
// as pieces: fields read and written on their own, whole copies, a whole
// write from an expression, a part write across two fields, registers, and a
// signed wide value. Every mode and the waveform must agree.
typedef struct packed {
  logic [31:0] pc;
  logic [47:0] addr;
  logic [15:0] tag;
  logic        valid;
} cap_t;                               // 97 bits

module ws_stage(input logic clk, input cap_t in_i, input logic sel, output cap_t out_o,
                output logic [127:0] bus_o);
  cap_t d, q, alt;
  logic [127:0] bus_d, bus_q;
  always_comb begin
    alt = in_i;
    alt.tag = in_i.tag ^ 16'h00ff;     // a field on top of a whole copy
    alt.valid = ~in_i.valid;
  end
  assign d = sel ? alt : in_i;         // whole write from a mux
  always_ff @(posedge clk) q <= d;
  always_ff @(posedge clk) begin       // part writes, one across a piece border
    bus_q[31:0]   <= q.pc + 32'd1;
    bus_q[95:32]  <= {q.addr, q.tag};
    bus_q[127:96] <= bus_q[31:0] ^ {31'b0, q.valid};
  end
  assign bus_d = {bus_q[63:0], bus_q[127:64]} ^ {32'h0, q[96:1]};
  assign out_o = q;
  assign bus_o = bus_d;
endmodule

module widesplit;
  logic clk = 0;
  logic [31:0] n = 0;
  cap_t a, b;
  logic [127:0] bus;
  logic signed [79:0] acc = 0;
  logic [31:0] sum = 0;
  always #5 clk = ~clk;
  always_ff @(posedge clk) begin
    n <= n * 32'd1664525 + 32'd1013904223;
    a.pc <= n;
    a.addr <= {n[15:0], n};
    a.tag <= n[31:16];
    a.valid <= n[7];
  end
  ws_stage s(.clk(clk), .in_i(a), .sel(n[3]), .out_o(b), .bus_o(bus));
  always_ff @(posedge clk)
    if (!$isunknown({b, bus})) begin
      acc <= acc - $signed({b.tag, bus[63:0]});
      sum <= sum * 33 + b.pc + {31'b0, b.valid} + bus[127:96] + bus[40:9] + acc[79:48] + {31'b0, acc < 0};
    end
  initial begin
    #600;
    $display("sum=%h b=%h bus=%h acc=%h", sum, b, bus, acc);
    $finish;
  end
endmodule
