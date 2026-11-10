// Retention (IEEE 1801-2024 6.53, 9.7): run with retention.upf.
// u_a: single control {ret high} / {ret low}: entering retention saves, the
//      register holds through power-down and shows the saved value at
//      power-up, and runs again when ret falls.
// u_b: dual control, save on the trailing edge of a level, restore on a
//      rising edge, with a save condition on the clock.
// u_c: retention supply switched off while the domain is off: the retained
//      value is lost (RET_SUP_COR).
module cnt8(input logic clk, input logic rst_n, input logic [7:0] step,
            output logic [7:0] keep, output logic [7:0] lose);
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) begin keep <= 0; lose <= 0; end
    else begin keep <= keep + step; lose <= lose + 1; end
endmodule
module tb;
  import UPF::*;
  logic clk = 0, rst_n = 1, ret = 0, save = 0, restore = 0, gclk;
  logic en_a = 0, en_b = 0, en_c = 0;
  logic [7:0] ka, la, kb, lb, kc, lc;
  cnt8 u_a(.clk(clk), .rst_n, .step(8'd1), .keep(ka), .lose(la));
  cnt8 u_b(.clk(gclk), .rst_n, .step(8'd2), .keep(kb), .lose(lb));
  cnt8 u_c(.clk(clk), .rst_n, .step(8'd3), .keep(kc), .lose(lc));
  logic run_b = 1;
  assign gclk = clk & run_b;
  always #5 clk = ~clk;
  task show(string what);
    $display("%0t %s: a=%0d/%0d b=%0d/%0d c=%0d/%0d", $time, what, ka, la, kb, lb, kc, lc);
  endtask
  initial begin
    supply_on("VDD", 1.0); supply_on("VSS", 0.0); supply_on("VRET", 1.0);
    en_a = 1; en_b = 1; en_c = 1;
    #2 rst_n = 0; #1 rst_n = 1;
    #40 show("running");
    // enter retention, stop b's clock, save b
    ret = 1; run_b = 0;
    #1 save = 1; #1 save = 0;
    #10 show("saved");
    en_a = 0; en_b = 0; en_c = 0;
    #10 show("off");
    supply_off("VRET");
    #10 supply_on("VRET", 1.0);
    en_a = 1; en_b = 1; en_c = 1;
    #1 show("power up");
    restore = 1; #1 restore = 0;
    #1 show("b restored");
    ret = 0; run_b = 1;
    #20 show("running again");
    $finish;
  end
endmodule
