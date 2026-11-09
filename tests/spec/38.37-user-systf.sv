// IEEE 1800-2023 38.37, 38.34, 38.35: system tasks and functions defined
// in C through vpi_register_systf. A variable or net argument is the object
// itself (its name, size, value); any other argument is an expression.
// Functions return through vpi_put_value on the call handle: an int, a
// sized (sizetf) and a real function. compiletf runs once per call site
// before the simulation.
//! args: data/vpi/systf.c
//! icarus-vpi: data/vpi/systf.c
module top;
  reg [7:0] r = 8'b1010_zz01;
  wire [3:0] w = 4'hc;
  integer i = -5;
  reg [15:0] t;
  real x;
  child u();
  initial begin
    #1 $show(r, w, i, r + 1, "hi", u);
    $display("sum=%0d mask=%b calls=%0d", $sum(1, 2, i), $mask(0), $calls);
    $poke(t, 16'h12x4);
    $display("t=%h", t);
    x = $half(5.0);
    $display("x=%0.2f", x);
    #2 $show(t[15:8]);
  end
endmodule
module child;
endmodule
//> 1 $show line 17: r(8)=b1010zz01,haZ,dZ w(4)=b1100,hc,d12 i(32)=b11111111111111111111111111111011,hfffffffb,d-5 expr(32)=bxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx,hxxxxxxxx,dx "hi" module top.u
//> sum=-2 mask=1111zzzzxxxx calls=3
//> t=12x4
//> x=2.50
//> 3 $show line 23: expr(8)=b00010010,h12,d18
