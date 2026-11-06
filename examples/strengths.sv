// Drive strength on a wire. A weaker driver must lose, equal 1 vs 0 must
// be X, and an explicit strength on assign or a simple gate must stick.
// Compared with Icarus Verilog 13 (v13_0).
module top;
  wire undriven;
  tri tri_undriven;

  wire strong_over_weak;
  assign (strong1, strong0) strong_over_weak = 1'b1;
  assign (weak1, weak0) strong_over_weak = 1'b0;

  wire strong0_wins;
  assign (weak1, weak0) strong0_wins = 1'b1;
  assign (strong1, strong0) strong0_wins = 1'b0;

  wire equal_x;
  assign (strong1, strong0) equal_x = 1'b1;
  assign (strong1, strong0) equal_x = 1'b0;

  wire pull_x;
  assign (pull1, pull0) pull_x = 1'b1;
  assign (pull1, pull0) pull_x = 1'b0;

  wire def_strong;
  assign def_strong = 1'b1;
  assign (weak1, weak0) def_strong = 1'b0;

  wire spec_one;
  assign (strong1, weak0) spec_one = 1'b1;
  assign (weak1, weak0) spec_one = 1'b0;

  wire spec_zero;
  assign (strong1, weak0) spec_zero = 1'b0;
  assign (pull1, pull0) spec_zero = 1'b1;

  wire x_unequal;
  assign (strong1, weak0) x_unequal = 1'bx;

  wire x_equal;
  assign (strong1, strong0) x_equal = 1'bx;

  wire z_drive;
  assign (strong1, weak0) z_drive = 1'bz;

  wire z_yields;
  assign (weak1, weak0) z_yields = 1'bz;
  assign (pull1, pull0) z_yields = 1'b1;

  wire strong1_vs_weakx;
  assign (strong1, strong0) strong1_vs_weakx = 1'b1;
  assign (weak1, weak0) strong1_vs_weakx = 1'bx;

  wire highz1_one;
  assign (highz1, strong0) highz1_one = 1'b1;

  wire highz1_zero;
  assign (highz1, strong0) highz1_zero = 1'b0;

  wire highz0_zero;
  assign (strong1, highz0) highz0_zero = 1'b0;

  wire highz0_one;
  assign (strong1, highz0) highz0_one = 1'b1;

  wire pu_vs_weak;
  pullup (pu_vs_weak);
  assign (weak1, weak0) pu_vs_weak = 1'b0;

  wire pu_vs_strong;
  pullup (pu_vs_strong);
  assign (strong1, strong0) pu_vs_strong = 1'b0;

  wire pd_vs_weak;
  pulldown (pd_vs_weak);
  assign (weak1, weak0) pd_vs_weak = 1'b1;

  wire pd_vs_strong;
  pulldown (pd_vs_strong);
  assign (strong1, strong0) pd_vs_strong = 1'b1;

  wire pu_only;
  pullup (pu_only);

  wire pd_only;
  pulldown (pd_only);

  wire pu_supply;
  pullup (supply1) (pu_supply);
  assign (strong1, strong0) pu_supply = 1'b0;

  wire g_and;
  and (strong1, weak0) (g_and, 1'b1, 1'b0);

  wire g_fight;
  and (weak1, weak0) (g_fight, 1'b1, 1'b0);
  assign (pull1, pull0) g_fight = 1'b1;

  wire g_not;
  not (weak1, weak0) (g_not, 1'b0);
  assign (pull1, pull0) g_not = 1'b0;

  wire g_buf1;
  buf (g_buf1, 1'b1);

  wire g_bufx;
  buf (strong1, weak0) (g_bufx, 1'bx);

  wire g_or;
  or (pull1, weak0) (g_or, 1'b0, 1'b1, 1'bz);

  wire g_nand;
  nand (g_nand, 1'b1, 1'b1);

  wire g_nor;
  nor (g_nor, 1'b0, 1'b0);

  wire g_xor;
  xor (weak1, weak0) (g_xor, 1'b1, 1'b0);
  assign (pull1, pull0) g_xor = 1'b0;

  wire g_xnor;
  xnor (g_xnor, 1'b1, 1'b0);

  wire [3:0] vec;
  assign (strong1, weak0) vec = 4'b10xz;
  assign (weak1, strong0) vec = 4'b010z;

  wire [3:0] lone;
  assign (pull1, pull0) lone = 4'b1x0z;

  initial begin
    #1;
    $display("UNDRIVEN %b", undriven);
    $display("TRI %b", tri_undriven);
    $display("STRONG %b", strong_over_weak);
    $display("STRONG0 %b", strong0_wins);
    $display("EQUAL %b", equal_x);
    $display("PULL %b", pull_x);
    $display("DEFAULT %b", def_strong);
    $display("SPEC1 %b", spec_one);
    $display("SPEC0 %b", spec_zero);
    $display("XUNEQ %b", x_unequal);
    $display("XEQ %b", x_equal);
    $display("ZDRV %b", z_drive);
    $display("ZYIELD %b", z_yields);
    $display("S1WX %b", strong1_vs_weakx);
    $display("HZ1 %b", highz1_one);
    $display("HZ1Z %b", highz1_zero);
    $display("HZ0 %b", highz0_zero);
    $display("HZ0O %b", highz0_one);
    $display("PUW %b", pu_vs_weak);
    $display("PUS %b", pu_vs_strong);
    $display("PDW %b", pd_vs_weak);
    $display("PDS %b", pd_vs_strong);
    $display("PU %b", pu_only);
    $display("PD %b", pd_only);
    $display("PUSUP %b", pu_supply);
    $display("AND %b", g_and);
    $display("ANDF %b", g_fight);
    $display("NOT %b", g_not);
    $display("BUF1 %b", g_buf1);
    $display("BUFX %b", g_bufx);
    $display("OR %b", g_or);
    $display("NAND %b", g_nand);
    $display("NOR %b", g_nor);
    $display("XOR %b", g_xor);
    $display("XNOR %b", g_xnor);
    $display("VEC %b", vec);
    $display("LONE %b", lone);
  end
endmodule
