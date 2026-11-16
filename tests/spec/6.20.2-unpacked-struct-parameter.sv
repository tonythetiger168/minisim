// IEEE 1800-2023 6.20.2, 7.2, 23.2.3: parameters and localparams of an
// unpacked struct type, overridden per instance, read whole and by member at
// run time; an unpacked array parameter of structs indexed at run time.
//! icarus-skip: Icarus does not support unpacked struct parameters
typedef struct { bit en; int w; } cfg_t;
typedef struct { cfg_t c; string name; real r; } mix_t;
module leaf #(parameter cfg_t C = '{1'b0, 0}) ();
  initial #1 $display("leaf en=%0d w=%0d %p", C.en, C.w, C);
endmodule
module t;
  localparam mix_t M = '{c: '{1'b1, 7}, name: "big", r: 2.5};
  localparam cfg_t TAB[3] = '{'{1'b0, 10}, '{1'b1, 20}, '{1'b0, 30}};
  leaf #(.C('{1'b1, 13})) u1 ();
  leaf u2 ();
  int i;
  initial begin
    $display("%0d %0d %s %0.1f", M.c.en, M.c.w, M.name, M.r);
    for (i = 0; i < 3; i++) $display("TAB[%0d] %0d %0d", i, TAB[i].en, TAB[i].w);
    $display("%0d", TAB[1].w + M.c.w);
  end
endmodule
//> 1 7 big 2.5
//> TAB[0] 0 10
//> TAB[1] 1 20
//> TAB[2] 0 30
//> 27
//> leaf en=1 w=13 '{en:1, w:13}
//> leaf en=0 w=0 '{en:0, w:0}
