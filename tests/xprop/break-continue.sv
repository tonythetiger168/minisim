// msim -xprop: break and continue under an unknown condition in a for or
// foreach loop. The rest of the loop merges with not running: a priority
// encoder with an unknown request gives x where the candidates differ.
// (Pessimistic: each unknown step merges with the state before it, so
// with req = 4'b01x0 found is x, though both cases find a request.)
//! args: -xprop
module top;
  logic [3:0] req;
  logic [1:0] g, h;
  logic found;
  logic [7:0] sum;
  logic [7:0] v [4];
  always_comb begin
    g = 2'd0; found = 1'b0;
    for (int i = 0; i < 4; i++)
      if (req[i]) begin g = i[1:0]; found = 1'b1; break; end
  end
  always_comb begin
    h = 2'd3;
    foreach (v[i]) begin
      if (v[i] == 8'h00) continue;
      h = i[1:0];
      if (v[i] > 8'h40) break;
    end
  end
  always_comb begin
    sum = 8'd0;
    for (int i = 0; i < 4; i++) begin
      if (!req[i]) continue;
      sum = sum + 8'd1;
    end
  end
  initial begin
    v[0] = 8'h00; v[1] = 8'h10; v[2] = 8'h50; v[3] = 8'h20;
    req = 4'b0100; #1 $display("g=%d found=%b h=%d sum=%0d", g, found, h, sum);
    req = 4'b01x0; #1 $display("g=%b found=%b sum=%b", g, found, sum);
    req = 4'b0x01; #1 $display("g=%b found=%b sum=%b", g, found, sum);
    req = 4'bx000; #1 $display("g=%b found=%b", g, found);
    v[2] = 8'hxx;  #1 $display("h=%b", h);
    $finish;
  end
endmodule
//> g=2 found=1 h=2 sum=1
//> g=xx found=x sum=xxxxxxxx
//> g=00 found=1 sum=000000xx
//> g=xx found=x
//> h=xx
