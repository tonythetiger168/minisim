// IEEE 1800-2023 12.5.4: case inside matches values, ranges and wildcards
// (12.5.4).
//! icarus-skip: Icarus 13 does not support case inside
module top;
  int a;
  logic [3:0] n = 4'b1010;
  initial begin
    for (a = 0; a < 9; a += 4)
      case (a) inside
        [1:6]: $display("%0d in", a);
        8, 9: $display("%0d eight-nine", a);
        default: $display("%0d out", a);
      endcase
    case (n) inside
      4'b1?1?: $display("wild");
      default: $display("no");
    endcase
  end
endmodule

//> 0 out
//> 4 in
//> 8 eight-nine
//> wild
