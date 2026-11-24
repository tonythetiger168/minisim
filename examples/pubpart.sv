// msim -compile: comb writes of S.sigs signals (an array read at a run-time
// index), whole and in parts, compare before they store (regWrite); one of
// them is also waited on. Three clocked blocks read each other's registers
// (orderDomain's exact order for a small group).
module pubpart;
  logic clk = 0;
  logic [7:0] arr [4];
  logic [7:0] a = 8'h11, b = 8'h5a, c = 0;
  logic [3:0] st = 0;
  logic [1:0] idx = 0;
  logic [31:0] acc = 0;
  int seen = 0;
  always_comb begin
    arr[0] = a; arr[1] = b; arr[2] = a ^ b;
    arr[3][3:0] = st; arr[3][7:4] = a[3:0];
  end
  always @(posedge clk) begin a <= a + b[2:0] + arr[idx] + c[1:0]; idx <= idx + 1; end
  always @(posedge clk) begin b <= b ^ {a[6:0], a[7]}; st <= st + a[0] + c[0]; end
  always @(posedge clk) begin c <= c + (a == b) + st[1]; acc <= acc * 31 + a + b + st + c + arr[idx]; end
  always @(arr[3]) seen++;
  initial begin
    repeat (300) begin #5 clk = 1; #5 clk = 0; end
    $display("acc=%h a=%h b=%h c=%h st=%h seen=%0d", acc, a, b, c, st, seen); $finish;
  end
endmodule
