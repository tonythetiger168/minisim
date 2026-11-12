// IEEE 1800-2023 6.21, 13.4.2, 7.4: an unpacked-array local of a task or
// function with an initializer. An automatic one takes the value at every
// call; a static one only before its first use, and then keeps what the
// calls wrote into it.
module top;
  function automatic logic [3:0] pick(int k);
    logic [3:0] tbl [4] = '{4'h3, 4'h5, 4'h9, 4'hc};
    tbl[k] = tbl[k] + 1;            // lost at the next call
    return tbl[k];
  endfunction
  function int bump(int k);
    static int cnt [0:2] = '{10, 20, 30};
    cnt[k] += 1;
    return cnt[k];
  endfunction
  task automatic show(input int base);
    byte b [2] = '{8'(base), 8'(base + 1)};
    $display("show %0d %0d", b[0], b[1]);
  endtask
  initial begin
    $display("pick %h %h %h", pick(0), pick(0), pick(3));
    $display("bump %0d %0d %0d", bump(1), bump(1), bump(2));
    show(7);
    show(40);
  end
endmodule
//> pick 4 4 d
//> bump 21 22 31
//> show 7 8
//> show 40 41
