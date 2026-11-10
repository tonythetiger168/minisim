// MW_add, MW_sub, MW_addsub, MW_inc, MW_dec, MW_incdec, MW_cmp, MW_minmax,
// MW_absval, MW_addsat: every architecture and several widths against the
// SystemVerilog operators.
//! args: -y ../../dw +libext+.sv -top tb
//> add ok
//> incdec ok
//> cmp ok
//> sat ok
//> errors=0
module add_chk #(parameter int W = 8, parameter int ARCH = 0) (input logic go, output int errs);
    logic [W-1:0] a, b, sum, diff, as;
    logic ci, co, bo, aco, mode;
    MW_add #(.WIDTH(W), .ARCH(ARCH), .BLOCK(3)) u_add (.a, .b, .ci, .sum, .co);
    MW_sub #(.WIDTH(W), .ARCH(ARCH)) u_sub (.a, .b, .bi(ci), .diff, .bo);
    MW_addsub #(.WIDTH(W), .ARCH(ARCH)) u_as (.a, .b, .ci, .add_sub(mode), .sum(as), .co(aco));
    logic [W:0] ref_add, ref_sub;
    always @(posedge go) begin
        errs = 0;
        for (int i = 0; i < 400; i++) begin
            a = W'({$urandom, $urandom, $urandom});
            b = W'({$urandom, $urandom, $urandom});
            ci = $urandom;
            mode = $urandom;
            case (i)
                0: begin a = '1; b = '0; ci = 1; end
                1: begin a = '1; b = '1; ci = 1; end
                2: begin a = '0; b = '0; ci = 0; end
                3: begin a = '0; b = '1; ci = 1; end
                4: begin a = {1'b0, {(W-1){1'b1}}}; b = W'(1); ci = 0; end
                default: ;
            endcase
            #1;
            ref_add = {1'b0, a} + {1'b0, b} + (W+1)'(ci);
            ref_sub = {1'b0, a} - {1'b0, b} - (W+1)'(ci);
            if ({co, sum} !== ref_add) begin
                errs++;
                if (errs < 4) $display("add W=%0d ARCH=%0d: %h+%h+%b = %b_%h, want %h", W, ARCH, a, b, ci, co, sum, ref_add);
            end
            if ({bo, diff} !== ref_sub) begin
                errs++;
                if (errs < 4) $display("sub W=%0d ARCH=%0d: %h-%h-%b = %b_%h, want %h", W, ARCH, a, b, ci, bo, diff, ref_sub);
            end
            if ({aco, as} !== (mode ? ref_sub : ref_add)) begin
                errs++;
                if (errs < 4) $display("addsub W=%0d ARCH=%0d mode=%b", W, ARCH, mode);
            end
        end
    end
endmodule

module misc_chk #(parameter int W = 8) (input logic go, output int e_inc, output int e_cmp, output int e_sat);
    logic [W-1:0] a, b, inc, dec, id, av, ss;
    logic sel, tc, lt, gt, eq, le, ge, ne, sat, sub;
    MW_inc #(.WIDTH(W)) u_inc (.a, .sum(inc));
    MW_dec #(.WIDTH(W)) u_dec (.a, .sum(dec));
    MW_incdec #(.WIDTH(W)) u_id (.a, .inc_dec(sel), .sum(id));
    MW_cmp #(.WIDTH(W)) u_cmp (.a, .b, .tc, .lt, .gt, .eq, .le, .ge, .ne);
    MW_absval #(.WIDTH(W)) u_abs (.a, .absval(av));
    MW_addsat #(.WIDTH(W)) u_sat (.a, .b, .tc, .add_sub(sub), .sum(ss), .sat);
    // minimum / maximum of three
    logic [W-1:0] c, mm;
    logic [1:0] mi;
    logic mx;
    MW_minmax #(.WIDTH(W), .NUM(3)) u_mm (.a({c, b, a}), .tc, .min_max(mx), .value(mm), .index(mi));

    function automatic logic less(input logic [W-1:0] x, input logic [W-1:0] y, input logic s);
        return s ? ($signed(x) < $signed(y)) : (x < y);
    endfunction
    logic [W+1:0] wide;
    logic [W-1:0] want, wmm;
    logic wsat;
    always @(posedge go) begin
        e_inc = 0; e_cmp = 0; e_sat = 0;
        for (int i = 0; i < 500; i++) begin
            a = W'({$urandom, $urandom, $urandom});
            b = W'({$urandom, $urandom, $urandom});
            c = W'({$urandom, $urandom, $urandom});
            {sel, tc, sub, mx} = $urandom;
            if (i % 5 == 0) b = a;
            if (i % 7 == 0) c = b;
            if (i == 1) a = '1;
            if (i == 2) a = '0;
            if (i == 3) a = {1'b1, {(W-1){1'b0}}};
            if (i == 4) begin a = {1'b0, {(W-1){1'b1}}}; b = a; end
            #1;
            if (inc !== a + 1'b1 || dec !== a - 1'b1 || id !== (sel ? a - 1'b1 : a + 1'b1)) e_inc++;
            if (av !== (a[W-1] ? -a : a)) e_inc++;
            if (lt !== less(a, b, tc) || gt !== less(b, a, tc) || eq !== (a == b) ||
                le !== (lt | eq) || ge !== !lt || ne !== !eq) begin
                e_cmp++;
                if (e_cmp < 4) $display("cmp W=%0d %h %h tc=%b: lt=%b gt=%b eq=%b", W, a, b, tc, lt, gt, eq);
            end
            // min/max of three, lowest index on a tie
            wmm = a;
            if (mx ? less(wmm, b, tc) : less(b, wmm, tc)) wmm = b;
            if (mx ? less(wmm, c, tc) : less(c, wmm, tc)) wmm = c;
            if (mm !== wmm || (mi == 0 ? a : mi == 1 ? b : c) !== wmm) e_cmp++;
            if (mi == 1 && a === wmm) e_cmp++;
            if (mi == 2 && (a === wmm || b === wmm)) e_cmp++;
            // saturating add / subtract, computed two bits wider
            if (tc) wide = sub ? (W+2)'($signed(a)) - (W+2)'($signed(b)) : (W+2)'($signed(a)) + (W+2)'($signed(b));
            else    wide = sub ? (W+2)'(a) - (W+2)'(b) : (W+2)'(a) + (W+2)'(b);
            want = wide[W-1:0];
            wsat = 0;
            if (tc) begin
                if ($signed(wide) > $signed((W+2)'({1'b0, {(W-1){1'b1}}}))) begin wsat = 1; want = {1'b0, {(W-1){1'b1}}}; end
                if ($signed(wide) < -$signed((W+2)'({1'b1, {(W-1){1'b0}}}))) begin wsat = 1; want = {1'b1, {(W-1){1'b0}}}; end
            end else begin
                if (wide[W+1]) begin wsat = 1; want = '0; end
                else if (wide[W]) begin wsat = 1; want = '1; end
            end
            if (ss !== want || sat !== wsat) begin
                e_sat++;
                if (e_sat < 4) $display("addsat W=%0d %h %h tc=%b sub=%b: %h sat=%b, want %h %b", W, a, b, tc, sub, ss, sat, want, wsat);
            end
        end
    end
endmodule

module tb;
    logic go = 0;
    int ea [5][6];
    int ei [4], ec [4], es [4];
    localparam int WS [6] = '{1, 2, 5, 8, 33, 64};
    localparam int MS [4] = '{2, 7, 16, 40};
    generate
        for (genvar arch = 0; arch < 5; arch++) begin : g_a
            for (genvar w = 0; w < 6; w++) begin : g_w
                add_chk #(.W(WS[w]), .ARCH(arch)) u (.go, .errs(ea[arch][w]));
            end
        end
        for (genvar w = 0; w < 4; w++) begin : g_m
            misc_chk #(.W(MS[w])) u (.go, .e_inc(ei[w]), .e_cmp(ec[w]), .e_sat(es[w]));
        end
    endgenerate
    int tot, t;
    initial begin
        #1 go = 1;
        #2000;
        tot = 0; t = 0;
        for (int i = 0; i < 5; i++) for (int j = 0; j < 6; j++) t += ea[i][j];
        if (t == 0) $display("add ok"); tot += t; t = 0;
        for (int i = 0; i < 4; i++) t += ei[i];
        if (t == 0) $display("incdec ok"); tot += t; t = 0;
        for (int i = 0; i < 4; i++) t += ec[i];
        if (t == 0) $display("cmp ok"); tot += t; t = 0;
        for (int i = 0; i < 4; i++) t += es[i];
        if (t == 0) $display("sat ok"); tot += t;
        $display("errors=%0d", tot);
    end
endmodule
