// IEEE 1800-2023 8.25: a class parameter or localparam, and an enum value
// declared in the class, can be read through an object handle (h.P); the
// value is the constant of the handle's class specialization, also when the
// handle is null.
//! icarus-skip: Icarus 13 does not parse class localparams of type real or string
module t;
  class c #(int W = 4);
    localparam int VMIN = 8'h20;
    localparam real R = 1.5;
    localparam string S = "name";
    typedef enum {A, B, C} e_t;
  endclass
  initial begin
    static c h = new; static c #(8) h8 = new; static c n;
    $display("%0d %0d %g %s %0d", h.VMIN, h8.W, h.R, h.S, h.C);
    $display("%0d %s", n.VMIN + 1, {"x", n.S});   // a null handle: still the constant
  end
endmodule
//> 32 8 1.5 name 2
//> 33 xname
