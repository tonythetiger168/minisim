// Power gating, isolation and level shifting (IEEE 1801-2024 9.2, 9.5, 9.6,
// 9.8, 9.9): run with gating.upf. A switched core counts; its outputs cross
// to the always-on top through isolation cells (clamp 0, clamp 1, latch)
// and a level shifter; a combinational output re-evaluates at power-up
// while the counter stays x until it is reset.
module core(input logic clk, input logic rst_n, input logic [7:0] d,
            output logic [7:0] cnt, output logic [7:0] q, output logic hi, output logic [7:0] lat);
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) cnt <= 0; else cnt <= cnt + 1;
  assign q = d ^ 8'hf0;
  assign hi = 1'b0;
  assign lat = cnt;
  // processes of a powered-off domain do not run (9.5.3)
  always @(posedge clk) if (cnt == 8'd3) $display("%0t core: cnt is 3", $time);
endmodule
module dut(input logic clk, input logic rst_n, input logic iso_en, input logic sw_en, input logic [7:0] d,
           output logic [7:0] cnt, output logic [7:0] q, output logic hi, output logic [7:0] lat, output logic ack);
  core u_core(.clk, .rst_n, .d, .cnt, .q, .hi, .lat);
endmodule
module tb;
  import UPF::*;
  logic clk = 0, rst_n = 1, iso_en = 0, sw_en = 0, hi, ack;
  logic [7:0] d = 8'h0f, q, cnt, lat;
  dut u_dut(.*);
  always #5 clk = ~clk;
  task show(string what);
    $display("%0t %s: cnt=%h q=%h hi=%b lat=%h ack=%b core.cnt=%h", $time, what, cnt, q, hi, lat, ack, u_dut.u_core.cnt);
  endtask
  initial begin
    #1 show("all off");
    supply_on("VDD", 1.0);
    supply_on("VSS", 0.0);
    #1 show("switch off");
    sw_en = 1;
    #1 show("switch on, not reset");
    rst_n = 0; #1 rst_n = 1;
    #40 show("running");
    iso_en = 1;
    #1 show("isolated");
    sw_en = 0;
    #20 show("core off");
    d = 8'h3c;
    #1 show("input changed while off");
    sw_en = 1;
    #1 show("core on, still isolated");
    iso_en = 0;
    #1 show("isolation off");
    iso_en = 1'bx;
    #1 show("isolation control x");
    iso_en = 0;
    rst_n = 0; #1 rst_n = 1;
    #20 show("reset again");
    supply_off("VDD");
    #1 show("VDD off");
    $finish;
  end
endmodule
