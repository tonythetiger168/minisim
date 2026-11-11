//! args: -only tidy -top tidy
// The slang-tidy checks, one defect each (mlint rules TIDY001..TIDY021).
package tidy_pkg;
  typedef enum logic {P0, P1} mode;                        //~ TIDY016
  typedef struct packed { logic a; logic b; } pair;        //~ TIDY017
  typedef union packed { logic [1:0] w; logic [1:0] v; } both; //~ TIDY018
endpackage

module tidy_sub(input logic clk, input logic rst, output logic q);
  assign q = clk & rst;
endmodule

module tidy_old(a, y);
  input a;                                                 //~ TIDY011
  output y;                                                //~ TIDY011
  assign y = a;
endmodule

module tidy(input logic clk_i, rst_ni, input logic [7:0] d [0:3], input logic s, output logic [7:0] o);
  import tidy_pkg::*;
  logic a, b, foo, bar, clk, rst, q1, q2, q3, y;
  logic [7:0] flop [0:3];
  logic [1:0] half;                                        //~ TIDY004
  logic arr [4];                                           //~ TIDY004
  logic [3:0] dc = 4'd?;                                   //~ TIDY008
  pair data;
  mode m;
  both u;
  enum logic {E0, E1} raw_e;                               //~ TIDY014
  struct packed { logic f; } raw_s;                        //~ TIDY015
  int cnt;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (~rst_ni) begin
      a <= '0;
      b <= 1'b1;                                           //~ TIDY001
    end else begin
      a <= 1'b1;
    end
  end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    foo <= bar;                                            //~ TIDY002
    if (~rst_ni) cnt <= '0;
    else cnt <= cnt + 1;
  end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    for (int unsigned i = 0; i < 4; ++i) begin             //~ TIDY003
      if (~rst_ni) flop[i] <= '0;
      else flop[i] <= d[i];
    end
  end

  always_comb half[0] = s;

  always_comb begin                                        //~ TIDY013
    data.a = 1'b0;
  end
  always_comb begin                                        //~ TIDY013
    data.b = 1'b1;                                         //~ TIDY005
  end

  always_comb begin                                        //~ TIDY013
    for (int i = 0; i < 4; i++) arr[2'(i)] = s;            //~ TIDY006
  end

  always @(posedge clk_i or posedge s) begin               //~ TIDY007
    bar <= 1'b0;
  end

  tidy_sub u1(.clk(clk), .rst, .q(q1));                    //~ TIDY010
  tidy_sub u2(.clk, .rst(rst), .q(q2));                    //~ TIDY010
  tidy_sub u3(.*, .q(q3));                                 //~ TIDY009
  tidy_old u4(.a(s), .y(y));

  generate                                                 //~ TIDY012
    genvar g;
    for (g = 0; g < 2; g = g + 1) begin : gen_g
      assign o[g] = d[0][g];
    end
  endgenerate
  assign o[7:2] = {m, u.w, data, raw_e, raw_s.f};
  assign {clk, rst, m, u, raw_e, raw_s} = '0;

  covergroup grp @(posedge clk_i);                         //~ TIDY019
    pt: coverpoint a;                                      //~ TIDY020
    pt2: coverpoint b;                                     //~ TIDY020
    xx: cross pt, pt2;                                     //~ TIDY021
  endgroup
endmodule
