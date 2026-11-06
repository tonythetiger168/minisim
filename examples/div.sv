// Division and remainder against Icarus Verilog 13.
// Toward zero. Remainder takes the dividend's sign.
// Either operand unsigned makes the expression unsigned.
// A zero divisor, or any X or Z bit, makes every result bit X.
// The operator takes the assignment width, same as multiply.
// A 4-bit signed minimum divided by -1 wraps; the 8-bit context fits.
// Each case uses its own variables. One initial that reuses a variable
// makes Verilator 5.052 keep only the last write and print the earlier
// quotients as zero.
module divmod;
  logic [3:0] u_div_a, u_div_b, u_mod_a, u_mod_b;
  logic [3:0] u15_a, u15_b, u0_a, u0_b, uhi_a, uhi_b;
  logic signed [3:0] sn_a, sn_b, sd_a, sd_b, snd_a, snd_b;
  logic signed [3:0] sm1_a, sm1_b, smin2_a, smin2_b, smin1_a, smin1_b;
  logic [3:0] zdiv_a, zdiv_b, zmod_a, zmod_b, z0_a, z0_b;
  logic signed [3:0] zs_a, zs_b;
  logic [3:0] xn_a, xn_b, xd_a, xd_b, zn_a, zn_b, zd_a, zd_b;
  logic signed [3:0] xs_a, xs_b;
  logic signed [3:0] mix_a;
  logic [3:0] mix_b, mixu_a;
  logic signed [3:0] mixu_b;
  logic signed [3:0] cu_a, cu_b, cs_b;
  logic [3:0] cs_a;
  logic signed [3:0] wn_a, wn_b;
  logic signed [7:0] wn_q, wm_q, ws_q, ws_r;
  logic [7:0] wnu_u;
  logic [3:0] wm_b;
  logic signed [7:0] ws_b;
  logic signed [63:0] w64_a, w64_m1, w64_3, w64_q1, w64_r1, w64_q3, w64_r3;

  initial begin
    u_div_a = 4'd7; u_div_b = 4'd3;
    u_mod_a = 4'd7; u_mod_b = 4'd3;
    $display("U_DIV %b", u_div_a / u_div_b);
    $display("U_MOD %b", u_mod_a % u_mod_b);
    u15_a = 4'd15; u15_b = 4'd4;
    $display("U_DIV15 %b", u15_a / u15_b);
    $display("U_MOD15 %b", u15_a % u15_b);
    u0_a = 4'd0; u0_b = 4'd1;
    $display("U_0DIV1 %b", u0_a / u0_b);
    $display("U_0MOD1 %b", u0_a % u0_b);
    uhi_a = 4'b1111; uhi_b = 4'd2;
    $display("U_HI_DIV %b", uhi_a / uhi_b);
    $display("U_HI_MOD %b", uhi_a % uhi_b);

    sn_a = 4'sb1001; sn_b = 4'sd3;
    $display("S_N_DIV %b", sn_a / sn_b);
    $display("S_N_MOD %b", sn_a % sn_b);
    sd_a = 4'sd7; sd_b = 4'sb1101;
    $display("S_D_DIV %b", sd_a / sd_b);
    $display("S_D_MOD %b", sd_a % sd_b);
    snd_a = 4'sb1001; snd_b = 4'sb1101;
    $display("S_ND_DIV %b", snd_a / snd_b);
    $display("S_ND_MOD %b", snd_a % snd_b);
    sm1_a = 4'sb1111; sm1_b = 4'sd2;
    $display("S_M1_DIV %b", sm1_a / sm1_b);
    $display("S_M1_MOD %b", sm1_a % sm1_b);
    smin2_a = 4'sb1000; smin2_b = 4'sd2;
    $display("S_MIN2_DIV %b", smin2_a / smin2_b);
    $display("S_MIN2_MOD %b", smin2_a % smin2_b);
    smin1_a = 4'sb1000; smin1_b = 4'sb1111;
    $display("S_MIN1_DIV %b", smin1_a / smin1_b);
    $display("S_MIN1_MOD %b", smin1_a % smin1_b);

    zdiv_a = 4'd7; zdiv_b = 4'd0;
    $display("U_DIV0 %b", zdiv_a / zdiv_b);
    zmod_a = 4'd7; zmod_b = 4'd0;
    $display("U_MOD0 %b", zmod_a % zmod_b);
    zs_a = 4'sb1000; zs_b = 4'sd0;
    $display("S_DIV0 %b", zs_a / zs_b);
    $display("S_MOD0 %b", zs_a % zs_b);
    z0_a = 4'd0; z0_b = 4'd0;
    $display("U_0DIV0 %b", z0_a / z0_b);
    $display("U_0MOD0 %b", z0_a % z0_b);

    xn_a = 4'b10x0; xn_b = 4'd2;
    $display("X_N_DIV %b", xn_a / xn_b);
    $display("X_N_MOD %b", xn_a % xn_b);
    xd_a = 4'd8; xd_b = 4'bxx00;
    $display("X_D_DIV %b", xd_a / xd_b);
    $display("X_D_MOD %b", xd_a % xd_b);
    zn_a = 4'b10z0; zn_b = 4'd2;
    $display("Z_N_DIV %b", zn_a / zn_b);
    $display("Z_N_MOD %b", zn_a % zn_b);
    zd_a = 4'd8; zd_b = 4'bz001;
    $display("Z_D_DIV %b", zd_a / zd_b);
    $display("Z_D_MOD %b", zd_a % zd_b);
    xs_a = 4'sb100x; xs_b = 4'sd2;
    $display("X_S_DIV %b", xs_a / xs_b);
    $display("X_S_MOD %b", xs_a % xs_b);

    mix_a = 4'sb1000; mix_b = 4'd2;
    $display("MIX_DIV %b", mix_a / mix_b);
    $display("MIX_MOD %b", mix_a % mix_b);
    mixu_a = 4'd7; mixu_b = 4'sb1101;
    $display("MIX_UD_DIV %b", mixu_a / mixu_b);
    $display("MIX_UD_MOD %b", mixu_a % mixu_b);
    cu_a = 4'sb1000; cu_b = 4'sd2;
    $display("CAST_U_DIV %b", $unsigned(cu_a) / cu_b);
    $display("CAST_U_MOD %b", $unsigned(cu_a) % cu_b);
    cs_a = 4'b1000; cs_b = 4'sd2;
    $display("CAST_S_DIV %b", $signed(cs_a) / cs_b);
    $display("CAST_S_MOD %b", $signed(cs_a) % cs_b);

    wn_a = 4'sb1000; wn_b = 4'sb1111;
    $display("NARROW_MIN %b", wn_a / wn_b);
    $display("NARROW_MIN_MOD %b", wn_a % wn_b);
    wn_q = wn_a / wn_b;
    $display("WIDE_MIN %b", wn_q);
    wnu_u = wn_a / wn_b;
    $display("WIDE_MIN_U %b", wnu_u);
    wm_b = 4'd3;
    wm_q = wn_a / wm_b;
    $display("WIDE_MIX %b", wm_q);
    ws_b = 8'sd3;
    ws_q = wn_a / ws_b;
    $display("WIDE_S3 %b", ws_q);
    ws_r = wn_a % ws_b;
    $display("WIDE_S3_MOD %b", ws_r);

    w64_a = 64'sh8000_0000_0000_0000;
    w64_m1 = 64'shffff_ffff_ffff_ffff;
    w64_q1 = w64_a / w64_m1;
    w64_r1 = w64_a % w64_m1;
    $display("S64_MIN1_DIV %b", w64_q1);
    $display("S64_MIN1_MOD %b", w64_r1);
    w64_3 = 64'sd3;
    w64_q3 = w64_a / w64_3;
    w64_r3 = w64_a % w64_3;
    $display("S64_MIN3_DIV %b", w64_q3);
    $display("S64_MIN3_MOD %b", w64_r3);
    $finish;
  end
endmodule
