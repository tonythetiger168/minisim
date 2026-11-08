// IEEE 1800-2023 11.4 operators on packed vectors wider than 64 bits.
// Packed vectors wider than 64 bits: 128 and 256, including a shift, a
// part-select, and a concatenation that cross a 64-bit limb boundary.
// 0/1 results are below the XZ line. X and Z results are below it.
module child(input logic [127:0] x, output logic [127:0] y);
  assign y = x << 1;
endmodule

module wide;
  logic [127:0] a, b, c, blk, nba, p, q;
  logic [255:0] w;
  logic signed [127:0] s;
  wire [127:0] net;
  logic [127:0] cin, cout;

  assign net = a ^ b;
  child u(.x(cin), .y(cout));

  function logic [127:0] add1(input logic [127:0] x);
    return x + 128'h1;
  endfunction

  initial begin
    // p and q are not read by a continuous assign, so the blocking writes
    // are the values the operators see. a and b are also driven later.
    p = 128'h0123456789abcdeffedcba9876543210;
    q = 128'h00000000000000010000000000000001;
    $display("ADD %h", p + q);
    $display("SUB %h", p - q);
    $display("AND %h", p & q);
    $display("OR  %h", p | q);
    $display("XOR %h", p ^ q);
    $display("NOT %h", ~p);
    $display("SHL4 %h", p << 4);
    $display("SHR4 %h", p >> 4);
    $display("SHL60 %h", p << 60);
    $display("SHL64 %h", p << 64);
    $display("SHR68 %h", p >> 68);
    $display("REP %h", {128{1'b1}});
    $display("FN %h", add1(128'hffff_ffff_ffff_ffff));

    w = {p, q};
    $display("CAT %h", w);
    $display("PART %h", w[191:64]);
    $display("BIT %b", w[70:60]);
    w[70:60] = 11'h5a5;
    $display("PINS %b", w[75:55]);

    w = 256'h1;
    w = w << 128;
    w = w * 256'h3;
    $display("MUL %h", w);
    $display("DIV %h", w / 256'h3);
    w = 256'd100;
    $display("MOD %0d", w % 256'd7);

    s = -128'sd5;
    $display("SDIV %0d %0d", s / 128'sd2, s % 128'sd2);
    $display("ASR %h", s >>> 70);
    $display("DECS %d", s);

    a = 128'h1;
    a <<= 64;
    a += 128'h2;
    $display("COMP %h", a);

    a = 128'hff;
    b = 128'h0f;
    cin = 128'h1;
    blk = 128'h11;
    nba <= 128'h22;
    $display("PRE %h", nba);
    $strobe("STROBE %h", nba);
    #1;
    $display("NET %h", net);
    $display("PORT %h", cout);
    $display("BLK %h", blk);
    $display("NBA %h", nba);

    $display("XZ");
    a = {64'hxxxx_xxxx_xxxx_xxxx, 64'hffff_ffff_ffff_ffff};
    $display("HX %h", a);
    $display("HXAND %h", a & {64'h0, 64'hffff_ffff_ffff_ffff});
    $display("HXADD %h", a + 128'h1);
    b = {64'hzzzz_zzzz_zzzz_zzzz, 64'h5};
    $display("HZ %h", b);
    $display("HZSH %h", b << 4);
    $display("HZSL %h", b[127:60]);
    c = 128'h0;
    c[70:60] = 11'b1010x0z1z01;
    $display("PZ %b", c[75:55]);
    $display("DECX %d", a);
    $display("DECZ %d", b);
  end
endmodule

//> ADD 0123456789abcdf0fedcba9876543211
//> SUB 0123456789abcdeefedcba987654320f
//> AND 00000000000000010000000000000000
//> OR  0123456789abcdeffedcba9876543211
//> XOR 0123456789abcdeefedcba9876543211
//> NOT fedcba98765432100123456789abcdef
//> SHL4 123456789abcdeffedcba98765432100
//> SHR4 00123456789abcdeffedcba987654321
//> SHL60 ffedcba9876543210000000000000000
//> SHL64 fedcba98765432100000000000000000
//> SHR68 000000000000000000123456789abcde
//> REP ffffffffffffffffffffffffffffffff
//> FN 00000000000000010000000000000000
//> CAT 0123456789abcdeffedcba987654321000000000000000010000000000000001
//> PART fedcba98765432100000000000000001
//> BIT 00000010000
//> PINS 000001011010010100000
//> MUL 0000000000000000000000000000000300000000000000000000000000000000
//> DIV 0000000000000000000000000000000100000000000000000000000000000000
//> MOD 2
//> SDIV -2 -1
//> ASR ffffffffffffffffffffffffffffffff
//> DECS                                       -5
//> COMP 00000000000000010000000000000002
//> PRE xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
//> STROBE 00000000000000000000000000000022
//> NET 000000000000000000000000000000f0
//> PORT 00000000000000000000000000000002
//> BLK 00000000000000000000000000000011
//> NBA 00000000000000000000000000000022
//> XZ
//> HX xxxxxxxxxxxxxxxxffffffffffffffff
//> HXAND 0000000000000000ffffffffffffffff
//> HXADD xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
//> HZ zzzzzzzzzzzzzzzz0000000000000005
//> HZSH zzzzzzzzzzzzzzz00000000000000050
//> HZSL zzzzzzzzzzzzzzzz0
//> PZ 000001010x0z1z0100000
//> DECX                                       X
//> DECZ                                       Z
