// IEEE 1800-2023 6.23: type references compare with == / != / === / !==
// and select a case item; both are known at elaboration.
module t #(parameter type T = logic [11:0]);
  initial begin
    case (type(T))
      type(logic [12:0]): $display("13 bits");
      type(logic [11:0]): $display("12 bits");
      default: $display("other");
    endcase
    $display("%0d %0d %0d %0d", type(T) == type(logic [11:0]), type(T) != type(int),
             type(T) === type(logic [12:0]), type(T) !== type(logic [11:0]));
  end
endmodule
//> 12 bits
//> 1 1 0 0
//! icarus-skip: Icarus has no type operator
