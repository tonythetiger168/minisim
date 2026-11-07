// bench/lanes.sv - throughput benchmark for msim (interpreter vs -compile) and other simulators.
// 16 lanes of a 32-bit LFSR feeding a 32-bit accumulator, one clock with period 10, run to END_TIME.
// Every lane is its own always block, plus one continuous assign per lane.
// Prints one checksum so results can be compared.
// END_TIME is 10 per cycle: 2000000 is 200k cycles.
`ifndef END_TIME
`define END_TIME 2000000
`endif
module lanes;
  reg clk;
  reg [31:0] lfsr0, acc0;
  wire [31:0] mix0;
  reg [31:0] lfsr1, acc1;
  wire [31:0] mix1;
  reg [31:0] lfsr2, acc2;
  wire [31:0] mix2;
  reg [31:0] lfsr3, acc3;
  wire [31:0] mix3;
  reg [31:0] lfsr4, acc4;
  wire [31:0] mix4;
  reg [31:0] lfsr5, acc5;
  wire [31:0] mix5;
  reg [31:0] lfsr6, acc6;
  wire [31:0] mix6;
  reg [31:0] lfsr7, acc7;
  wire [31:0] mix7;
  reg [31:0] lfsr8, acc8;
  wire [31:0] mix8;
  reg [31:0] lfsr9, acc9;
  wire [31:0] mix9;
  reg [31:0] lfsr10, acc10;
  wire [31:0] mix10;
  reg [31:0] lfsr11, acc11;
  wire [31:0] mix11;
  reg [31:0] lfsr12, acc12;
  wire [31:0] mix12;
  reg [31:0] lfsr13, acc13;
  wire [31:0] mix13;
  reg [31:0] lfsr14, acc14;
  wire [31:0] mix14;
  reg [31:0] lfsr15, acc15;
  wire [31:0] mix15;
  assign mix0 = (lfsr0 ^ acc15) + {acc0[15:0], lfsr0[31:16]};
  assign mix1 = (lfsr1 ^ acc0) + {acc1[15:0], lfsr1[31:16]};
  assign mix2 = (lfsr2 ^ acc1) + {acc2[15:0], lfsr2[31:16]};
  assign mix3 = (lfsr3 ^ acc2) + {acc3[15:0], lfsr3[31:16]};
  assign mix4 = (lfsr4 ^ acc3) + {acc4[15:0], lfsr4[31:16]};
  assign mix5 = (lfsr5 ^ acc4) + {acc5[15:0], lfsr5[31:16]};
  assign mix6 = (lfsr6 ^ acc5) + {acc6[15:0], lfsr6[31:16]};
  assign mix7 = (lfsr7 ^ acc6) + {acc7[15:0], lfsr7[31:16]};
  assign mix8 = (lfsr8 ^ acc7) + {acc8[15:0], lfsr8[31:16]};
  assign mix9 = (lfsr9 ^ acc8) + {acc9[15:0], lfsr9[31:16]};
  assign mix10 = (lfsr10 ^ acc9) + {acc10[15:0], lfsr10[31:16]};
  assign mix11 = (lfsr11 ^ acc10) + {acc11[15:0], lfsr11[31:16]};
  assign mix12 = (lfsr12 ^ acc11) + {acc12[15:0], lfsr12[31:16]};
  assign mix13 = (lfsr13 ^ acc12) + {acc13[15:0], lfsr13[31:16]};
  assign mix14 = (lfsr14 ^ acc13) + {acc14[15:0], lfsr14[31:16]};
  assign mix15 = (lfsr15 ^ acc14) + {acc15[15:0], lfsr15[31:16]};
  always @(posedge clk) begin
    lfsr0 <= {lfsr0[30:0], lfsr0[31] ^ lfsr0[21] ^ lfsr0[1] ^ lfsr0[0]};
    if (mix0[0]) acc0 <= acc0 + mix0;
    else acc0 <= acc0 - (mix0 >> 3);
  end
  always @(posedge clk) begin
    lfsr1 <= {lfsr1[30:0], lfsr1[31] ^ lfsr1[21] ^ lfsr1[1] ^ lfsr1[0]};
    if (mix1[0]) acc1 <= acc1 + mix1;
    else acc1 <= acc1 - (mix1 >> 3);
  end
  always @(posedge clk) begin
    lfsr2 <= {lfsr2[30:0], lfsr2[31] ^ lfsr2[21] ^ lfsr2[1] ^ lfsr2[0]};
    if (mix2[0]) acc2 <= acc2 + mix2;
    else acc2 <= acc2 - (mix2 >> 3);
  end
  always @(posedge clk) begin
    lfsr3 <= {lfsr3[30:0], lfsr3[31] ^ lfsr3[21] ^ lfsr3[1] ^ lfsr3[0]};
    if (mix3[0]) acc3 <= acc3 + mix3;
    else acc3 <= acc3 - (mix3 >> 3);
  end
  always @(posedge clk) begin
    lfsr4 <= {lfsr4[30:0], lfsr4[31] ^ lfsr4[21] ^ lfsr4[1] ^ lfsr4[0]};
    if (mix4[0]) acc4 <= acc4 + mix4;
    else acc4 <= acc4 - (mix4 >> 3);
  end
  always @(posedge clk) begin
    lfsr5 <= {lfsr5[30:0], lfsr5[31] ^ lfsr5[21] ^ lfsr5[1] ^ lfsr5[0]};
    if (mix5[0]) acc5 <= acc5 + mix5;
    else acc5 <= acc5 - (mix5 >> 3);
  end
  always @(posedge clk) begin
    lfsr6 <= {lfsr6[30:0], lfsr6[31] ^ lfsr6[21] ^ lfsr6[1] ^ lfsr6[0]};
    if (mix6[0]) acc6 <= acc6 + mix6;
    else acc6 <= acc6 - (mix6 >> 3);
  end
  always @(posedge clk) begin
    lfsr7 <= {lfsr7[30:0], lfsr7[31] ^ lfsr7[21] ^ lfsr7[1] ^ lfsr7[0]};
    if (mix7[0]) acc7 <= acc7 + mix7;
    else acc7 <= acc7 - (mix7 >> 3);
  end
  always @(posedge clk) begin
    lfsr8 <= {lfsr8[30:0], lfsr8[31] ^ lfsr8[21] ^ lfsr8[1] ^ lfsr8[0]};
    if (mix8[0]) acc8 <= acc8 + mix8;
    else acc8 <= acc8 - (mix8 >> 3);
  end
  always @(posedge clk) begin
    lfsr9 <= {lfsr9[30:0], lfsr9[31] ^ lfsr9[21] ^ lfsr9[1] ^ lfsr9[0]};
    if (mix9[0]) acc9 <= acc9 + mix9;
    else acc9 <= acc9 - (mix9 >> 3);
  end
  always @(posedge clk) begin
    lfsr10 <= {lfsr10[30:0], lfsr10[31] ^ lfsr10[21] ^ lfsr10[1] ^ lfsr10[0]};
    if (mix10[0]) acc10 <= acc10 + mix10;
    else acc10 <= acc10 - (mix10 >> 3);
  end
  always @(posedge clk) begin
    lfsr11 <= {lfsr11[30:0], lfsr11[31] ^ lfsr11[21] ^ lfsr11[1] ^ lfsr11[0]};
    if (mix11[0]) acc11 <= acc11 + mix11;
    else acc11 <= acc11 - (mix11 >> 3);
  end
  always @(posedge clk) begin
    lfsr12 <= {lfsr12[30:0], lfsr12[31] ^ lfsr12[21] ^ lfsr12[1] ^ lfsr12[0]};
    if (mix12[0]) acc12 <= acc12 + mix12;
    else acc12 <= acc12 - (mix12 >> 3);
  end
  always @(posedge clk) begin
    lfsr13 <= {lfsr13[30:0], lfsr13[31] ^ lfsr13[21] ^ lfsr13[1] ^ lfsr13[0]};
    if (mix13[0]) acc13 <= acc13 + mix13;
    else acc13 <= acc13 - (mix13 >> 3);
  end
  always @(posedge clk) begin
    lfsr14 <= {lfsr14[30:0], lfsr14[31] ^ lfsr14[21] ^ lfsr14[1] ^ lfsr14[0]};
    if (mix14[0]) acc14 <= acc14 + mix14;
    else acc14 <= acc14 - (mix14 >> 3);
  end
  always @(posedge clk) begin
    lfsr15 <= {lfsr15[30:0], lfsr15[31] ^ lfsr15[21] ^ lfsr15[1] ^ lfsr15[0]};
    if (mix15[0]) acc15 <= acc15 + mix15;
    else acc15 <= acc15 - (mix15 >> 3);
  end
  always #5 clk = ~clk;
  initial begin
    lfsr0 = 32'd19088743; acc0 = 32'd0;
    lfsr1 = 32'd19096662; acc1 = 32'd1;
    lfsr2 = 32'd19104581; acc2 = 32'd2;
    lfsr3 = 32'd19112500; acc3 = 32'd3;
    lfsr4 = 32'd19120419; acc4 = 32'd4;
    lfsr5 = 32'd19128338; acc5 = 32'd5;
    lfsr6 = 32'd19136257; acc6 = 32'd6;
    lfsr7 = 32'd19144176; acc7 = 32'd7;
    lfsr8 = 32'd19152095; acc8 = 32'd8;
    lfsr9 = 32'd19160014; acc9 = 32'd9;
    lfsr10 = 32'd19167933; acc10 = 32'd10;
    lfsr11 = 32'd19175852; acc11 = 32'd11;
    lfsr12 = 32'd19183771; acc12 = 32'd12;
    lfsr13 = 32'd19191690; acc13 = 32'd13;
    lfsr14 = 32'd19199609; acc14 = 32'd14;
    lfsr15 = 32'd19207528; acc15 = 32'd15;
    clk = 0;
    #`END_TIME
    $display("checksum=%d", acc0 ^ acc1 ^ acc2 ^ acc3 ^ acc4 ^ acc5 ^ acc6 ^ acc7 ^ acc8 ^ acc9 ^ acc10 ^ acc11 ^ acc12 ^ acc13 ^ acc14 ^ acc15);
    $finish;
  end
endmodule
