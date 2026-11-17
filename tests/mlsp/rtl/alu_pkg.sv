package alu_pkg;
  typedef enum logic [1:0] {ADD, SUB, AND_OP, OR_OP} op_t;
  function automatic logic [7:0] apply(op_t op, logic [7:0] a, logic [7:0] b);
    case (op)
      ADD: return a + b;
      SUB: return a - b;
      AND_OP: return a & b;
      default: return a | b;
    endcase
  endfunction
endpackage
