// IEEE 1800-2023 33.4.3: a config's use clause can set parameters of an
// instance, by name, from the config's localparams or the design's own
// parameters; use #(.W()) puts one parameter back to its default, use #()
// all of them; and a config override wins over a defparam of the same
// parameter. Examples 3 to 8 of 33.4.3, as one config with several tops
// (top9 is Example 2's top under Example 7's config).
//! icarus-skip: Icarus 13 does not support configurations
module adder #(parameter ID = "id",
                         W = 8,
                         D = 512)
              ();
  initial $display("%m: ID = %s, W = %0d, D = %0d", ID, W, D);
endmodule: adder
module top ();
  parameter WIDTH = 16;
  adder a1 ();
endmodule
module top4 ();
  parameter S = 16;
  adder #(.ID("a1")) a1 ();
  adder #(.ID("a2")) a2 ();
  adder #(.ID("a3")) a3 ();
  adder #(.ID("a4")) a4 ();
endmodule
module top5 ();
  parameter WIDTH = 64, DEPTH = 1024, ID = "A1";
  adder #(.ID(ID), .W(WIDTH), .D(DEPTH)) a1 ();
endmodule
module top9 ();
  adder #(.ID("x"), .W(1), .D(2)) a1 ();
endmodule
module test;
  top8 t ();
  defparam t.WIDTH = 64;
  defparam t.a1.W = 16;
endmodule
module top8 ();
  parameter WIDTH = 32;
  adder #(.ID("a1")) a1 ();
  adder #(.ID("a2"), .W(WIDTH)) a2 ();
endmodule

config cfg;
  localparam S = 24;
  design work.top work.top4 work.top5 work.top9 work.test;
  instance top use #(.WIDTH(32));
  instance top.a1 use #(.W(top.WIDTH));   // Example 3: W = 32
  instance top4.a1 use #(.W(top4.S));     // Example 4: the design's S, 16
  instance top4.a2 use #(.W(S));          // the config's S, 24
  instance top5.a1 use #(.W());           // Example 6: W back to its default
  instance top9.a1 use #();               // Example 7: every parameter back
  instance test.t use #(.WIDTH(48));      // Example 8: wins over defparam t.WIDTH
endconfig

//> test.t.a1: ID = a1, W = 16, D = 512
//> test.t.a2: ID = a2, W = 48, D = 512
//> top.a1: ID = id, W = 32, D = 512
//> top4.a1: ID = a1, W = 16, D = 512
//> top4.a2: ID = a2, W = 24, D = 512
//> top4.a3: ID = a3, W = 8, D = 512
//> top4.a4: ID = a4, W = 8, D = 512
//> top5.a1: ID = A1, W = 8, D = 1024
//> top9.a1: ID = id, W = 8, D = 512
