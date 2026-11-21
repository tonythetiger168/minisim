// msim -compile, event mode: Sim::set / Sim::setPart compare the new
// words with the stored ones and return when nothing changed; a change of
// a narrow signal is stored and wakes its readers on a short path
// (commitNarrow). Here constant part writes move bit 0 of a vector (so
// @(posedge) / @(negedge) waiters on it must fire, x to 1 included), leave
// it as it was (no wake), and write x and z bits.
module setpart;
  logic clk = 0;
  logic [7:0] a, b;
  logic [3:0] c;
  int pe = 0, ne = 0, an = 0;
  always #5 clk = ~clk;
  always @* begin   // part writes of a comb block
    a = 8'h00;
    a[3:0] = b[3:0] ^ {c[2:0], c[3]};
    a[7:4] = b[7:4];
    a[0] = c[0] & b[0];
  end
  always @(posedge a[0]) pe++;
  always @(negedge a[0]) ne++;
  always @(a) an++;
  initial begin
    b = 'x; c = 'z;
    #1 $display("t1 a=%b", a);
    b = 8'h01; c = 4'h1;
    #1 $display("t2 a=%b pe=%0d ne=%0d", a, pe, ne);
    b[7:4] = 4'b10xz;
    #1 $display("t3 a=%b", a);
    for (int i = 0; i < 40; i++) begin
      @(posedge clk);
      b[3:0] = i[3:0];
      c[1:0] = i[5:4];
      c[3:2] = i[3] ? 2'b1z : 2'b01;
    end
    #1 $display("end a=%b pe=%0d ne=%0d an=%0d", a, pe, ne, an);
    $finish;
  end
endmodule
