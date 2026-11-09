// IEEE 1800-2023 8.19: const class properties. A global constant has an
// initial value; an instance constant is assigned once in the constructor.
module top;
  class Jumbo;
    const int max_size = 9 * 1024;
    const int id;
    static const int version = 3;
    function new(int i); id = i; endfunction
  endclass
  Jumbo j;
  initial begin
    j = new(4);
    $display("%0d %0d %0d", j.max_size, j.id, Jumbo::version);
  end
endmodule

//> 9216 4 3
