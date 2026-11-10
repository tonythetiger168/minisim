// IEEE 1800-2023 9.3.2, 9.4.3, 8.11: a fork inside a class method still sees
// the method's object (`this`), and wait(0) blocks that branch forever.
//! icarus-skip: Icarus 13 does not support fork in class methods
module top;
  class Closer;
    int fd = 7;
    bit closed;
    function void close();
      fork
        begin closed = 1; $display("closing %0d", fd); end
      join_none
    endfunction
    task park();
      fork
        begin wait (0); $display("never"); end
      join_none
    endtask
  endclass
  Closer c;
  initial begin
    c = new;
    c.close();
    c.park();
    #1 $display("%0d", c.closed);
  end
endmodule

//> closing 7
//> 1
