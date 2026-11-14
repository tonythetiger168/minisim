// msim -compile: a clock edge runs only the clocks' own cone (combConeK);
// a change of the clock gate's enable from outside runs the whole cone.
module gate (input logic clk_i, en_i, output logic clk_o);
    logic en_latch;
    always_latch if (!clk_i) en_latch = en_i;
    assign clk_o = en_latch & clk_i;
endmodule
module core (input logic clk_i, rst_ni, req_i, output logic [15:0] cnt_o, busy_o);
    logic busy, gclk;
    assign busy = req_i | (cnt_o[2:0] != 0);
    assign busy_o = busy;
    gate g (.clk_i(clk_i), .en_i(busy), .clk_o(gclk));
    always_ff @(posedge gclk or negedge rst_ni)
        if (!rst_ni) cnt_o <= 0; else cnt_o <= cnt_o + 16'd1;
endmodule
module conek;
    logic clk = 0, rst_n = 0, req = 0;
    logic [15:0] cnt;
    logic busy;
    core u (.clk_i(clk), .rst_ni(rst_n), .req_i(req), .cnt_o(cnt), .busy_o(busy));
    always #5 clk = ~clk;
    initial begin
        #12 rst_n = 1;
        #40 req = 1;
        #33 req = 0;
        #200 $display("%0t cnt=%0d busy=%b", $time, cnt, busy);
        #3 req = 1;
        #20 req = 0;
        #100 $display("%0t cnt=%0d busy=%b", $time, cnt, busy);
        $finish;
    end
endmodule
