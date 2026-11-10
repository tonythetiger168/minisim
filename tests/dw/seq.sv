// Sequential blocks: FIFOs (one and two clocks), RAM, counters, LFSR, CRC,
// synchronizer and arbiters against queue and loop models.
//! args: -y ../../dw +libext+.sv -top tb
//> fifo_sync ok
//> fifo_async ok
//> ram ok
//> counters ok
//> crc ok
//> arbiters ok
//> errors=0
module fifo_sync_chk #(parameter int DEPTH = 5) (input logic clk, input logic rst_n, output int errs, output logic fin);
    localparam int CW = $clog2(DEPTH + 1);
    localparam int AF = (DEPTH > 2) ? 2 : 1;
    logic push, pop, empty, full, ae, af, error;
    logic [7:0] din, dout;
    logic [CW-1:0] count;
    MW_fifo_sync #(.WIDTH(8), .DEPTH(DEPTH), .AF_LEVEL(AF), .AE_LEVEL(1)) u (
        .clk, .rst_n, .push, .data_in(din), .pop, .data_out(dout), .empty, .full,
        .almost_empty(ae), .almost_full(af), .count, .error);
    logic [7:0] q [$];
    int phase;
    initial begin
        errs = 0; fin = 0; push = 0; pop = 0; din = 0;
        @(posedge rst_n);
        for (int i = 0; i < 3000; i++) begin
            @(negedge clk);
            // the flags describe the queue as it stands
            if (count !== CW'(q.size()) || empty !== (q.size() == 0) || full !== (q.size() == DEPTH) ||
                ae !== (q.size() <= 1) || af !== (q.size() >= DEPTH - AF)) begin
                errs++;
                if (errs < 4) $display("fifo_sync D=%0d flags: count=%0d size=%0d e=%b f=%b", DEPTH, count, q.size(), empty, full);
            end
            if (q.size() > 0 && dout !== q[0]) begin
                errs++;
                if (errs < 4) $display("fifo_sync D=%0d data %h, want %h", DEPTH, dout, q[0]);
            end
            // fill, drain, then mix
            phase = (i / 500) % 3;
            push = (phase == 0) ? ($urandom_range(0, 9) < 8) : (phase == 1) ? ($urandom_range(0, 9) < 2) : $urandom;
            pop  = (phase == 0) ? ($urandom_range(0, 9) < 2) : (phase == 1) ? ($urandom_range(0, 9) < 8) : $urandom;
            din  = $urandom;
            #1;
            if (error !== ((push && q.size() == DEPTH) || (pop && q.size() == 0))) errs++;
            // model: a pop and a push in one clock both happen when legal
            if (pop && q.size() > 0) begin
                if (push && q.size() < DEPTH) begin void'(q.pop_front()); q.push_back(din); end
                else void'(q.pop_front());
            end else if (push && q.size() < DEPTH) q.push_back(din);
        end
        fin = 1;
    end
endmodule

module fifo_async_chk #(parameter int WP = 7, parameter int RP = 11) (input logic rst_n, output int errs, output logic fin);
    logic wclk = 0, rclk = 0;
    always #(WP) wclk = ~wclk;
    always #(RP) rclk = ~rclk;
    logic push, pop, full, empty;
    logic [15:0] din, dout;
    MW_fifo_async #(.WIDTH(16), .DEPTH(8)) u (
        .wr_clk(wclk), .wr_rst_n(rst_n), .push, .data_in(din), .full,
        .rd_clk(rclk), .rd_rst_n(rst_n), .pop, .data_out(dout), .empty);
    logic [15:0] q [$];
    int sent, got, maxfill;
    logic wdone;
    // writer: every accepted word goes in the model queue
    initial begin
        push = 0; din = 0; sent = 0; wdone = 0;
        @(posedge rst_n);
        while (sent < 400) begin
            @(negedge wclk);
            push = ($urandom_range(0, 9) < (sent < 200 ? 9 : 3));
            din = 16'(sent * 7 + 3);
            #1 if (push && !full) begin q.push_back(din); sent++; end
        end
        @(negedge wclk) push = 0;
        wdone = 1;
    end
    // reader: words come out in order, none lost or repeated
    initial begin
        errs = 0; fin = 0; pop = 0; got = 0; maxfill = 0;
        @(posedge rst_n);
        while (got < 400) begin
            @(negedge rclk);
            pop = ($urandom_range(0, 9) < (got < 200 ? 3 : 9));
            #1 if (pop && !empty) begin
                if (q.size() == 0 || dout !== q[0]) begin
                    errs++;
                    if (errs < 4) $display("fifo_async %0d/%0d: got %h at word %0d", WP, RP, dout, got);
                end
                if (q.size() > 0) void'(q.pop_front());
                got++;
            end
            if (q.size() > maxfill) maxfill = q.size();
        end
        @(negedge rclk) pop = 0;
        repeat (6) @(negedge rclk);
        if (!empty || q.size() != 0) errs++;
        if (maxfill > 8) errs++;                // never held more than DEPTH
        if (maxfill < 7) errs++;                // and the slow reader did back it up
        fin = 1;
    end
endmodule

module misc_chk (input logic clk, input logic rst_n, output int e_ram, output int e_cnt, output int e_crc, output int e_arb, output logic fin);
    // RAM, both read modes
    logic we, re;
    logic [3:0] wa, ra;
    logic [7:0] wd, rd_a, rd_s, model [12], last_s;
    MW_ram #(.WIDTH(8), .DEPTH(12), .SYNC_READ(0)) u_ra (.clk, .wr_en(we), .wr_addr(wa), .wr_data(wd), .rd_en(re), .rd_addr(ra), .rd_data(rd_a));
    MW_ram #(.WIDTH(8), .DEPTH(12), .SYNC_READ(1)) u_rs (.clk, .wr_en(we), .wr_addr(wa), .wr_data(wd), .rd_en(re), .rd_addr(ra), .rd_data(rd_s));
    // counters
    logic load, en, up, ter;
    logic [4:0] cdata, cnt, cmodel, gray, gbin, gprev;
    MW_counter #(.WIDTH(5)) u_cnt (.clk, .rst_n, .load, .data(cdata), .en, .up_dn(up), .count(cnt), .tercnt(ter));
    MW_gray_counter #(.WIDTH(5)) u_gc (.clk, .rst_n, .en, .count(gray), .bin(gbin));
    logic [7:0] lf;
    MW_lfsr #(.WIDTH(8)) u_lfsr (.clk, .rst_n, .load(1'b0), .data(8'd0), .en(1'b1), .q(lf));
    // synchronizer: 3 stages
    logic sd, sq;
    logic [2:0] spipe;
    MW_sync #(.WIDTH(1), .STAGES(3)) u_sync (.clk, .rst_n, .d(sd), .q(sq));
    // CRCs with known check values over "123456789"
    logic cinit, cen;
    logic [7:0] cbyte;
    logic [15:0] crc16;
    logic [31:0] crc32, crc32w;
    logic [31:0] cword;
    MW_crc #(.WIDTH(16), .DATA_WIDTH(8), .POLY(16'h1021), .INIT(16'hFFFF)) u_c16 (.clk, .rst_n, .init(cinit), .en(cen), .data(cbyte), .crc(crc16));
    MW_crc #(.WIDTH(32), .DATA_WIDTH(8), .POLY(32'h04C11DB7), .INIT(32'hFFFFFFFF), .XOROUT(32'hFFFFFFFF), .REFLECT(1))
        u_c32 (.clk, .rst_n, .init(cinit), .en(cen), .data(cbyte), .crc(crc32));
    // the same CRC-32, four bytes a clock
    logic wen;
    MW_crc #(.WIDTH(32), .DATA_WIDTH(32), .POLY(32'h04C11DB7), .INIT(32'hFFFFFFFF), .XOROUT(32'hFFFFFFFF), .REFLECT(1))
        u_c32w (.clk, .rst_n, .init(cinit), .en(wen), .data(cword), .crc(crc32w));
    // arbiters
    logic [5:0] req, g_fp, g_rr;
    logic [2:0] i_fp, i_rr;
    logic v_fp, v_rr, arb_en;
    MW_arb_fp #(.NUM(6)) u_fp (.request(req), .grant(g_fp), .index(i_fp), .granted(v_fp));
    MW_arb_rr #(.NUM(6)) u_rr (.clk, .rst_n, .en(arb_en), .request(req), .grant(g_rr), .index(i_rr), .granted(v_rr));

    string msg = "123456789";
    int seen [256];
    int last, want, served [6];
    initial begin
        e_ram = 0; e_cnt = 0; e_crc = 0; e_arb = 0; fin = 0;
        we = 0; re = 0; wa = 0; ra = 0; wd = 0; load = 0; en = 0; up = 1; cdata = 0; sd = 0;
        cinit = 0; cen = 0; wen = 0; cbyte = 0; cword = 0; req = 0; arb_en = 0;
        @(posedge rst_n);
        // ---- RAM
        for (int i = 0; i < 12; i++) begin
            @(negedge clk); we = 1; wa = 4'(i); wd = 8'(i * 3); model[i] = 8'(i * 3);
        end
        last_s = 'x;
        for (int i = 0; i < 300; i++) begin
            @(negedge clk);
            we = $urandom; re = $urandom;
            wa = 4'($urandom_range(0, 11)); ra = 4'($urandom_range(0, 11));
            wd = $urandom;
            #1 if (rd_a !== model[ra]) e_ram++;
            @(posedge clk);
            if (re) last_s = model[ra];            // the old word, even when written this clock
            if (we) model[wa] = wd;
            #1 if (re && rd_s !== last_s) e_ram++;
            if (!re && last_s !== 'x && rd_s !== last_s) e_ram++;
        end
        we = 0;
        // ---- counters, LFSR, synchronizer
        cmodel = cnt;
        gprev = gray;
        spipe = '0;
        for (int i = 0; i < 400; i++) begin
            @(negedge clk);
            if (cnt !== cmodel || ter !== (up ? cmodel == 5'd31 : cmodel == 5'd0)) begin
                e_cnt++;
                if (e_cnt < 4) $display("counter %0d, want %0d", cnt, cmodel);
            end
            if (gray !== (gbin ^ (gbin >> 1))) e_cnt++;
            if (i > 0 && en && $countones(gray ^ gprev) > 1) e_cnt++;
            if (sq !== spipe[2]) e_cnt++;
            gprev = gray;
            load = ($urandom_range(0, 19) == 0);
            en = ($urandom_range(0, 9) < 8);
            if (i % 70 == 0) up = ~up;
            cdata = $urandom;
            sd = $urandom;
            @(posedge clk);
            if (load) cmodel = cdata; else if (en) cmodel = up ? cmodel + 1'b1 : cmodel - 1'b1;
            spipe = {spipe[1:0], sd};
        end
        // the default polynomial visits all 255 non-zero states
        for (int i = 0; i < 256; i++) seen[i] = 0;
        for (int i = 0; i < 255; i++) begin
            @(negedge clk);
            seen[lf]++;
        end
        for (int i = 1; i < 256; i++) if (seen[i] != 1) e_cnt++;
        if (seen[0] != 0) e_cnt++;
        // ---- CRC
        @(negedge clk) cinit = 1;
        @(negedge clk) cinit = 0;
        for (int i = 0; i < 9; i++) begin
            cen = 1; cbyte = msg[i];
            @(negedge clk);
        end
        cen = 0;
        if (crc16 !== 16'h29B1) begin e_crc++; $display("crc16 %h, want 29b1", crc16); end
        if (crc32 !== 32'hCBF43926) begin e_crc++; $display("crc32 %h, want cbf43926", crc32); end
        // "12345678" as two little-endian words gives the same as eight bytes
        @(negedge clk) cinit = 1;
        @(negedge clk) cinit = 0;
        for (int i = 0; i < 8; i++) begin
            cen = 1; cbyte = msg[i];
            wen = (i % 4 == 3);
            cword = {cbyte, cword[31:8]};
            @(negedge clk);
        end
        cen = 0; wen = 0;
        if (crc32w !== crc32 || crc32 !== 32'h9AE0DAAF) begin e_crc++; $display("crc32 wide %h, bytes %h, want 9ae0daaf", crc32w, crc32); end
        // ---- arbiters
        last = 5;                                 // reset state: position 0 is first in line
        for (int i = 0; i < 6; i++) served[i] = 0;
        for (int i = 0; i < 600; i++) begin
            @(negedge clk);
            req = (i < 300) ? 6'($urandom) : 6'b111111;
            if (i % 13 == 0) req = '0;
            arb_en = (i % 5 != 0);
            #1;
            // fixed priority: the lowest request
            want = -1;
            for (int k = 5; k >= 0; k--) if (req[k]) want = k;
            if (v_fp !== (req != 0) || g_fp !== (want < 0 ? 6'd0 : 6'(1 << want)) || (want >= 0 && i_fp !== 3'(want))) begin
                e_arb++;
                if (e_arb < 4) $display("arb_fp req=%b grant=%b idx=%0d want %0d", req, g_fp, i_fp, want);
            end
            // round robin: the first request after the last winner
            want = -1;
            for (int k = 6; k >= 1; k--) if (req[(last + k) % 6]) want = (last + k) % 6;
            if (v_rr !== (req != 0) || g_rr !== (want < 0 ? 6'd0 : 6'(1 << want)) || (want >= 0 && i_rr !== 3'(want))) begin
                e_arb++;
                if (e_arb < 4) $display("arb_rr req=%b last=%0d grant=%b want %0d", req, last, g_rr, want);
            end
            if (arb_en && want >= 0) begin
                last = want;
                if (i >= 300) served[want]++;
            end
        end
        // with every request up, the grants are shared evenly
        for (int i = 1; i < 6; i++)
            if (served[i] - served[0] > 1 || served[0] - served[i] > 1 || served[i] < 30) begin
                e_arb++;
                $display("arb_rr served[%0d]=%0d, [0]=%0d", i, served[i], served[0]);
            end
        fin = 1;
    end
endmodule

module tb;
    logic clk = 0, rst_n = 0;
    always #5 clk = ~clk;
    int ef [3], ea [3], e_ram, e_cnt, e_crc, e_arb;
    logic ff [3], fa [3], fm;
    fifo_sync_chk #(.DEPTH(1)) u_f0 (.clk, .rst_n, .errs(ef[0]), .fin(ff[0]));
    fifo_sync_chk #(.DEPTH(5)) u_f1 (.clk, .rst_n, .errs(ef[1]), .fin(ff[1]));
    fifo_sync_chk #(.DEPTH(16)) u_f2 (.clk, .rst_n, .errs(ef[2]), .fin(ff[2]));
    fifo_async_chk #(.WP(7), .RP(11)) u_a0 (.rst_n, .errs(ea[0]), .fin(fa[0]));
    fifo_async_chk #(.WP(13), .RP(5)) u_a1 (.rst_n, .errs(ea[1]), .fin(fa[1]));
    fifo_async_chk #(.WP(6), .RP(6)) u_a2 (.rst_n, .errs(ea[2]), .fin(fa[2]));
    misc_chk u_m (.clk, .rst_n, .e_ram, .e_cnt, .e_crc, .e_arb, .fin(fm));
    int tot, t;
    initial begin
        #12 rst_n = 1;
        wait (ff[0] && ff[1] && ff[2] && fa[0] && fa[1] && fa[2] && fm);
        tot = 0;
        t = ef[0] + ef[1] + ef[2];
        if (t == 0) $display("fifo_sync ok"); tot += t;
        t = ea[0] + ea[1] + ea[2];
        if (t == 0) $display("fifo_async ok"); tot += t;
        if (e_ram == 0) $display("ram ok");
        if (e_cnt == 0) $display("counters ok");
        if (e_crc == 0) $display("crc ok");
        if (e_arb == 0) $display("arbiters ok");
        tot += e_ram + e_cnt + e_crc + e_arb;
        $display("errors=%0d", tot);
        $finish;
    end
    initial begin
        #5000000 $display("timeout %b %b %b %b %b %b %b", ff[0], ff[1], ff[2], fa[0], fa[1], fa[2], fm);
        $finish;
    end
endmodule
