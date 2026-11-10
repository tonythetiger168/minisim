// MW_mult (array and Booth/Wallace), MW_square, MW_mac, MW_csa, MW_sum,
// MW_div, MW_sqrt, and the sequential MW_mult_seq, MW_div_seq, MW_mult_pipe
// against the SystemVerilog operators.
//! args: -y ../../dw +libext+.sv -top tb
//> mult ok
//> div ok
//> sqrt ok
//> seq ok
//> errors=0
module mul_chk #(parameter int AW = 8, parameter int BW = 8) (input logic go, output int errs);
    localparam int W = AW + BW;
    logic [AW-1:0] a;
    logic [BW-1:0] b;
    logic [W-1:0]  c, p0, p1, mac, want, s3, cy3, sum4;
    logic [2*AW-1:0] sq, wsq;
    logic tc, co;
    MW_mult #(.A_WIDTH(AW), .B_WIDTH(BW), .ARCH(0)) u0 (.a, .b, .tc, .product(p0));
    MW_mult #(.A_WIDTH(AW), .B_WIDTH(BW), .ARCH(1)) u1 (.a, .b, .tc, .product(p1));
    MW_square #(.WIDTH(AW)) u_sq (.a, .tc, .square(sq));
    MW_mac #(.A_WIDTH(AW), .B_WIDTH(BW)) u_mac (.a, .b, .c, .tc, .mac);
    MW_csa #(.WIDTH(W)) u_csa (.a(c), .b(p0), .c(W'(a)), .sum(s3), .carry(cy3), .co);
    MW_sum #(.WIDTH(W), .NUM(5)) u_sum (.in({c, p0, W'(a), W'(b), c}), .sum(sum4));
    always @(posedge go) begin
        errs = 0;
        for (int i = 0; i < 600; i++) begin
            a = AW'({$urandom, $urandom});
            b = BW'({$urandom, $urandom});
            c = W'({$urandom, $urandom, $urandom, $urandom});
            tc = $urandom;
            case (i)
                0: begin a = '1; b = '1; end
                1: begin a = {1'b1, {(AW-1){1'b0}}}; b = {1'b1, {(BW-1){1'b0}}}; end
                2: begin a = '0; b = '1; end
                3: begin a = {1'b1, {(AW-1){1'b0}}}; b = '1; end
                4: begin a = '1; b = {1'b0, {(BW-1){1'b1}}}; end
                5: begin a = {1'b0, {(AW-1){1'b1}}}; b = {1'b1, {(BW-1){1'b0}}}; end
                default: ;
            endcase
            #1;
            if (tc) want = W'($signed(a)) * W'($signed(b));
            else    want = W'(a) * W'(b);
            if (p0 !== want || p1 !== want) begin
                errs++;
                if (errs < 4) $display("mult %0dx%0d tc=%b: %h*%h = %h / %h, want %h", AW, BW, tc, a, b, p0, p1, want);
            end
            if (tc) wsq = (2*AW)'($signed(a)) * (2*AW)'($signed(a));
            else    wsq = (2*AW)'(a) * (2*AW)'(a);
            if (sq !== wsq) errs++;
            if (mac !== W'(want + c)) begin
                errs++;
                if (errs < 4) $display("mac %0dx%0d tc=%b: %h*%h+%h = %h", AW, BW, tc, a, b, c, mac);
            end
            if (W'(s3 + cy3) !== W'(c + p0 + W'(a))) errs++;
            if (sum4 !== W'(c + p0 + W'(a) + W'(b) + c)) errs++;
        end
    end
endmodule

module div_chk #(parameter int AW = 8, parameter int BW = 8) (input logic go, output int errs);
    logic [AW-1:0] a, q, wq;
    logic [BW-1:0] b, r, wr;
    logic tc, dz;
    localparam int M = (AW > BW ? AW : BW) + 1;
    logic signed [M-1:0] sa, sb, sq, sr;
    MW_div #(.A_WIDTH(AW), .B_WIDTH(BW)) u (.a, .b, .tc, .quotient(q), .remainder(r), .divide_by_0(dz));
    always @(posedge go) begin
        errs = 0;
        for (int i = 0; i < 800; i++) begin
            a = AW'({$urandom, $urandom});
            b = BW'({$urandom, $urandom});
            if (i % 3 == 0) b = BW'($urandom_range(1, 9));
            if (i % 11 == 0) b = -BW'($urandom_range(1, 9));
            tc = $urandom;
            case (i)
                0: b = '0;
                1: begin a = {1'b1, {(AW-1){1'b0}}}; b = '1; tc = 1; end
                2: begin a = '1; b = '1; end
                3: begin a = '0; b = BW'(3); end
                4: begin b = '0; tc = 1; a[AW-1] = 1; end
                5: begin b = '0; tc = 1; a[AW-1] = 0; end
                default: ;
            endcase
            #1;
            if (b == 0) begin
                wq = !tc ? '1 : a[AW-1] ? {1'b1, {(AW-1){1'b0}}} : {1'b0, {(AW-1){1'b1}}};
                if (tc) sa = M'($signed(a)); else sa = M'(a);
                wr = BW'(sa);
            end else begin
                if (tc) begin sa = M'($signed(a)); sb = M'($signed(b)); end
                else    begin sa = M'(a); sb = M'(b); end
                sq = sa / sb;
                sr = sa % sb;
                wq = AW'(sq);
                wr = BW'(sr);
            end
            if (q !== wq || r !== wr || dz !== (b == 0)) begin
                errs++;
                if (errs < 4) $display("div %0d/%0d tc=%b: %h/%h = %h r %h dz=%b, want %h r %h", AW, BW, tc, a, b, q, r, dz, wq, wr);
            end
        end
    end
endmodule

module sqrt_chk #(parameter int W = 8) (input logic go, output int errs);
    localparam int RW = (W + 1) / 2;
    logic [W-1:0] a;
    logic [RW-1:0] root;
    logic [RW:0] rem;
    logic [2*RW+1:0] lo, hi;
    MW_sqrt #(.WIDTH(W)) u (.a, .root, .remainder(rem));
    always @(posedge go) begin
        errs = 0;
        for (int i = 0; i < 600; i++) begin
            a = W'({$urandom, $urandom});
            if (i < 20) a = W'(i);
            if (i == 20) a = '1;
            if (i > 500) a = a >> (i % W);
            #1;
            lo = (2*RW+2)'(root) * (2*RW+2)'(root);
            hi = ((2*RW+2)'(root) + 1) * ((2*RW+2)'(root) + 1);
            if (!(lo <= a && a < hi) || (2*RW+2)'(rem) !== a - lo) begin
                errs++;
                if (errs < 4) $display("sqrt W=%0d: sqrt(%h) = %h rem %h", W, a, root, rem);
            end
        end
    end
endmodule

module seq_chk #(parameter int AW = 8, parameter int BW = 8) (input logic clk, input logic rst_n, output int errs, output logic fin);
    localparam int W = AW + BW;
    logic [AW-1:0] a, q, cq;
    logic [BW-1:0] b, r, cr;
    logic [W-1:0] p, cp, pp;
    logic tc, start, mdone, ddone, dz, cdz;
    MW_mult_seq #(.A_WIDTH(AW), .B_WIDTH(BW)) u_m (.clk, .rst_n, .start, .a, .b, .tc, .done(mdone), .product(p));
    MW_div_seq #(.A_WIDTH(AW), .B_WIDTH(BW)) u_d (.clk, .rst_n, .start, .a, .b, .tc, .done(ddone),
                                                .quotient(q), .remainder(r), .divide_by_0(dz));
    MW_mult #(.A_WIDTH(AW), .B_WIDTH(BW)) u_cm (.a, .b, .tc, .product(cp));
    MW_div #(.A_WIDTH(AW), .B_WIDTH(BW)) u_cd (.a, .b, .tc, .quotient(cq), .remainder(cr), .divide_by_0(cdz));
    MW_mult_pipe #(.A_WIDTH(AW), .B_WIDTH(BW), .STAGES(3)) u_p (.clk, .rst_n, .en(1'b1), .a, .b, .tc, .product(pp));
    int mlat, dlat;
    initial begin
        errs = 0; fin = 0; start = 0; a = 0; b = 0; tc = 0;
        @(posedge rst_n);
        for (int i = 0; i < 60; i++) begin
            @(negedge clk);
            a = AW'({$urandom, $urandom});
            b = BW'({$urandom, $urandom});
            tc = $urandom;
            if (i == 3) b = '0;
            if (i == 4) begin a = '1; b = '1; end
            start = 1;
            @(negedge clk);
            start = 0;
            if (mdone || ddone) errs++;          // start clears done
            mlat = 0; dlat = 0;
            while (!mdone) begin @(negedge clk); mlat++; end
            while (!ddone) begin @(negedge clk); dlat++; end
            // a, b, tc are still held, so the combinational modules give the answer
            if (p !== cp) begin errs++; if (errs < 4) $display("mult_seq %h*%h tc=%b = %h, want %h", a, b, tc, p, cp); end
            if (q !== cq || r !== cr || dz !== cdz) begin
                errs++;
                if (errs < 4) $display("div_seq %h/%h tc=%b = %h r %h, want %h r %h", a, b, tc, q, r, cq, cr);
            end
            if (pp !== cp) errs++;               // held for more than 3 clocks
            if (mlat > BW || dlat > AW) errs++;
            repeat (2) @(negedge clk);
            if (!mdone || !ddone || p !== cp || q !== cq) errs++;   // results hold
        end
        fin = 1;
    end
endmodule

module tb;
    logic go = 0, clk = 0, rst_n = 0;
    always #5 clk = ~clk;
    localparam int AS [6] = '{2, 4, 8, 13, 16, 32};
    localparam int BS [6] = '{2, 7, 8, 5, 16, 32};
    int em [6], ed [6], es [4], eq [3];
    logic fin [3];
    generate
        for (genvar i = 0; i < 6; i++) begin : g_m
            mul_chk #(.AW(AS[i]), .BW(BS[i])) um (.go, .errs(em[i]));
            div_chk #(.AW(AS[i]), .BW(BS[i])) ud (.go, .errs(ed[i]));
        end
        for (genvar i = 0; i < 4; i++) begin : g_s
            sqrt_chk #(.W(3 + 7*i)) us (.go, .errs(es[i]));
        end
        for (genvar i = 0; i < 3; i++) begin : g_q
            seq_chk #(.AW(AS[i+1]), .BW(BS[i+1])) uq (.clk, .rst_n, .errs(eq[i]), .fin(fin[i]));
        end
    endgenerate
    int tot, t;
    initial begin
        #1 go = 1;
        #12 rst_n = 1;
        wait (fin[0] && fin[1] && fin[2]);
        tot = 0; t = 0;
        for (int i = 0; i < 6; i++) t += em[i];
        if (t == 0) $display("mult ok"); tot += t; t = 0;
        for (int i = 0; i < 6; i++) t += ed[i];
        if (t == 0) $display("div ok"); tot += t; t = 0;
        for (int i = 0; i < 4; i++) t += es[i];
        if (t == 0) $display("sqrt ok"); tot += t; t = 0;
        for (int i = 0; i < 3; i++) t += eq[i];
        if (t == 0) $display("seq ok"); tot += t;
        $display("errors=%0d", tot);
        $finish;
    end
endmodule
