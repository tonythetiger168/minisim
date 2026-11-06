// Compound assignment matches a = a op b, including signedness and width.
// Each result has its own variables. Verilator 5.052 drops an earlier
// $display of a variable that is written again later in the same initial.
module compound;
  logic [7:0] add_u, add_ub, add_eq, add_eqb;
  logic [7:0] sub_u, sub_ub, sub_eq, sub_eqb;
  logic [7:0] mul_u, mul_ub, mul_eq, mul_eqb;
  logic [7:0] div_u, div_ub, div_eq, div_eqb;
  logic [7:0] mod_u, mod_ub, mod_eq, mod_eqb;

  logic signed [7:0] add_s, add_sb, add_seq, add_seqb;
  logic signed [7:0] sub_s, sub_sb, sub_seq, sub_seqb;
  logic signed [7:0] mul_s, mul_sb, mul_seq, mul_seqb;
  logic signed [7:0] div_s, div_sb, div_seq, div_seqb;
  logic signed [7:0] mod_s, mod_sb, mod_seq, mod_seqb;

  logic [7:0] band, band_b, band_eq, band_eqb;
  logic [7:0] bor, bor_b;
  logic [7:0] bxor, bxor_b;
  logic [7:0] shl, shl_eq;
  logic [7:0] shr;
  logic signed [7:0] asr, asr_eq;
  logic [7:0] asru;

  logic [3:0] xadd, xadd_b, xeq, xeq_b;
  logic [3:0] zand, zand_b;

  logic [3:0] nar, nar_b4, nar_eq;
  logic [7:0] nar_b, nar_w;
  logic [3:0] ndiv, ndiv_eq, n20, n20_eq, nmod, nmod_eq;
  logic [7:0] ndiv_b, n20_b, nmod_b;

  logic signed [7:0] wadd;
  logic signed [3:0] wadd_b;
  logic [7:0] mix;
  logic signed [3:0] mix_b;

  logic signed [3:0] sdiv, sdiv_b, sdiv_eq, sdiv_eqb;
  logic signed [3:0] smod, smod_b;
  logic signed [3:0] sasr, sasr_eq;
  logic signed [3:0] smul, smul_b;

  logic [7:0] part;
  logic [3:0] acc, idx;
  logic [3:0] zdiv;

  initial begin
    add_u = 8'd200; add_ub = 8'd100; add_u += add_ub;
    add_eq = 8'd200; add_eqb = 8'd100; add_eq = add_eq + add_eqb;
    $display("U_ADD %b", add_u);
    $display("U_ADD_EQ %b", add_eq);

    sub_u = 8'd20; sub_ub = 8'd50; sub_u -= sub_ub;
    sub_eq = 8'd20; sub_eqb = 8'd50; sub_eq = sub_eq - sub_eqb;
    $display("U_SUB %b", sub_u);
    $display("U_SUB_EQ %b", sub_eq);

    mul_u = 8'd20; mul_ub = 8'd13; mul_u *= mul_ub;
    mul_eq = 8'd20; mul_eqb = 8'd13; mul_eq = mul_eq * mul_eqb;
    $display("U_MUL %b", mul_u);
    $display("U_MUL_EQ %b", mul_eq);

    div_u = 8'd200; div_ub = 8'd7; div_u /= div_ub;
    div_eq = 8'd200; div_eqb = 8'd7; div_eq = div_eq / div_eqb;
    $display("U_DIV %b", div_u);
    $display("U_DIV_EQ %b", div_eq);

    mod_u = 8'd200; mod_ub = 8'd7; mod_u %= mod_ub;
    mod_eq = 8'd200; mod_eqb = 8'd7; mod_eq = mod_eq % mod_eqb;
    $display("U_MOD %b", mod_u);
    $display("U_MOD_EQ %b", mod_eq);

    add_s = 8'shD8; add_sb = 8'shE2; add_s += add_sb;          // -40 + -30
    add_seq = 8'shD8; add_seqb = 8'shE2; add_seq = add_seq + add_seqb;
    $display("S_ADD %b", add_s);
    $display("S_ADD_EQ %b", add_seq);

    sub_s = 8'shD8; sub_sb = 8'sd10; sub_s -= sub_sb;           // -40 - 10
    sub_seq = 8'shD8; sub_seqb = 8'sd10; sub_seq = sub_seq - sub_seqb;
    $display("S_SUB %b", sub_s);
    $display("S_SUB_EQ %b", sub_seq);

    mul_s = 8'shF8; mul_sb = 8'sd3; mul_s *= mul_sb;            // -8 * 3
    mul_seq = 8'shF8; mul_seqb = 8'sd3; mul_seq = mul_seq * mul_seqb;
    $display("S_MUL %b", mul_s);
    $display("S_MUL_EQ %b", mul_seq);

    div_s = 8'shD8; div_sb = 8'sd7; div_s /= div_sb;            // -40 / 7
    div_seq = 8'shD8; div_seqb = 8'sd7; div_seq = div_seq / div_seqb;
    $display("S_DIV %b", div_s);
    $display("S_DIV_EQ %b", div_seq);

    mod_s = 8'shD8; mod_sb = 8'sd7; mod_s %= mod_sb;            // -40 % 7
    mod_seq = 8'shD8; mod_seqb = 8'sd7; mod_seq = mod_seq % mod_seqb;
    $display("S_MOD %b", mod_s);
    $display("S_MOD_EQ %b", mod_seq);

    band = 8'b11001100; band_b = 8'b10101010; band &= band_b;
    band_eq = 8'b11001100; band_eqb = 8'b10101010; band_eq = band_eq & band_eqb;
    $display("AND %b", band);
    $display("AND_EQ %b", band_eq);

    bor = 8'b11001100; bor_b = 8'b10101010; bor |= bor_b;
    $display("OR %b", bor);

    bxor = 8'b11001100; bxor_b = 8'b10101010; bxor ^= bxor_b;
    $display("XOR %b", bxor);

    shl = 8'b00001101; shl <<= 2;
    shl_eq = 8'b00001101; shl_eq = shl_eq << 2;
    $display("SHL %b", shl);
    $display("SHL_EQ %b", shl_eq);

    shr = 8'b00001101; shr >>= 2;
    $display("SHR %b", shr);

    asr = 8'sb11110000; asr >>>= 2;
    asr_eq = 8'sb11110000; asr_eq = asr_eq >>> 2;
    $display("ASR %b", asr);
    $display("ASR_EQ %b", asr_eq);

    asru = 8'b11110000; asru >>>= 2;
    $display("ASR_U %b", asru);

    xadd = 4'b10x0; xadd_b = 4'd1; xadd += xadd_b;
    xeq = 4'b10x0; xeq_b = 4'd1; xeq = xeq + xeq_b;
    $display("X_ADD %b", xadd);
    $display("X_ADD_EQ %b", xeq);

    zand = 4'b1100; zand_b = 4'b1z01; zand &= zand_b;
    $display("Z_AND %b", zand);

    nar = 4'h1; nar_b = 8'h0f; nar += nar_b;
    nar_eq = 4'h1; nar_eq = nar_eq + nar_b;
    nar_b4 = 4'h1; nar_w = nar_b4 + nar_b;
    $display("NAR %b", nar);
    $display("NAR_EQ %b", nar_eq);
    $display("NAR_W %b", nar_w);

    ndiv = 4'd7; ndiv_b = 8'd16; ndiv /= ndiv_b;
    ndiv_eq = 4'd7; ndiv_eq = ndiv_eq / ndiv_b;
    $display("NDIV %b", ndiv);
    $display("NDIV_EQ %b", ndiv_eq);

    // 7/20 is 0. The low 4 bits of 20 are 4, so a 4-bit divide would be 1.
    n20 = 4'd7; n20_b = 8'd20; n20 /= n20_b;
    n20_eq = 4'd7; n20_eq = n20_eq / n20_b;
    $display("N20 %b", n20);
    $display("N20_EQ %b", n20_eq);

    nmod = 4'd7; nmod_b = 8'd20; nmod %= nmod_b;
    nmod_eq = 4'd7; nmod_eq = nmod_eq % nmod_b;
    $display("NMOD %b", nmod);
    $display("NMOD_EQ %b", nmod_eq);

    wadd = 8'sd1; wadd_b = 4'sb1000; wadd += wadd_b;
    $display("WADD %b", wadd);

    mix = 8'd1; mix_b = 4'sb1000; mix += mix_b;
    $display("MIX %b", mix);

    sdiv = 4'sb1000; sdiv_b = 4'sd3; sdiv /= sdiv_b;
    sdiv_eq = 4'sb1000; sdiv_eqb = 4'sd3; sdiv_eq = sdiv_eq / sdiv_eqb;
    $display("S4_DIV %b", sdiv);
    $display("S4_DIV_EQ %b", sdiv_eq);

    smod = 4'sb1000; smod_b = 4'sd3; smod %= smod_b;
    $display("S4_MOD %b", smod);

    sasr = 4'sb1000; sasr >>>= 1;
    sasr_eq = 4'sb1000; sasr_eq = sasr_eq >>> 1;
    $display("S4_ASR %b", sasr);
    $display("S4_ASR_EQ %b", sasr_eq);

    smul = 4'sb1000; smul_b = 4'sd2; smul *= smul_b;
    $display("S4_MUL %b", smul);

    part = 8'b10111111; part[3:0] += 4'd1;
    $display("PART %b", part);

    acc = 0;
    for (idx = 0; idx < 4; idx += 1) acc += idx;
    $display("FOR %b", acc);

    zdiv = 4'd7; zdiv /= 4'd0;
    $display("DIV0 %b", zdiv);

    $finish;
  end
endmodule
