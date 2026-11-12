// msim -xprop: break and continue in while, do ... while and repeat loops;
// an unknown loop condition after an unknown break makes what the loop
// assigns x. A foreach and a repeat whose body jumps from the clean copy
// into the merging copy keep their counters.
//! args: -xprop
module top;
  logic [7:0] req; logic [3:0] m, d, r, c, f, g, n, w, v; logic a [8];
  always_comb begin   // while with break
    m = 4'd0;
    while (m < 4'd8) begin
      if (req[m]) break;
      m = m + 4'd1;
    end
  end
  always_comb begin   // do ... while with break
    d = 4'd0;
    do begin
      if (req[d]) break;
      d = d + 4'd1;
    end while (d < 4'd8);
  end
  always_comb begin   // repeat with continue and break
    r = 4'd0; c = 4'd0;
    repeat (8) begin
      r = r + 4'd1;
      if (!req[r - 4'd1]) continue;
      c = c + 4'd1;
      if (c == 4'd2) break;
    end
  end
  always_comb begin   // while (1) and forever, left only by break
    w = 4'd0;
    while (1) begin
      if (req[w] || w == 4'd7) break;
      w = w + 4'd1;
    end
    v = 4'd0;
    forever begin
      if (req[v] || v == 4'd7) break;
      v = v + 4'd1;
    end
  end
  always_comb begin   // foreach with continue and break
    f = 4'd0; g = 4'd0;
    foreach (a[k]) begin
      f = f + 4'd1;
      if (!req[k]) continue;
      g = g + 4'd1;
      if (g == 4'd2) break;
    end
  end
  always_comb begin   // foreach and repeat without a break
    n = 4'd0;
    foreach (a[k]) if (req[k]) n = n + 4'd1;
    repeat (2) if (req[7]) n = n + 4'd1;
  end
  initial begin
    req = 8'b0001_0100; #1 $display("m=%0d d=%0d r=%0d c=%0d f=%0d g=%0d n=%0d w=%0d v=%0d", m, d, r, c, f, g, n, w, v);
    req = 8'b0000_0000; #1 $display("m=%0d d=%0d r=%0d c=%0d f=%0d g=%0d n=%0d w=%0d v=%0d", m, d, r, c, f, g, n, w, v);
    req = 8'b0001_0x00; #1 $display("m=%b d=%b r=%b c=%b f=%b g=%b n=%b w=%b v=%b", m, d, r, c, f, g, n, w, v);
    req = 8'b0000_00x1; #1 $display("m=%b d=%b r=%b c=%b f=%b g=%b n=%b w=%b v=%b", m, d, r, c, f, g, n, w, v);
    req = 8'b1000_0101; #1 $display("n=%0d", n);
    req = 8'b0000_0x01; #1 $display("n=%b", n);
    req = 8'bx000_0001; #1 $display("n=%b", n);
    $finish;
  end
endmodule
//> m=2 d=2 r=5 c=2 f=5 g=2 n=2 w=2 v=2
//> m=8 d=8 r=8 c=0 f=8 g=0 n=0 w=7 v=7
//> m=xxxx d=xxxx r=xxxx c=xxxx f=xxxx g=xxxx n=xxxx w=xxxx v=xxxx
//> m=0000 d=0000 r=xxxx c=xxxx f=xxxx g=00xx n=00xx w=0000 v=0000
//> n=5
//> n=00xx
//> n=xxxx
