// IEEE 1800-2023 13.5, 11.4.12: an output or inout argument whose actual is
// a concatenation of variables (and part selects). On return each part
// takes its slice of the formal, the leftmost part the most significant
// bits; an inout's formal starts as the concatenated value.
module top;
  typedef enum bit [3:0] {IDLE = 0, BUSY = 9} st_e;
  bit [7:0] a;
  st_e e;
  bit [3:0] b;
  logic [7:0] w = 8'h00;
  task automatic fill(output bit [15:0] d);
    d = 16'hc3_95;
  endtask
  task automatic bump(inout bit [11:0] v, output int old);
    old = v;
    v = v + 12'h101;
  endtask
  initial begin
    fill({a, e, b});
    $display("a=%h e=%s b=%h", a, e.name(), b);
    w = 8'h3c;
    begin
      int o;
      bump({w[7:4], a}, o);
      $display("old=%h w=%h a=%h", o, w, a);
    end
  end
endmodule
//> a=c3 e=BUSY b=5
//> old=000003c3 w=4c a=c4
