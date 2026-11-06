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
  std::cout<<"ok\n";
}
