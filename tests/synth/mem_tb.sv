module mem_tb;
    logic clk = 0, we, wa, wb;
    logic [3:0] waddr, raddr, da, db, rq;
    logic [7:0] wdata, rdata;
    logic [1:0] be;
    logic [2:0] baddr, aa, ab, ra;
    logic [15:0] bdata, bq;
    mem dut (.*);
    always #5 clk = ~clk;
    int seed = 7;
    initial begin
        // fill every word first, so that no output depends on power-up state
        for (int i = 0; i < 16; i++) begin
            we = 1; waddr = i[3:0]; wdata = 8'(i * 7); raddr = 0;
            be = 2'b11; baddr = i[2:0]; bdata = 16'(i * 1001);
            wa = 1; wb = 0; aa = i[2:0]; ab = 0; da = i[3:0]; db = 0; ra = 0;
            @(posedge clk); #1;
        end
        for (int i = 0; i < 400; i++) begin
            we = $random(seed); waddr = $random(seed); raddr = $random(seed); wdata = $random(seed);
            be = $random(seed); baddr = $random(seed); bdata = $random(seed);
            wa = $random(seed); wb = $random(seed); aa = $random(seed); ab = $random(seed);
            da = $random(seed); db = $random(seed); ra = $random(seed);
            @(posedge clk); #1;
            $display("%0d %h %h %h", i, rdata, bq, rq);
        end
        $finish;
    end
endmodule
