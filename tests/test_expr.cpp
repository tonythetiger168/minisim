#include "expr.hpp"
#include <cassert>
#include <iostream>
int main(){
  Val X=allx(4), z=mk(0,4), one=mk(1,4);
  assert(v_and(X,z).x==0 && v_and(X,z).v==0);      // X & 0 = 0
  assert(v_or(X,mk(0xF,4)).x==0 && v_or(X,mk(0xF,4)).v==0xF); // X | 1 = 1
  assert(v_add(X,one).x==0xF);
  assert(v_add(mk(15,4),one).v==0);                // wraps at 4 bits
  assert(v_eq(make(0b10,0b01,2),mk(0b00,2)).v==0 && v_eq(make(0b10,0b01,2),mk(0b00,2)).x==0); // known bit differs
  assert(v_eq(X,z).x==1);
  assert(v_concat({mk(0xA,4),mk(0x5,4)}).v==0xA5 && v_concat({mk(0xA,4),mk(0x5,4)}).w==8);
  assert(v_slice(mk(0xA5,8),7,4).v==0xA);
  Val m=v_mux(allx(1),mk(0b1100,4),mk(0b1010,4)); assert(m.v==0b1000 && m.x==0b0110);
  assert(v_red_and(mk(15,4)).v==1 && v_red_or(z).v==0 && v_red_xor(mk(7,4)).v==1);
  std::vector<int> regs={5};
  auto e=bin(Op::Add,ref(0),konst(mk(1,4)));
  assert(eval(e,[&](int s){return mk(regs[s],4);}).v==6);

  // Signedness is a tag on the value, not a second encoding of the bits.
  // 4'sb1000 is -8. The same bits unsigned are 8.
  Val s4 = make(0b1000, 0, 4, 0, true);
  Val u4 = mk(0b1000, 4);
  Val s1 = make(1, 0, 4, 0, true);
  assert(v_lt(s4, s1).v == 1 && v_lt(s4, s1).x == 0);
  assert(v_lt(u4, mk(1, 4)).v == 0);
  assert(v_le(s4, s4).v == 1 && v_gt(s4, s1).v == 0 && v_ge(s1, s4).v == 1);
  // Mixed: either side unsigned => the compare zero-extends.
  assert(v_lt(s4, mk(1, 4)).v == 0);
  // Different widths, both signed: sign-extend, then signed compare.
  Val s32 = make(1, 0, 32, 0, true);
  assert(v_lt(s4, s32).v == 1);
  assert(v_eq(make(0b1111, 0, 4, 0, true), make(1, 0, 1, 0, true)).v == 1);
  assert(v_eq(make(0b1111, 0, 4, 0, true), mk(1, 1)).v == 0);
  // 4-bit -8 + -8 wraps to 0. In 8 bits it is -16.
  Val sum4 = v_add(s4, s4);
  assert(sum4.w == 4 && sum4.v == 0 && sum4.sgn);
  Val se = v_resize(s4, 8, true);
  se.sgn = true;
  Val sum8 = v_add(se, se);
  assert(sum8.w == 8 && sum8.v == 0xF0 && sum8.sgn);
  assert(v_sub(se, make(1, 0, 8, 0, true)).v == 0xF7); // -9
  assert(v_mul(se, make(2, 0, 8, 0, true)).v == 0xF0); // -16
  // Signed + unsigned is unsigned, so -8 is read as 8.
  Val mix = v_add(s4, mk(1, 4));
  assert(mix.v == 9 && mix.w == 4 && !mix.sgn);
  // >>> sign-fills. >> of a signed value zero-fills and stays signed.
  assert(v_asr(s4, mk(1)).v == 0b1100 && v_asr(s4, mk(1)).sgn);
  assert(v_asr(s4, mk(4, 8)).v == 0xF);
  assert(v_shr(s4, mk(1)).v == 0b0100 && v_shr(s4, mk(1)).sgn);
  assert(v_asr(u4, mk(1)).v == 0b0100 && !v_asr(u4, mk(1)).sgn);
  // X/Z in the sign bit replicates. An X anywhere else makes a compare X.
  Val x000 = make(0, 0b1000, 4, 0, true);
  Val ax = v_asr(x000, mk(1));
  assert(ax.x == 0b1100 && ax.z == 0 && ax.v == 0);
  Val z000 = make(0, 0b1000, 4, 0b1000, true);
  Val az = v_asr(z000, mk(1));
  assert(az.x == 0b1100 && az.z == 0b1100);
  assert(v_lt(make(0b1000, 0b0001, 4, 0b0001, true), s1).x == 1);
  Val sx = v_resize(x000, 8, true);
  assert(sx.w == 8 && sx.x == 0xF8 && sx.v == 0);
  // The ref carries the tag. Storage is still the unsigned bit pattern.
  auto cmp = bin(Op::Lt, ref(0, true), konst(s32));
  assert(eval(cmp, [&](int) { return u4; }).v == 1);
  auto ext = resize(ref(0, true), 8, true, true);
  Val ev = eval(ext, [&](int) { return u4; });
  assert(ev.w == 8 && ev.v == 0xF8 && ev.sgn);

  Val m2 = make(0b1110, 0, 4, 0, true);
  Val p2 = make(0b0010, 0, 4, 0, true);
  Val sq = v_div(m2, p2);
  assert(sq.v == 0b1111 && sq.x == 0 && sq.z == 0 && sq.w == 4 && sq.sgn);
  assert(v_mod(m2, p2).v == 0 && v_mod(m2, p2).sgn);
  assert(v_div(mk(0b1110, 4), mk(0b0010, 4)).v == 7 && !v_div(mk(0b1110, 4), mk(2, 4)).sgn);
  assert(v_div(mk(7, 4), mk(3, 4)).v == 2 && v_mod(mk(7, 4), mk(3, 4)).v == 1);
  assert(v_div(mk(0, 4), mk(1, 4)).v == 0 && v_div(mk(0, 4), mk(1, 4)).x == 0);
  assert(v_mod(mk(0, 4), mk(1, 4)).v == 0);
  Val m7 = make(0b1001, 0, 4, 0, true);
  Val p3 = make(0b0011, 0, 4, 0, true);
  Val n3 = make(0b1101, 0, 4, 0, true);
  Val p7 = make(0b0111, 0, 4, 0, true);
  assert(v_div(m7, p3).v == 0b1110 && v_mod(m7, p3).v == 0b1111);
  assert(v_div(p7, n3).v == 0b1110 && v_mod(p7, n3).v == 0b0001);
  assert(v_div(m7, n3).v == 0b0010 && v_mod(m7, n3).v == 0b1111);
  Val m8 = make(0b1000, 0, 4, 0, true);
  Val mixed = v_div(m8, mk(2, 4));
  assert(mixed.v == 4 && mixed.w == 4 && !mixed.sgn);
  assert(v_mod(m8, mk(3, 4)).v == 2 && !v_mod(m8, mk(3, 4)).sgn);
  assert(v_div(mk(7, 4), n3).v == 0 && !v_div(mk(7, 4), n3).sgn);
  assert(v_mod(mk(7, 4), n3).v == 7);
  Val m1 = make(0b1111, 0, 4, 0, true);
  assert(v_div(m8, m1).v == 0b1000 && v_mod(m8, m1).v == 0 && v_div(m8, m1).sgn);
  Val m8w = v_resize(m8, 8, true); m8w.sgn = true;
  Val m1w = v_resize(m1, 8, true); m1w.sgn = true;
  Val wide = v_div(m8w, m1w);
  assert(wide.v == 0x08 && wide.w == 8 && wide.sgn && wide.x == 0);
  Val p3w = make(3, 0, 8, 0, true);
  assert(v_div(m8, p3w).v == 0xFE && v_div(m8, p3w).w == 8 && v_div(m8, p3w).sgn);
  assert(v_mod(m8, p3w).v == 0xFE);
  Val smin = make(1ULL << 63, 0, 64, 0, true);
  Val sm1 = make(~0ULL, 0, 64, 0, true);
  Val s3 = make(3, 0, 64, 0, true);
  assert(v_div(smin, sm1).v == (1ULL << 63) && v_div(smin, sm1).x == 0 && v_div(smin, sm1).sgn);
  assert(v_mod(smin, sm1).v == 0 && v_mod(smin, sm1).x == 0);
  assert(v_div(smin, s3).v == 0xD555555555555556ULL && v_div(smin, s3).sgn);
  assert(v_mod(smin, s3).v == 0xFFFFFFFFFFFFFFFEULL);
  assert(v_div(make(0b1000, 0b0001, 4), p2).x == 0xF && v_div(make(0b1000, 0b0001, 4), p2).v == 0);
  assert(v_mod(p7, make(0b0010, 0b0001, 4)).x == 0xF);
  Val zx = v_div(allz(4), mk(2, 4));
  assert(zx.x == 0xF && zx.z == 0 && zx.w == 4);
  Val zd = v_mod(mk(8, 4), allz(4));
  assert(zd.x == 0xF && zd.z == 0);
  assert(v_div(make(0, 0b1000, 4, 0b1000, true), p2).x == 0xF);
  assert(v_div(mk(7, 4), mk(0, 4)).x == 0xF && v_mod(mk(7, 4), mk(0, 4)).x == 0xF);
  assert(v_div(m8, make(0, 0, 4, 0, true)).x == 0xF && v_mod(m8, make(0, 0, 4, 0, true)).x == 0xF);
  assert(v_div(mk(0, 4), mk(0, 4)).x == 0xF);
  auto ed = bin(Op::Div, ref(0, true), ref(1, true));
  Val edv = eval(ed, [&](int s) { return s == 0 ? mk(0b1110, 4) : mk(0b0010, 4); });
  assert(edv.v == 0b1111 && edv.w == 4 && edv.sgn && edv.x == 0);
  auto em = bin(Op::Mod, konst(mk(7, 4)), konst(mk(0, 4)));
  Val emv = eval(em, [](int) { return mk(0, 1); });
  assert(emv.x == 0xF && emv.w == 4);

  // Wider than one limb, and wider than two. Limb 0 is the low 64 bits.
  auto wv = [](std::initializer_list<uint64_t> limbs, int w, bool sgn = false) {
    Val a = make(0, 0, w, 0, sgn);
    int i = 0;
    for (uint64_t limb : limbs) { setLimb(a, i, limb, 0, 0); i++; }
    return finish(a);
  };
  Val a128 = wv({0x0123456789abcdefULL, 0xfedcba9876543210ULL}, 128);
  Val one128 = wv({1, 0}, 128);
  assert(a128.w == 128 && a128.v == 0x0123456789abcdefULL && a128.hi.size() == 1);
  assert(a128.hi[0].v == 0xfedcba9876543210ULL);
  // Carry out of limb 0.
  Val sum = v_add(wv({~0ULL, 0x1}, 128), one128);
  assert(sum.v == 0 && sum.hi[0].v == 0x2 && !sum.x);
  assert(v_add(wv({~0ULL, ~0ULL}, 128), one128).v == 0 && v_add(wv({~0ULL, ~0ULL}, 128), one128).hi[0].v == 0);
  assert(v_sub(wv({0, 0}, 128), one128).v == ~0ULL && v_sub(wv({0, 0}, 128), one128).hi[0].v == ~0ULL);
  // Shift across the 64-bit boundary.
  Val sh64 = v_shl(one128, mk(64, 32));
  assert(sh64.v == 0 && sh64.hi[0].v == 1 && !sh64.x);
  Val sh4 = v_shl(a128, mk(4, 8));
  assert(sh4.v == 0x123456789abcdef0ULL && sh4.hi[0].v == 0xedcba98765432100ULL);
  Val shR = v_shr(a128, mk(4, 8));
  assert(shR.v == 0x00123456789abcdeULL); // low nibble came from the high limb
  assert((shR.hi[0].v & 0x0fffffffffffffffULL) == 0x0fedcba987654321ULL);
  // 256 bits: multiply across two limbs, divide back.
  Val a256 = wv({0, 0, 1, 0}, 256);          // 1 << 128
  Val three = wv({3, 0, 0, 0}, 256);
  Val prod = v_mul(a256, three);
  assert(prod.v == 0 && prod.hi[0].v == 0 && prod.hi[1].v == 3 && prod.hi[2].v == 0);
  assert(v_div(prod, three).hi[1].v == 1 && v_div(prod, three).v == 0);
  assert(v_mod(wv({100, 0, 0, 0}, 256), wv({7, 0, 0, 0}, 256)).v == 2);
  // Signed, past 64 bits.
  Val m5 = wv({~0ULL - 4, ~0ULL}, 128, true); // -5
  assert(v_div(m5, wv({2, 0}, 128, true)).v == (~0ULL - 1) && v_div(m5, wv({2, 0}, 128, true)).hi[0].v == ~0ULL);
  assert(v_mod(m5, wv({2, 0}, 128, true)).v == ~0ULL && v_mod(m5, wv({2, 0}, 128, true)).hi[0].v == ~0ULL);
  Val asr = v_asr(m5, mk(70, 32));
  assert(asr.sgn && asr.v == ~0ULL && asr.hi[0].v == ~0ULL);
  // X in the high limb contaminates arithmetic and is masked by a known 0.
  Val hx = wv({0xffffffffffffffffULL, 0}, 128);
  setLimb(hx, 1, 0, ~0ULL, 0);
  hx = finish(hx);
  assert(v_add(hx, one128).x == ~0ULL && v_add(hx, one128).hi[0].x == ~0ULL);
  Val m0 = v_and(hx, wv({~0ULL, 0}, 128));
  assert(m0.v == ~0ULL && m0.hi[0].v == 0 && m0.hi[0].x == 0);
  // Part-select and concatenation across the boundary.
  Val mid = v_slice(a128, 70, 60);
  assert(mid.w == 11);
  Val cat = v_concat({wv({0x11, 0x22}, 128), wv({0x33, 0x44}, 128)});
  assert(cat.w == 256 && cat.v == 0x33 && cat.hi[0].v == 0x44 && cat.hi[1].v == 0x11 && cat.hi[2].v == 0x22);
  Val ins = v_insert(wv({0, 0}, 128), mk(0x7ff, 11), 70, 60);
  assert(bitAt(ins, 60) == 1 && bitAt(ins, 70) == 1 && bitAt(ins, 71) == 0 && bitAt(ins, 59) == 0);
  // Z in the high limb survives a logical shift and a slice.
  Val hz = allz(128);
  hz = v_insert(hz, wv({0x5, 0}, 64), 63, 0); // low 64 known, high 64 Z
  Val moved = v_shl(hz, mk(4, 8));
  assert(bitAt(moved, 67) == 0 && bitAt(moved, 68) == 3 && bitAt(moved, 127) == 3);
  assert(v_slice(hz, 127, 64).z == ~0ULL && v_slice(hz, 127, 64).x == ~0ULL);
  assert(truth(wv({0, 1}, 128)) == 1 && truth(wv({0, 0}, 128)) == 0);
  assert(v_red_xor(wv({1, 1}, 128)).v == 0 && v_red_and(v_ones(128)).v == 1);
  assert(v_eq(a128, a128).v == 1 && v_lt(m5, wv({1, 0}, 128, true)).v == 1);
  std::cout<<"ok\n";
}
