// set_retention -applies_to flop / latch (IEEE 1801-2024 6.54): run with
// retlatch.upf. RET_F keeps the flip-flop f, RET_L the latches l1
// (always_latch) and l2 (inferred: always @* that assigns it only when en
// is high); c is combinational and is retained by neither.
module blk(input logic clk, input logic en, input logic [3:0] d,
           output logic [3:0] f, output logic [3:0] l1, output logic [3:0] l2, output logic [3:0] c);
  always_ff @(posedge clk) f <= d;
  always_latch if (en) l1 <= d;
  always @* if (en) l2 = d;
  always @* c = en ? d : 4'd0;
endmodule
module tb;
  import UPF::*;
  logic clk = 0, en = 0, sf = 0, sl = 0, on = 0;
  logic [3:0] d = 0, f, l1, l2, c;
  blk u_a(.clk, .en, .d, .f, .l1, .l2, .c);
  task show(string what);
    $display("%0t %s: f=%h l1=%h l2=%h c=%h", $time, what, f, l1, l2, c);
  endtask
  task cycle(logic [3:0] v);
    d = v; en = 1;
    #1 clk = 1; #1 clk = 0; en = 0;
  endtask
  initial begin
    supply_on("VDD", 1.0); supply_on("VSS", 0.0);
    on = 1;
    #1 cycle(4'h5);
    #1 show("loaded 5");
    sf = 1; #1 on = 0;
    #1 show("off, flops saved");
    on = 1; #1 sf = 0;
    #1 show("on, flops restored");
    cycle(4'h9);
    #1 show("loaded 9");
    sl = 1; #1 on = 0;
    #1 show("off, latches saved");
    on = 1; #1 sl = 0;
    #1 show("on, latches restored");
    $finish;
  end
endmodule
