// IEEE 1800-2023 29.4-29.10: UDP tables. A combinational UDP gives x for an
// input combination it does not list and reads z as x; a level-sensitive
// latch holds with '-'; an edge-sensitive flip-flop gives x on a transition
// it does not list; level rows win over edge rows (29.10).
primitive mux2 (y, s, a, b);
  output y;
  input s, a, b;
  table
    0 0 ? : 0;
    0 1 ? : 1;
    1 ? 0 : 0;
    1 ? 1 : 1;
    ? 0 0 : 0;
    ? 1 1 : 1;
  endtable
endprimitive
primitive latch (q, en, d);
  output q; reg q;
  input en, d;
  table
    1 0 : ? : 0;
    1 1 : ? : 1;
    0 ? : ? : -;
  endtable
endprimitive
primitive d_edge_ff (q, clock, data);
  output q; reg q;
  input clock, data;
  table
    (01) 0 : ? : 0;
    (01) 1 : ? : 1;
    (0?) 1 : 1 : 1;
    (0?) 0 : 0 : 0;
    (?0) ? : ? : -;
    ? (??) : ? : -;
  endtable
endprimitive
primitive jk_edge_ff (q, clock, j, k, preset, clear);
  output q; reg q;
  input clock, j, k, preset, clear;
  initial q = 0;
  table
    ? ?? 01 : ? : 1;
    ? ?? *1 : 1 : 1;
    ? ?? 10 : ? : 0;
    ? ?? 1* : 0 : 0;
    r 00 00 : 0 : 1;
    r 00 11 : ? : -;
    r 01 11 : ? : 0;
    r 10 11 : ? : 1;
    r 11 11 : 0 : 1;
    r 11 11 : 1 : 0;
    f ?? ?? : ? : -;
    b *? ?? : ? : -;
    b ?* ?? : ? : -;
  endtable
endprimitive
module top;
  reg s, a, b, en, d, clk, j, k, pr, cl;
  wire y, lq, fq, jq;
  mux2 m (y, s, a, b);
  latch l (lq, en, d);
  d_edge_ff f (fq, clk, d);
  jk_edge_ff g (jq, clk, j, k, pr, cl);
  initial begin
    s = 0; a = 1; b = 0; en = 1; d = 0; clk = 0; j = 0; k = 0; pr = 1; cl = 1;
    #1 $display("mux %b", y);
    s = 1; #1 $display("mux %b", y);
    s = 1'bx; #1 $display("mux %b", y);
    a = 0; #1 $display("mux %b", y);
    s = 1'bz; a = 1; #1 $display("mux %b", y);
    d = 1; #1 $display("latch %b", lq);
    en = 0; d = 0; #1 $display("latch %b", lq);
    clk = 1; #1 $display("ff %b", fq);
    d = 1; #1 $display("ff %b", fq);
    clk = 0; #1 clk = 1; #1 $display("ff %b", fq);
    clk = 1'bx; #1 $display("ff %b", fq);
    clk = 0; #1 clk = 1'bx; #1 $display("ff %b", fq);
    clk = 0; pr = 0; cl = 1; #1 $display("jk %b", jq);
    pr = 1; #1 j = 1; k = 1; #1 clk = 1; #1 $display("jk %b", jq);
    clk = 0; #1 clk = 1; #1 $display("jk %b", jq);
    cl = 0; #1 $display("jk %b", jq);
  end
endmodule
//> mux 1
//> mux 0
//> mux x
//> mux 0
//> mux x
//> latch 1
//> latch 1
//> ff 0
//> ff 0
//> ff 1
//> ff x
//> ff x
//> jk 1
//> jk 0
//> jk 1
//> jk 0
