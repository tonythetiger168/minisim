// A 2x2 mesh of one tile module under one top instance, built by nested
// generate loops (soc.row[r].col[c].t, as bench/gen_manycore.py). The tiles
// are the instances that share one copy of the code (codegen.hpp instKey):
// the first name component (soc) would make the whole mesh one instance.
module mtile #(parameter logic [7:0] SEED = 8'h01) (input logic clk, input logic [7:0] in, output logic [7:0] out);
  logic [7:0] acc = SEED, dec;
  logic [1:0] st = 0;
  always_comb begin
    case (st)
      2'd0: dec = acc + in;
      2'd1: dec = acc ^ in;
      2'd2: dec = {acc[6:0], acc[7]};
      default: dec = ~acc;
    endcase
  end
  always @(posedge clk) begin
    acc <= dec;
    st <= st + 2'd1;
  end
  assign out = acc ^ 8'h33;
endmodule

module mesh (input logic clk, output logic [7:0] sum);
  logic [7:0] o [2][2];
  for (genvar r = 0; r < 2; r++) begin : row
    for (genvar c = 0; c < 2; c++) begin : col
      mtile #(8'(r * 2 + c + 1)) t (clk, o[1 - r][1 - c], o[r][c]);
    end
  end
  assign sum = o[0][0] ^ o[0][1] ^ o[1][0] ^ o[1][1];
endmodule

module meshshare;
  logic clk = 0;
  logic [7:0] sum;
  always #5 clk = ~clk;
  mesh soc (clk, sum);
  initial begin
    for (int i = 0; i < 12; i++) begin
      @(negedge clk);
      $display("%0t sum=%h t00=%h t11=%h", $time, sum, soc.o[0][0], soc.o[1][1]);
    end
    $finish;
  end
endmodule
