// IEEE 1800-2023 14.3, 14.13, 14.16, 25.9: a clockvar that is an unpacked
// array. Its input samples every element; a synchronous drive writes the
// whole array or one element (a constant or a variable index), directly or
// through a virtual interface, onto a variable or a net array. A plain
// array member of an interface is read and written through a virtual
// interface element by element.
//! icarus-skip: Icarus 13 does not support clocking blocks
interface lanes_if(input logic clk);
  logic [7:0] tx [4];
  wire  [3:0] pins [2];
  logic [3:0] pin_drv [2];
  int         tag [0:2];
  clocking dcb @(posedge clk);
    output tx;
    output pins;
  endclocking
  clocking mcb @(posedge clk);
    input tx;
  endclocking
endinterface

class lane_drv;
  virtual lanes_if v;
  function new(virtual lanes_if v); this.v = v; endfunction
  task run();
    int k = 2;
    @(v.dcb);
    v.dcb.tx <= '{8'h11, 8'h22, 8'h33, 8'h44};
    @(v.dcb);
    v.dcb.tx[k] <= 8'hc3;
    v.dcb.pins[1] <= 4'h9;
    @(v.dcb);
    $display("mon %h %h %h %h", v.mcb.tx[0], v.mcb.tx[1], v.mcb.tx[2], v.mcb.tx[3]);
    v.tag[1] = 42;
    v.tag[k] = v.tag[1] + 1;
    $display("tag %0d %0d %0d", v.tag[0], v.tag[1], v.tag[2]);
  endtask
endclass

module top;
  logic clk = 0;
  always #5 clk = ~clk;
  lanes_if u(clk);
  logic [7:0] q [2];
  default clocking cb @(posedge clk);
    output q;
  endclocking
  initial begin
    static lane_drv d = new(u);
    ##1;
    cb.q <= '{8'h5a, 8'ha5};
    ##1;
    $display("q %h %h", q[0], q[1]);
    cb.q[1] <= 8'hff;
    ##1;
    $display("q %h %h", q[0], q[1]);
    d.run();
    $display("tx %h %h %h %h", u.tx[0], u.tx[1], u.tx[2], u.tx[3]);
    $display("pins %h %b", u.pins[1], u.pins[0]);
    $finish;
  end
endmodule
//> q 5a a5
//> q 5a ff
//> mon 11 22 c3 44
//> tag 0 42 43
//> tx 11 22 c3 44
//> pins 9 zzzz
