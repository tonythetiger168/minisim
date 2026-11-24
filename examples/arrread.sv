// An unpacked array read at a run-time index whose elements are copies of
// flops in a generate loop, plus a constant element 0 (Ibex's register
// file): -compile keeps the elements private and reads them in place.
module rf(input logic clk, input logic we, input logic [2:0] wa, input logic [7:0] wd,
          input logic [2:0] ra, input logic [2:0] rb, output logic [7:0] rd, output logic [7:0] rq);
  logic [7:0] r [8], s [8];
  for (genvar i = 1; i < 8; i++) begin : g
    logic [7:0] q;
    always_ff @(posedge clk) if (we && wa == i) q <= wd;
    assign r[i] = q;
    assign s[i] = q ^ 8'(i);
  end
  assign r[0] = 8'h5a;
  assign s[0] = 8'h33;
  assign rd = r[ra];
  // A clocked reader (it reads the registers' old values): s stays in S.sigs.
  always_ff @(posedge clk) rq <= s[rb] + 8'd1;
endmodule
module arrread;
  logic clk = 0, we; logic [2:0] wa, ra, rb; logic [7:0] wd, rd, rq;
  rf u(.*);
  int sum = 0;
  initial begin
    we = 0; wa = 0; wd = 0; ra = 0; rb = 0;
    for (int c = 0; c < 300; c++) begin
      we = c[0] | c[4]; wa = c[3:1] ^ c[6:4]; wd = c * 7 + 3; ra = c[5:3] ^ c[2:0]; rb = c[2:0] + c[7:5];
      #1 clk = 1; #1 clk = 0;
      sum = sum * 3 + rd + 5 * rq;
    end
    $display("sum=%0d rd=%0d rq=%0d", sum, rd, rq);
    $finish;
  end
endmodule
