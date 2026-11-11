//! args: -only case -top cases
module cases(input logic [1:0] a, input logic clk, output logic [3:0] y, z, w, v, q);
  typedef enum logic [1:0] {A, B, C} e_t;
  e_t st;
  assign st = e_t'(a);
  always_comb begin
    y = 4'd0;
    case (a)                      //~ CASE001
      2'd0: y = 4'd1;
      2'd1: y = 4'd2;
    endcase
  end
  always_comb begin
    case (a)                      // every value: full
      2'd0: z = 4'd1;
      2'd1: z = 4'd2;
      2'd2: z = 4'd3;
      2'd3: z = 4'd4;
    endcase
  end
  always_comb begin
    unique case (st)              // unique says the rest cannot happen
      A: w = 4'd1;
      B: w = 4'd2;
    endcase
  end
  always_comb begin
    casex (a)                     //~ CASE002
      2'b1x: v = 4'd1;
      default: v = 4'd0;
    endcase
    case (a)
      2'b1x: v = 4'd2;            //~ CASE003
      2'd1: v = 4'd3;
      2'd1: v = 4'd4;             //~ CASE004
      default: v = 4'd5;
    endcase
    casez (a)
      2'b1?: v = 4'd6;
      default: ;
    endcase
  end
  always_ff @(posedge clk)
    case (a)                      // in a flop the value is held anyway
      2'd0: q <= 4'd1;
    endcase
endmodule
