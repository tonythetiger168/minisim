//! args: -only NAME015,NAME016,NAME017,SUS002,SUS003,STY010,STY011,STY012 -top src2
`define uvm_info(a, b, c) $display(a, b);
interface bus; //~ NAME015
  logic v;
endinterface
interface bus_if;
  logic v;
endinterface
class item;
  rand int a;
  constraint lo { a < 4; } //~ NAME016
  constraint big_c { a > 1; }
endclass
module src2 #(
    parameter bit DisableCheck = 0, //~ NAME017
    parameter bit EnableCheck = 1
) (
    input logic clk
);
  item it;
  int r;
  string s;
  initial begin : main
    it = new;
    void'(it.randomize()); //~ SUS002
    if (!it.randomize()) $display("failed");
    void'($value$plusargs("r=%d", r));
    r = $random; //~ SUS003
    r = $urandom;
    s = $psprintf("%d", r); //~ SUS003
    //~v STY010
    s = "one \
two";
    `uvm_info("a", "b", 0); //~ STY011
    `uvm_info("a", "b", 0)
    fork : workers
      #1;
    join_none
    disable workers; //~ STY012
    disable fork;
    begin : inner
      if (r == 0) disable inner;
      if (r == 1) disable main;
    end
  end
endmodule
