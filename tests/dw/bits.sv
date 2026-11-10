// Shifters, rotators, bit counting, encoders, code converters, muxes,
// saturation and ECC against behavioural references.
//! args: -y ../../dw +libext+.sv -top tb
//> shift ok
//> count ok
//> code ok
//> ecc ok
//> errors=0
module bits_chk #(parameter int W = 8) (input logic go, output int e_sh, output int e_cnt, output int e_code);
    localparam int SW = (W > 1) ? $clog2(W) : 1;
    localparam int CW = $clog2(W + 1);
    logic [W-1:0] a, sh_o, rot_o, norm_o, gray, back, want;
    logic [SW:0] sh;                         // one bit wider: shifts past WIDTH too
    logic [SW-1:0] rsh, pe_lo, pe_hi, o2b;
    logic dir, arith, all0, n_all0, v_lo, v_hi, par, perr, o2b_err;
    logic [CW-1:0] lz, n_cnt, pop;
    logic [SW-1:0] ls;
    MW_shifter #(.WIDTH(W), .SH_WIDTH(SW+1)) u_sh (.data(a), .sh, .dir, .arith, .out(sh_o));
    MW_rotate #(.WIDTH(W)) u_rot (.data(a), .sh(rsh), .dir, .out(rot_o));
    MW_lzd #(.WIDTH(W)) u_lzd (.a, .count(lz), .all_0(all0));
    MW_lsd #(.WIDTH(W)) u_lsd (.a, .count(ls));
    MW_norm #(.WIDTH(W)) u_norm (.a, .out(norm_o), .count(n_cnt), .all_0(n_all0));
    MW_pri_enc #(.WIDTH(W), .LSB_FIRST(1)) u_pl (.a, .index(pe_lo), .valid(v_lo));
    MW_pri_enc #(.WIDTH(W), .LSB_FIRST(0)) u_ph (.a, .index(pe_hi), .valid(v_hi));
    MW_popcount #(.WIDTH(W)) u_pop (.a, .count(pop));
    MW_parity #(.WIDTH(W), .ODD(1)) u_par (.a, .parity_in(1'b0), .parity(par), .error(perr));
    MW_bin2gray #(.WIDTH(W)) u_b2g (.b(a), .g(gray));
    MW_gray2bin #(.WIDTH(W)) u_g2b (.g(gray), .b(back));
    logic [W-1:0] oh;
    MW_onehot2bin #(.WIDTH(W)) u_o2b (.a(oh), .index(o2b), .error(o2b_err));
    int n, lzr, lsr, lo, hi;
    logic [W-1:0] g1, g2;
    always @(posedge go) begin
        e_sh = 0; e_cnt = 0; e_code = 0;
        for (int i = 0; i < 600; i++) begin
            a = W'({$urandom, $urandom, $urandom});
            if (i % 4 == 1) a = a >> $urandom_range(0, W);
            if (i % 4 == 2) a = ~(a >> $urandom_range(0, W));
            if (i == 0) a = '0;
            if (i == 3) a = '1;
            sh = $urandom;
            rsh = $urandom;
            {dir, arith} = $urandom;
            oh = '0;
            oh[i % W] = 1'b1;
            if (i % 9 == 8 && W > 1) oh[(i + 1) % W] = 1'b1;
            if (i == 5) oh = '0;
            #1;
            // shifts
            if (!dir) want = a << sh;
            else if (arith) want = W'($signed(a) >>> sh);
            else want = a >> sh;
            if (sh_o !== want) begin
                e_sh++;
                if (e_sh < 4) $display("shifter W=%0d: %h dir=%b arith=%b sh=%0d = %h, want %h", W, a, dir, arith, sh, sh_o, want);
            end
            for (int k = 0; k < W; k++)
                want[k] = dir ? a[(k + int'(rsh)) % W] : a[(k + W - int'(rsh) % W) % W];
            if (rot_o !== want) begin
                e_sh++;
                if (e_sh < 4) $display("rotate W=%0d: %h dir=%b sh=%0d = %h, want %h", W, a, dir, rsh, rot_o, want);
            end
            // counts
            n = 0; lzr = W; lo = -1; hi = -1;
            for (int k = 0; k < W; k++) if (a[k]) begin n++; lzr = W - 1 - k; hi = k; if (lo < 0) lo = k; end
            lsr = W - 1;
            for (int k = 0; k < W - 1; k++) if (a[k] != a[W-1]) lsr = W - 2 - k;
            if (pop !== CW'(n) || lz !== CW'(lzr) || all0 !== (a == 0) || ls !== SW'(lsr)) begin
                e_cnt++;
                if (e_cnt < 4) $display("count W=%0d: %b pop=%0d lz=%0d ls=%0d (want %0d %0d %0d)", W, a, pop, lz, ls, n, lzr, lsr);
            end
            if (norm_o !== (a << lzr) || n_cnt !== lz || n_all0 !== all0) e_cnt++;
            if (v_lo !== (a != 0) || v_hi !== (a != 0)) e_cnt++;
            if (a != 0 && (pe_lo !== SW'(lo) || pe_hi !== SW'(hi))) e_cnt++;
            if (a == 0 && (pe_lo !== '0 || pe_hi !== '0)) e_cnt++;
            if (par !== ~(^a) || perr !== par) e_cnt++;
            // codes
            if (back !== a) e_code++;
            g1 = a ^ (a >> 1);
            g2 = (a + 1'b1) ^ ((a + 1'b1) >> 1);
            if (gray !== g1 || $countones(g1 ^ g2) != 1) e_code++;
            if ($countones(oh) == 1) begin
                if (o2b !== SW'(i % W) || o2b_err) e_code++;
            end else if (!o2b_err) e_code++;
        end
    end
endmodule

module mux_chk (input logic go, output int errs);
    logic [5*7-1:0] in;
    logic [2:0] sel;
    logic [4:0] oh;
    logic [6:0] o, oo, want;
    logic [3:0] da;
    logic den;
    logic [15:0] dout;
    logic [11:0] sa;
    logic [4:0] so;
    logic stc, ssat;
    MW_mux #(.WIDTH(7), .NUM(5)) u_m (.in, .sel, .out(o));
    MW_mux_onehot #(.WIDTH(7), .NUM(5)) u_o (.in, .sel(oh), .out(oo));
    MW_decode #(.WIDTH(4)) u_d (.a(da), .en(den), .out(dout));
    MW_sat #(.IN_WIDTH(12), .OUT_WIDTH(5)) u_s (.a(sa), .tc(stc), .out(so), .sat(ssat));
    logic [4:0] wso;
    logic wsat;
    always @(posedge go) begin
        errs = 0;
        for (int i = 0; i < 400; i++) begin
            in = {$urandom, $urandom};
            sel = $urandom;
            oh = $urandom;
            da = $urandom;
            den = (i % 8 != 0);
            sa = $urandom;
            if (i % 3 == 0) sa = 12'($signed(6'($urandom)));
            stc = $urandom;
            #1;
            want = (sel < 5) ? in[sel*7 +: 7] : '0;
            if (o !== want) errs++;
            want = '0;
            for (int k = 0; k < 5; k++) if (oh[k]) want |= in[k*7 +: 7];
            if (oo !== want) errs++;
            if (dout !== (den ? 16'(1) << da : 16'd0)) errs++;
            wso = sa[4:0]; wsat = 0;
            if (stc) begin
                if ($signed(sa) > 15) begin wso = 5'd15; wsat = 1; end
                if ($signed(sa) < -16) begin wso = 5'b10000; wsat = 1; end
            end else if (sa > 31) begin wso = 5'd31; wsat = 1; end
            if (so !== wso || ssat !== wsat) begin
                errs++;
                if (errs < 4) $display("sat %h tc=%b = %h %b, want %h %b", sa, stc, so, ssat, wso, wsat);
            end
        end
    end
endmodule

module ecc_chk #(parameter int W = 32, parameter int C = 7) (input logic go, output int errs);
    logic [W-1:0] d, stored, fixed;
    logic [C-1:0] chk, schk, syn;
    logic det, mult;
    logic [C-1:0] unused;
    MW_ecc #(.WIDTH(W), .CHK_WIDTH(C)) u_gen (.gen(1'b1), .data_in(d), .chk_in('0), .data_out(), .chk_out(chk),
                                              .err_detect(), .err_multiple());
    MW_ecc #(.WIDTH(W), .CHK_WIDTH(C)) u_chk (.gen(1'b0), .data_in(stored), .chk_in(schk), .data_out(fixed),
                                              .chk_out(syn), .err_detect(det), .err_multiple(mult));
    int f1, f2;
    always @(posedge go) begin
        errs = 0;
        for (int i = 0; i < 300; i++) begin
            d = W'({$urandom, $urandom, $urandom, $urandom});
            #1;
            // clean word
            stored = d; schk = chk;
            #1 if (det || mult || fixed !== d || syn !== '0) errs++;
            // every single-bit error, data and check
            for (int k = 0; k < W + C; k += (i < 3 ? 1 : 5)) begin
                stored = d; schk = chk;
                if (k < W) stored[k] = ~stored[k]; else schk[k-W] = ~schk[k-W];
                #1 if (!det || mult || fixed !== d) begin
                    errs++;
                    if (errs < 4) $display("ecc W=%0d: single error at %0d not corrected", W, k);
                end
            end
            // double errors are flagged
            repeat (8) begin
                f1 = $urandom_range(0, W + C - 1);
                f2 = $urandom_range(0, W + C - 1);
                if (f1 != f2) begin
                    stored = d; schk = chk;
                    if (f1 < W) stored[f1] = ~stored[f1]; else schk[f1-W] = ~schk[f1-W];
                    if (f2 < W) stored[f2] = ~stored[f2]; else schk[f2-W] = ~schk[f2-W];
                    #1 if (!det || !mult) begin
                        errs++;
                        if (errs < 4) $display("ecc W=%0d: double error %0d,%0d not flagged", W, f1, f2);
                    end
                end
            end
        end
    end
endmodule

module tb;
    logic go = 0;
    localparam int WS [5] = '{2, 5, 8, 31, 64};
    int es [5], ec [5], eo [5], em, ee [4];
    generate
        for (genvar i = 0; i < 5; i++) begin : g_b
            bits_chk #(.W(WS[i])) u (.go, .e_sh(es[i]), .e_cnt(ec[i]), .e_code(eo[i]));
        end
    endgenerate
    mux_chk u_mux (.go, .errs(em));
    ecc_chk #(.W(8), .C(5)) u_e0 (.go, .errs(ee[0]));
    ecc_chk #(.W(16), .C(6)) u_e1 (.go, .errs(ee[1]));
    ecc_chk #(.W(32), .C(7)) u_e2 (.go, .errs(ee[2]));
    ecc_chk #(.W(64), .C(8)) u_e3 (.go, .errs(ee[3]));
    int tot, t;
    initial begin
        #1 go = 1;
        #200000;
        tot = 0; t = 0;
        for (int i = 0; i < 5; i++) t += es[i];
        if (t == 0) $display("shift ok"); tot += t; t = 0;
        for (int i = 0; i < 5; i++) t += ec[i];
        if (t == 0) $display("count ok"); tot += t; t = 0;
        for (int i = 0; i < 5; i++) t += eo[i];
        t += em;
        if (t == 0) $display("code ok"); tot += t; t = 0;
        for (int i = 0; i < 4; i++) t += ee[i];
        if (t == 0) $display("ecc ok"); tot += t;
        $display("errors=%0d", tot);
    end
endmodule
