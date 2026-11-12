// The shapes of a small CPU (PicoRV32-like) that msim -compile runs in the
// cycle domain:
// - always @* blocks (9.4.2.2), ranked like always_comb;
// - a clocked block that keeps working values in module variables with
//   blocking assignments, read before their first write too (they keep the
//   last value), one set to x, one written a bit at a time;
// - register-file and byte-enable memory writes at a run-time index.
// Every run must print the same line.
module seqtemp;
  logic clk = 0;
  logic [3:0] st = 0;
  logic [7:0] acc, pc = 0;
  logic [7:0] rf [0:7];
  logic [31:0] mem [0:63];
  logic [3:0] be;
  logic [7:0] dec, nxt;
  logic go, hold, carry;
  logic [7:0] cur, last;
  logic [3:0] flags, fl2;
  always #5 clk = ~clk;
  always @* begin
    dec = 8'h00;
    case (st[1:0])
      2'd0: dec = acc + 8'd1;
      2'd1: dec = acc ^ pc;
      2'd2: if (acc[0]) dec = acc >> 1; else dec = acc << 1;
      default: dec = ~acc;
    endcase
  end
  always @(st or acc) nxt = st[3] ? acc - 8'd3 : acc + 8'd5;
  assign be = 4'b0001 << pc[1:0];
  always @(posedge clk) begin
    go = 0;
    hold = 1'b0;
    cur = 'bx;
    flags[2] = 1'b0;
    if (st == 4'd3) go = 1;
    if (last[0]) hold = 1;
    cur = dec;
    if (go) flags[2] = 1'b1;
    fl2[1] = go ^ last[1];
    carry <= cur[7] & !hold ^ fl2[1];
    acc <= hold ? nxt : cur + {7'd0, go};
    last = cur;                       // read before it is written next time
    st <= st + 4'd1;
    pc <= pc + 8'd1;
    rf[pc[2:0]] <= acc ^ {5'd0, flags[2], 2'd0};
    if (be[0]) mem[pc[7:2]][7:0] <= acc;
    if (be[1]) mem[pc[7:2]][15:8] <= nxt;
    if (be[2]) mem[pc[7:2]][23:16] <= dec;
    if (be[3]) mem[pc[7:2]][31:24] <= last;
  end
  initial begin
    for (int k = 0; k < 64; k++) mem[k] = 0;
    #1 acc = 8'h11;                  // wakes the @* blocks (a declaration initializer would not)
    #639 $display("acc=%h pc=%h st=%h carry=%b rf=%h %h %h %h mem=%h %h %h", acc, pc, st, carry,
                  rf[0], rf[3], rf[5], rf[7], mem[0], mem[7], mem[15]);
    $finish;
  end
endmodule
