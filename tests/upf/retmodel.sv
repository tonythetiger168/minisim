// map_retention_cell -lib_model_name (IEEE 1801-2024 6.39): run with
// retmodel.upf. u_a.keep is simulated by the retention model ret_ff, wired
// to the register's canonical clock, data and async reset and to the
// strategy's save signal; u_a.lose keeps the RTL with corruption.
module cnt8(input logic clk, input logic rst_n, input logic [7:0] step,
            output logic [7:0] keep, output logic [7:0] lose);
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) begin keep <= 0; lose <= 0; end
    else begin keep <= keep + step; lose <= lose + 1; end
endmodule
// A retention flop: holds while RET is high; x when its own supply is off.
module ret_ff(input logic CK, input logic [7:0] D, input logic AR, input logic RET, input logic VDDC,
              output logic [7:0] Q);
  always @(posedge CK or posedge AR or negedge VDDC)
    if (!VDDC) Q <= 'x;
    else if (AR) Q <= '0;
    else if (!RET) Q <= D;
endmodule
module tb;
  import UPF::*;
  logic clk = 0, rst_n = 1, ret = 0, en_a = 0;
  logic [7:0] ka, la;
  cnt8 u_a(.clk(clk), .rst_n, .step(8'd1), .keep(ka), .lose(la));
  always #5 clk = ~clk;
  task show(string what);
    $display("%0t %s: keep=%0d lose=%0d", $time, what, ka, la);
  endtask
  initial begin
    supply_on("VDD", 1.0); supply_on("VSS", 0.0); supply_on("VRET", 1.0);
    en_a = 1;
    #2 rst_n = 0; #1 rst_n = 1;
    #40 show("running");
    ret = 1;
    #10 show("held");
    en_a = 0;
    #10 show("off");
    supply_off("VRET");
    #10 show("model supply off");
    supply_on("VRET", 1.0);
    en_a = 1;
    #1 rst_n = 0; #1 rst_n = 1;
    #1 show("power up, reset");
    ret = 0;
    #20 show("running again");
    $finish;
  end
endmodule
