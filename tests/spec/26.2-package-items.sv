// IEEE 1800-2023 26.2, 26.3: explicit imports (import cfg::apply), a packed
// struct and enum typedef from a package, a package variable a package
// function updates, a pkg::task() call, and a second package.
//! icarus-skip: Icarus 13 does not parse pkg::task() calls or a struct pattern with enum members
package cfg;
  typedef struct packed { logic [3:0] op; logic [7:0] arg; } cmd_t;
  typedef enum logic [1:0] {ADD, SUB, NOP} op_e;
  int calls;
  function automatic logic [7:0] apply(cmd_t c, logic [7:0] acc);
    calls++;
    case (op_e'(c.op))
      ADD: return acc + c.arg;
      SUB: return acc - c.arg;
      default: return acc;
    endcase
  endfunction
  task automatic reset_calls();
    calls = 0;
  endtask
endpackage

package other;
  parameter int SEED = 7;
endpackage

module top;
  import cfg::cmd_t;
  import cfg::apply;
  logic [7:0] acc = 8'd10;
  cmd_t c;
  initial begin
    c = '{op: cfg::ADD, arg: 8'd5};
    acc = apply(c, acc);
    c.op = cfg::SUB; c.arg = other::SEED;
    acc = apply(c, acc);
    $display("%0d %0d", acc, cfg::calls);
    cfg::reset_calls();
    $display("%0d", cfg::calls);
  end
endmodule

//> 8 2
//> 0
