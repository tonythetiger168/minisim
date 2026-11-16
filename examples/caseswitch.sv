// msim -compile runs a case statement of a combinational process as one
// switch on the selector in its 2-state code (the tests have no effect, so
// the first item that holds is found at once). Selectors: a signal, a part
// select, a concatenation of part selects; items with several keys, a key
// repeated in a later item (the first wins), a key wider than the selector
// (never matches), casez with known keys, inside, and a case in a clocked
// process.
module csdec(input clk, input [31:0] ins, output reg [7:0] y = 0);
  reg [7:0] a, b, c, d;
  always_comb begin
    case (ins[6:0])
      7'h33, 7'h3b: a = 8'd1;
      7'h13: a = 8'd2;
      7'h03, 7'h23: a = 8'd3;
      7'h13: a = 8'd99;          // never: 7'h13 is taken above
      7'h63: a = 8'd4;
      default: a = 8'd0;
    endcase
    case ({ins[31:25], ins[14:12]})
      10'h000: b = 8'd10;
      10'h100: b = 8'd11;
      10'h001: b = 8'd12;
      10'h005, 10'h105: b = 8'd13;
      default: b = 8'd14;
    endcase
    casez (ins[14:12])
      3'b000: c = 8'd20;
      3'b001: c = 8'd21;
      3'b010: c = 8'd22;
      3'b111: c = 8'd23;
      default: c = 8'd24;
    endcase
    if (ins[11:7] inside {5'd1, 5'd3, 5'd5, 5'd7, 5'd9}) d = 8'd30;
    else d = 8'd31;
    case (ins[3:0])
      4'd1: d = d + 1;
      4'd2: d = d + 2;
      5'd17: d = 8'd0;           // wider than the selector: never matches
      4'd4: d = d + 4;
      4'd8: d = d + 8;
    endcase
  end
  always @(posedge clk) y <= a ^ (b << 1) ^ (c << 2) ^ (d << 3);
endmodule
module caseswitch;
  reg clk = 0;
  reg [31:0] r0 = 32'h1234_5678, r1 = 32'h9abc_def0;
  wire [7:0] y0, y1;
  csdec u0(.clk(clk), .ins(r0), .y(y0));
  csdec u1(.clk(clk), .ins(r1), .y(y1));
  always #1 clk = ~clk;
  reg [31:0] acc = 0;
  always @(posedge clk) begin
    r0 <= {r0[30:0], r0[31] ^ r0[21] ^ r0[1] ^ r0[0]};
    r1 <= r1 * 32'd1103515245 + 32'd12345;
    acc <= {acc[30:0], acc[31]} ^ {y0, y1, y0 ^ y1, y1 + y0};
  end
  // A case in a clocked process (its 2-state body).
  reg [7:0] st = 0;
  always @(posedge clk)
    case ({r1[9:8], r0[3:2]})
      4'd0, 4'd5: st <= st + 8'd1;
      4'd1: st <= st ^ 8'h5a;
      4'd2, 4'd9, 4'd12: st <= {st[6:0], st[7]};
      4'd5: st <= 8'd0;          // never: 4'd5 is taken above
      4'd15: st <= st - 8'd3;
      default: st <= st;
    endcase
  integer k;
  initial begin
    for (k = 0; k < 4; k = k + 1) begin
      #400;
      $display("t=%0t acc=%h y=%h %h st=%h", $time, acc, y0, y1, st);
    end
    $finish;
  end
endmodule
