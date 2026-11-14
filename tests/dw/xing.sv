// MW_reset_sync, MW_pulse_sync, MW_data_sync across two clocks;
// MW_8b10b_enc / MW_8b10b_dec over every character and code; MW_sincos
// against $sin/$cos; MW_fir against a delay-line model; MW_tap through
// IDCODE, IR and BYPASS scans.
//! args: -y ../../dw +libext+.sv -top tb
//> reset_sync ok
//> pulse_sync ok
//> data_sync ok
//> 8b10b ok
//> sincos ok
//> fir ok
//> tap ok
//> errors=0
module sc_chk #(parameter int W = 16) (input logic go, output int errs, output logic done = 0);
    logic [W-1:0] a;
    logic signed [W-1:0] s, c;
    MW_sincos #(.WIDTH(W)) u (.a, .sin(s), .cos(c));
    always @(posedge go) begin
        real ang, fs, ws, wc;
        errs = 0;
        fs = (2.0 ** (W - 1)) - 1.0;
        for (int i = 0; i < 3000; i++) begin
            a = W'($urandom);
            if (i < 8) a = W'(i) << (W - 3);           // multiples of 45 degrees
            #1;
            ang = real'(a) / (2.0 ** W) * 2.0 * 3.14159265358979323846;
            ws = $sin(ang) * fs;
            wc = $cos(ang) * fs;
            if ((real'(s) - ws) > 2.0 || (ws - real'(s)) > 2.0 || (real'(c) - wc) > 2.0 || (wc - real'(c)) > 2.0) begin
                errs++;
                if (errs < 5) $display("sincos W=%0d a=%h sin=%0d (%f) cos=%0d (%f)", W, a, s, ws, c, wc);
            end
        end
        done = 1;
    end
endmodule

module tb;
    int errs = 0;

    // ---------------------------------------------------------------- clocks
    logic clk_s = 0, clk_d = 0, rst_n = 0;
    always #7 clk_s = ~clk_s;
    always #11 clk_d = ~clk_d;

    // reset synchronizer
    logic rin_n = 0, rout_n;
    MW_reset_sync #(.STAGES(2)) u_rs (.clk(clk_d), .rst_in_n(rin_n), .rst_out_n(rout_n));

    // pulse synchronizer
    logic ps = 0, pbusy, pd;
    int sent_p = 0, got_p = 0;
    MW_pulse_sync u_ps (.clk_s, .rst_s_n(rst_n), .pulse_s(ps), .busy(pbusy), .clk_d, .rst_d_n(rst_n), .pulse_d(pd));
    always @(posedge clk_d) if (rst_n && pd) got_p++;

    // data synchronizer
    logic send = 0, dbusy, avail;
    logic [11:0] ds = 0, dd;
    logic [11:0] q[$];
    int got_d = 0;
    MW_data_sync #(.WIDTH(12)) u_ds (.clk_s, .rst_s_n(rst_n), .send, .data_s(ds), .busy(dbusy),
                                     .clk_d, .rst_d_n(rst_n), .data_d(dd), .data_avail(avail));
    always @(posedge clk_d) if (rst_n && avail) begin
        logic [11:0] w;
        if (q.size() == 0) begin errs++; $display("data_sync: a word nobody sent: %h", dd); end
        else begin
            w = q.pop_front();
            if (w !== dd) begin errs++; $display("data_sync: got %h want %h", dd, w); end
        end
        got_d++;
    end

    // 8b10b
    logic ek, erd, erd_o, ekerr;
    logic [7:0] edata;
    logic [9:0] ecode, dcode;
    logic drd, dk, drd_o, dcerr, drerr;
    logic [7:0] ddata;
    MW_8b10b_enc u_enc (.k(ek), .data(edata), .rd_in(erd), .code(ecode), .rd_out(erd_o), .k_err(ekerr));
    MW_8b10b_dec u_dec (.code(dcode), .rd_in(drd), .data(ddata), .k(dk), .rd_out(drd_o), .code_err(dcerr), .rd_err(drerr));

    // FIR
    logic fen = 0;
    logic [4*6-1:0] fcoef;
    logic signed [7:0] fx = 0;
    logic signed [7+6+2:0] fy;
    MW_fir #(.DATA_WIDTH(8), .COEF_WIDTH(6), .TAPS(4)) u_fir (.clk(clk_s), .rst_n, .en(fen), .coef(fcoef), .x(fx), .y(fy));

    // TAP
    logic tck = 0, trst_n = 0, tms = 1, tdi = 0, tdo, tdo_en, tlr, cdr, sdr, udr;
    logic [3:0] ir;
    MW_tap #(.IR_WIDTH(4), .IDCODE(32'h1BAD_C0DF), .IDCODE_OPCODE(4'h1)) u_tap (
        .tck, .trst_n, .tms, .tdi, .tdo, .tdo_en, .ir, .test_logic_reset(tlr),
        .capture_dr(cdr), .shift_dr(sdr), .update_dr(udr), .user_tdo(1'b1));
    task automatic step(input logic m, input logic d, output logic o);
        tms = m; tdi = d;
        #5 tck = 1;
        #5 tck = 0;
        #1 o = tdo;
    endtask

    logic [1:0] sgo = 0;
    int serr[3];
    logic sdone[3];
    sc_chk #(8)  c8  (.go(sgo[0]), .errs(serr[0]), .done(sdone[0]));
    sc_chk #(12) c12 (.go(sgo[0]), .errs(serr[1]), .done(sdone[1]));
    sc_chk #(16) c16 (.go(sgo[0]), .errs(serr[2]), .done(sdone[2]));

    initial begin
        int e0;
        // ---- reset synchronizer: low at once, high two clk_d edges after release
        e0 = errs;
        #30;
        if (rout_n !== 1'b0) begin errs++; $display("reset_sync: not low in reset"); end
        @(negedge clk_d) rin_n = 1;
        @(posedge clk_d) #1 if (rout_n !== 1'b0) begin errs++; $display("reset_sync: high after one edge"); end
        @(posedge clk_d) #1 if (rout_n !== 1'b1) begin errs++; $display("reset_sync: still low after two edges"); end
        #3 rin_n = 0;
        #1 if (rout_n !== 1'b0) begin errs++; $display("reset_sync: assertion waited for the clock"); end
        rin_n = 1;
        if (errs == e0) $display("reset_sync ok");

        // ---- pulse synchronizer
        e0 = errs;
        rst_n = 0;
        #40 rst_n = 1;
        for (int i = 0; i < 40; i++) begin
            @(posedge clk_s);
            while (pbusy) @(posedge clk_s);
            #1 ps = 1;
            @(posedge clk_s) #1 ps = 0;
            sent_p++;
            repeat ($urandom_range(0, 3)) @(posedge clk_s);
        end
        repeat (20) @(posedge clk_d);
        if (got_p != sent_p) begin errs++; $display("pulse_sync: sent %0d got %0d", sent_p, got_p); end
        if (errs == e0) $display("pulse_sync ok");

        // ---- data synchronizer
        e0 = errs;
        for (int i = 0; i < 40; i++) begin
            @(posedge clk_s);
            while (dbusy) @(posedge clk_s);
            #1 send = 1;
            ds = 12'($urandom);
            q.push_back(ds);
            @(posedge clk_s) #1 send = 0;
            ds = 12'($urandom);                 // changing data must not leak through
        end
        repeat (30) @(posedge clk_d);
        if (got_d != 40 || q.size() != 0) begin errs++; $display("data_sync: got %0d of 40", got_d); end
        if (errs == e0) $display("data_sync ok");

        // ---- 8b10b
        e0 = errs;
        begin
            bit valid [2][1024];
            int ones, run, maxrun;
            logic last, rd;
            // known codes (802.3 table 36-1/36-2)
            ek = 1; edata = {3'd5, 5'd28}; erd = 0; #1;
            if (ecode !== 10'b0011111010 || erd_o !== 1) begin errs++; $display("8b10b: K28.5 RD- = %b", ecode); end
            erd = 1; #1;
            if (ecode !== 10'b1100000101 || erd_o !== 0) begin errs++; $display("8b10b: K28.5 RD+ = %b", ecode); end
            ek = 0; edata = {3'd5, 5'd21}; erd = 0; #1;
            if (ecode !== 10'b1010101010 || erd_o !== 0) begin errs++; $display("8b10b: D21.5 = %b", ecode); end
            edata = 0; #1;
            if (ecode !== 10'b1001110100) begin errs++; $display("8b10b: D0.0 RD- = %b", ecode); end
            edata = {3'd7, 5'd17}; #1;      // D17.7 RD-: alternate 0111 after 100011
            if (ecode !== 10'b1000110111) begin errs++; $display("8b10b: D17.7 RD- = %b", ecode); end
            ek = 1; edata = {3'd0, 5'd1}; #1;
            if (!ekerr) begin errs++; $display("8b10b: K1.0 not flagged"); end
            // every character at both disparities: balance, disparity rule, round trip
            for (int p = 0; p < 2; p++)
                for (int i = 0; i < 268; i++) begin
                    ek = i >= 256;
                    edata = i < 256 ? 8'(i) : i < 264 ? {3'(i - 256), 5'd28} : i == 264 ? {3'd7, 5'd23}
                          : i == 265 ? {3'd7, 5'd27} : i == 266 ? {3'd7, 5'd29} : {3'd7, 5'd30};
                    erd = p[0];
                    #1;
                    ones = $countones(ecode);
                    if (ekerr || !(ones == 5 && erd_o == erd || ones == 6 && !erd && erd_o || ones == 4 && erd && !erd_o)) begin
                        errs++;
                        if (errs < e0 + 5) $display("8b10b: %s%h rd%0d -> %b rd%0d", ek ? "K" : "D", edata, erd, ecode, erd_o);
                    end
                    if (valid[p][ecode]) begin errs++; $display("8b10b: code %b twice", ecode); end
                    valid[p][ecode] = 1;
                    dcode = ecode; drd = erd; #1;
                    if (dcerr || drerr || dk !== ek || ddata !== edata || drd_o !== erd_o) begin
                        errs++;
                        if (errs < e0 + 5) $display("8b10b: decode %b rd%0d -> %h k%0d err %0d%0d", dcode, drd, ddata, dk, dcerr, drerr);
                    end
                end
            // every 10-bit code: an error exactly when no character has it
            for (int cd = 0; cd < 1024; cd++)
                for (int p = 0; p < 2; p++) begin
                    dcode = 10'(cd); drd = p[0]; #1;
                    if (dcerr !== !(valid[0][cd] || valid[1][cd]) || (!dcerr && drerr !== !valid[p][cd])) begin
                        errs++;
                        if (errs < e0 + 5) $display("8b10b: code %b rd%0d err %0d%0d", dcode, drd, dcerr, drerr);
                    end
                end
            // a random stream: no run longer than five
            rd = 0; run = 0; maxrun = 0; last = 0;
            for (int i = 0; i < 3000; i++) begin
                ek = ($urandom % 16) == 0;
                edata = ek ? {3'($urandom), 5'd28} : 8'($urandom);
                erd = rd; #1;
                for (int b = 9; b >= 0; b--) begin
                    run = (i == 0 && b == 9) || ecode[b] != last ? 1 : run + 1;
                    last = ecode[b];
                    if (run > maxrun) maxrun = run;
                end
                rd = erd_o;
            end
            if (maxrun > 5) begin errs++; $display("8b10b: a run of %0d", maxrun); end
        end
        if (errs == e0) $display("8b10b ok");

        // ---- sincos
        sgo[0] = 1;
        wait (sdone[0] && sdone[1] && sdone[2]);
        errs += serr[0] + serr[1] + serr[2];
        if (serr[0] + serr[1] + serr[2] == 0) $display("sincos ok");

        // ---- FIR
        e0 = errs;
        begin
            logic signed [5:0] cf [4];
            logic signed [7:0] hist [4];
            int want;
            for (int k = 0; k < 4; k++) begin cf[k] = 6'($urandom); hist[k] = 0; end
            cf[0] = -32;
            fcoef = {cf[3], cf[2], cf[1], cf[0]};
            for (int n = 0; n < 200; n++) begin
                @(negedge clk_s);
                fx = 8'($urandom);
                if (n == 5) fx = -128;
                fen = $urandom % 4 != 0;
                if (fen) begin
                    for (int k = 3; k > 0; k--) hist[k] = hist[k-1];
                    hist[0] = fx;
                    want = 0;
                    for (int k = 0; k < 4; k++) want += int'(cf[k]) * int'(hist[k]);
                    @(posedge clk_s) #1;
                    if (int'(fy) != want) begin errs++; if (errs < e0 + 5) $display("fir: n=%0d y=%0d want %0d", n, fy, want); end
                end
            end
            fen = 0;
        end
        if (errs == e0) $display("fir ok");

        // ---- TAP
        e0 = errs;
        begin
            logic o;
            logic [31:0] id;
            logic [7:0] pat, got;
            #3 trst_n = 1;
            for (int i = 0; i < 5; i++) step(1, 0, o);
            if (!tlr || ir !== 4'h1) begin errs++; $display("tap: not in Test-Logic-Reset with IDCODE"); end
            // IDCODE scan
            step(0, 0, o); step(1, 0, o); step(0, 0, o); step(0, 0, o);
            id[0] = o;
            if (!tdo_en) begin errs++; $display("tap: tdo not enabled in Shift-DR"); end
            for (int i = 1; i < 32; i++) begin step(0, 0, o); id[i] = o; end
            step(1, 0, o); step(1, 0, o); step(0, 0, o);
            if (id !== 32'h1BAD_C0DF) begin errs++; $display("tap: IDCODE %h", id); end
            // IR scan: load BYPASS, see the captured 01
            step(1, 0, o); step(1, 0, o); step(0, 0, o); step(0, 0, o);
            got[0] = o;
            for (int i = 0; i < 4; i++) begin step(i == 3, 1, o); got[i + 1] = o; end
            step(1, 0, o); step(0, 0, o);
            if (got[1:0] !== 2'b01 || ir !== 4'hF) begin errs++; $display("tap: IR capture %b, ir %h", got[3:0], ir); end
            // BYPASS: one bit of delay, capturing 0
            step(1, 0, o); step(0, 0, o); step(0, 0, o);
            if (o !== 0) begin errs++; $display("tap: bypass did not capture 0"); end
            pat = 8'b1011_0010;
            for (int i = 0; i < 8; i++) begin step(i == 7, pat[i], o); got[i] = o; end
            step(1, 0, o); step(0, 0, o);
            if (got[6:0] !== pat[6:0]) begin errs++; $display("tap: bypass %b for %b", got, pat); end
            // a user instruction: the controller gives the strobes and passes user_tdo
            step(1, 0, o); step(1, 0, o); step(0, 0, o); step(0, 0, o);
            for (int i = 0; i < 4; i++) step(i == 3, i == 1, o);       // 4'b0010
            step(1, 0, o); step(0, 0, o);
            step(1, 0, o); step(0, 0, o);
            if (!cdr) begin errs++; $display("tap: no capture_dr for a user instruction"); end
            step(0, 0, o);
            if (!sdr || o !== 1) begin errs++; $display("tap: no shift_dr / user_tdo"); end
            step(1, 0, o); step(1, 0, o);
            if (!udr) begin errs++; $display("tap: no update_dr"); end
            // trst_n resets at once
            trst_n = 0; #1;
            if (!tlr || ir !== 4'h1) begin errs++; $display("tap: trst_n did not reset"); end
        end
        if (errs == e0) $display("tap ok");

        $display("errors=%0d", errs);
        $finish;
    end
endmodule
