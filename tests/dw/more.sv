// MW_thermdec, MW_stack, MW_prod_sum, MW_div_pipe, MW_sqrt_pipe, MW_log2 and
// MW_exp2 against the SystemVerilog operators, a queue model and real
// arithmetic.
//! args: -y ../../dw +libext+.sv -top tb
//> thermdec ok
//> stack ok
//> prod_sum ok
//> pipes ok
//> log2 exp2 ok
//> errors=0
module log_chk #(parameter int W = 8) (input logic go, output int errs, output logic done = 0);
    logic [W-1:0] a, zl, ze, b;
    MW_log2 #(.OP_WIDTH(W)) u_l (.a, .z(zl));
    MW_exp2 #(.OP_WIDTH(W)) u_e (.a(b), .z(ze));
    always @(posedge go) begin
        real t;
        errs = 0;
        for (int i = 0; i < 3000; i++) begin
            a = {1'b1, (W-1)'($urandom)};
            b = W'($urandom);
            if (i < 4) begin a = {1'b1, {(W-1){i[0]}}}; b = {W{i[1]}}; end
            #1;
            // within one unit below the true value
            t = $ln(real'(a) / (2.0 ** (W - 1))) / $ln(2.0) * (2.0 ** W);
            if (!(real'(zl) <= t + 1e-6 && real'(zl) > t - 2.0)) begin
                errs++;
                if (errs < 5) $display("log2 W=%0d a=%h z=%h want %f", W, a, zl, t);
            end
            t = (2.0 ** (real'(b) / (2.0 ** W))) * (2.0 ** (W - 1));
            if (!(real'(ze) <= t + 1e-6 && real'(ze) > t - 2.0)) begin
                errs++;
                if (errs < 5) $display("exp2 W=%0d a=%h z=%h want %f", W, b, ze, t);
            end
        end
        done = 1;
    end
endmodule

module tb;
    int errs = 0;
    logic clk = 0, rst_n = 0;
    always #5 clk = ~clk;

    // thermometer decoder
    logic [3:0] ta;
    logic te;
    logic [15:0] th;
    MW_thermdec #(.WIDTH(4)) u_th (.a(ta), .en(te), .thermo(th));

    // stack against a queue
    logic push, pop, empty, full, serr;
    logic [7:0] sin, sout;
    logic [3:0] scount;
    MW_stack #(.WIDTH(8), .DEPTH(6)) u_st (.clk, .rst_n, .push, .pop, .data_in(sin), .data_out(sout),
                                           .empty, .full, .count(scount), .error(serr));

    // sum of products
    localparam int N = 3;
    logic [N*7-1:0] pa;
    logic [N*5-1:0] pb;
    logic ptc;
    logic [13:0] psum;
    MW_prod_sum #(.A_WIDTH(7), .B_WIDTH(5), .NUM(N)) u_ps (.a(pa), .b(pb), .tc(ptc), .sum(psum));

    // pipes
    logic [11:0] da;
    logic [6:0]  db;
    logic dtc, en;
    logic [11:0] dq;
    logic [6:0]  dr;
    logic dz;
    logic [5:0]  sq;
    logic [6:0]  sr;
    MW_div_pipe #(.A_WIDTH(12), .B_WIDTH(7), .STAGES(3)) u_dp (.clk, .rst_n, .en, .a(da), .b(db), .tc(dtc),
                                                               .quotient(dq), .remainder(dr), .divide_by_0(dz));
    MW_sqrt_pipe #(.WIDTH(12), .STAGES(2)) u_sp (.clk, .rst_n, .en, .a(da), .root(sq), .remainder(sr));

    logic go = 0;
    int e8, e13, e20;
    logic d8, d13, d20;
    log_chk #(8) c8 (.go, .errs(e8), .done(d8));
    log_chk #(13) c13 (.go, .errs(e13), .done(d13));
    log_chk #(20) c20 (.go, .errs(e20), .done(d20));

    initial begin
        int e;
        // thermometer
        e = 0;
        for (int i = 0; i < 32; i++) begin
            ta = i[3:0]; te = i[4];
            #1;
            for (int k = 0; k < 16; k++) if (th[k] !== (te && k < ta)) e++;
        end
        if (e == 0) $display("thermdec ok");
        errs += e;

        // stack
        e = 0;
        push = 0; pop = 0; sin = 0;
        #12 rst_n = 1;
        begin
            logic [7:0] q [$];
            logic want_err;
            for (int i = 0; i < 2000; i++) begin
                @(negedge clk);
                if (scount !== 4'(q.size()) || empty !== (q.size() == 0) || full !== (q.size() == 6) ||
                    (q.size() > 0 && sout !== q[$])) begin
                    e++;
                    if (e < 5) $display("stack: count %0d/%0d top %h/%h", scount, q.size(), sout, q.size() ? q[$] : 8'h0);
                end
                push = $urandom_range(0, 1);
                pop = $urandom_range(0, 1);
                sin = $urandom;
                want_err = 0;
                @(posedge clk);
                if (push && pop) begin
                    if (q.size() == 0) want_err = 1; else q[$] = sin;
                end else if (push) begin
                    if (q.size() == 6) want_err = 1; else q.push_back(sin);
                end else if (pop) begin
                    if (q.size() == 0) want_err = 1; else void'(q.pop_back());
                end
                #1 if (serr !== want_err) e++;
            end
        end
        if (e == 0) $display("stack ok");
        errs += e;

        // sum of products
        e = 0;
        for (int i = 0; i < 2000; i++) begin
            longint s;
            pa = {$urandom, $urandom};
            pb = {$urandom, $urandom};
            ptc = $urandom;
            #1;
            s = 0;
            for (int k = 0; k < N; k++) begin
                longint x, y;
                x = ptc ? longint'($signed(pa[k*7 +: 7])) : longint'(pa[k*7 +: 7]);
                y = ptc ? longint'($signed(pb[k*5 +: 5])) : longint'(pb[k*5 +: 5]);
                s += x * y;
            end
            if (psum !== 14'(s)) e++;
        end
        if (e == 0) $display("prod_sum ok");
        errs += e;

        // pipes: the outputs are the combinational result 3 (2) enabled clocks back
        e = 0;
        begin
            logic [11:0] ha [$];
            logic [6:0]  hb [$];
            logic        ht [$];
            en = 1;
            for (int i = 0; i < 600; i++) begin
                @(negedge clk);
                en = (i % 7) != 3;
                da = $urandom; db = $urandom; dtc = $urandom;
                if (i % 50 == 0) db = 0;
                @(posedge clk);
                if (en) begin ha.push_back(da); hb.push_back(db); ht.push_back(dtc); end
                #1;
                if (ha.size() > 3) begin void'(ha.pop_front()); void'(hb.pop_front()); void'(ht.pop_front()); end
                if (ha.size() == 3) begin
                    logic [11:0] wq;
                    logic [6:0]  wr;
                    if (hb[0] == 0) begin
                        if (dz !== 1'b1) e++;
                    end else if (ht[0]) begin
                        wq = 12'($signed(ha[0]) / $signed(hb[0]));
                        wr = 7'($signed(ha[0]) % $signed(hb[0]));
                        if ({dq, dr, dz} !== {wq, wr, 1'b0}) e++;
                    end else begin
                        if ({dq, dr, dz} !== {ha[0] / 12'(hb[0]), 7'(ha[0] % 12'(hb[0])), 1'b0}) e++;
                    end
                    // the root two stages back is the entry one later
                    if (int'(sq) != int'($floor($sqrt(real'(ha[1]))))) e++;
                    if (int'(sr) != int'(ha[1]) - int'(sq) * int'(sq)) e++;
                end
            end
        end
        if (e == 0) $display("pipes ok");
        errs += e;

        go = 1;
        wait (d8 && d13 && d20);
        if (e8 + e13 + e20 == 0) $display("log2 exp2 ok");
        errs += e8 + e13 + e20;
        $display("errors=%0d", errs);
        $finish;
    end
endmodule
