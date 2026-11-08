// IEEE 1800-2023 26.2, 26.3: a package with a parameter, localparam,
// typedefs, an enum, a variable with an initializer, a function and a task;
// a wildcard import, and pkg::name references.
package pk;
  parameter int W = 8;
  localparam int TWICE = W * 2;
  typedef logic [W-1:0] word_t;
  typedef enum logic [1:0] {IDLE, RUN, DONE} state_t;
  int counter = 5;
  function automatic int add1(int x);
    return x + 1;
  endfunction
  task automatic bump();
    counter++;
  endtask
endpackage

module top;
  import pk::*;
  word_t w;
  state_t s;
  initial begin
    w = 8'hA5;
    s = RUN;
    $display("%0d %0d %h %0d", W, pk::TWICE, w, s);
    $display("%0d", add1(41));
    $display("%0d", pk::counter);
    bump();
    $display("%0d", counter);
  end
endmodule

//> 8 16 a5 1
//> 42
//> 5
//> 6
