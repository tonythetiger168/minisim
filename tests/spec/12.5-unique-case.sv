// IEEE 1800-2023 12.5 Case statement, with 12.5.3 unique-case and default.
module t;
  logic [1:0] s;
  initial begin
    for (s = 0; s < 3; s++)
      unique case (s)
        0: $display("zero");
        1: $display("one");
        default: $display("other");
      endcase
  end
endmodule
//> zero
//> one
//> other
