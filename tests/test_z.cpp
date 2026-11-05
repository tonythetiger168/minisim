#include "expr.hpp"
#include <cassert>
#include <iostream>
int main(){
  Val Z=allz(4), X=allx(4);
  // Z treated as X by operators, result has no Z
  assert(v_and(Z,mk(0xF,4)).x==0xF && v_and(Z,mk(0xF,4)).z==0);
  assert(v_and(Z,mk(0,4)).x==0 && v_and(Z,mk(0,4)).v==0);          // Z & 0 = 0
  assert(v_not(Z).x==0xF && v_not(Z).z==0);
  assert(v_add(Z,mk(1,4)).z==0 && v_add(Z,mk(1,4)).x==0xF);
  // === distinguishes X from Z
  assert(v_case_eq(Z,Z).v==1 && v_case_eq(Z,X).v==0 && v_case_eq(X,X).v==1);
  // slice / concat / insert / index keep Z
  Val m = v_concat({mk(0xA,4), Z});               // 8'hA_z
  assert(m.w==8 && m.z==0x0F && m.x==0x0F && m.v==0xA0);
  assert(v_slice(m,3,0).z==0x0F && v_slice(m,7,4).z==0);
  Val ins = v_insert(mk(0,8), Z, 3, 0);  assert(ins.z==0x0F);
  assert(v_index(m,mk(1,3)).z==1);
  // casez: 4'b1zz0 style item (z bits wildcards)
  Val item = make(0b1000, 0b0110, 4, 0b0110);     // 1zz0
  assert(v_casez_eq(mk(0b1010,4), item).v==1);    // 1010 matches
  assert(v_casez_eq(mk(0b1110,4), item).v==1);    // 1110 matches
  assert(v_casez_eq(mk(0b0010,4), item).v==0);    // msb differs
  assert(v_casez_eq(mk(0b1011,4), item).v==0);    // lsb differs
  // casez does NOT wildcard X
  Val itemx = make(0b1000, 0b0110, 4, 0);         // 1xx0
  assert(v_casez_eq(mk(0b1010,4), itemx).v==0);
  assert(v_casez_eq(make(0b1000,0b0110,4), itemx).v==1); // X matches X exactly
  // casex wildcards both X and Z
  assert(v_casex_eq(mk(0b1010,4), itemx).v==1);
  assert(v_casex_eq(mk(0b1010,4), item).v==1);
  assert(v_casex_eq(mk(0b0010,4), item).v==0);
  // equality now sees X vs Z as different
  assert(!(Z==X));
  // mux with unknown cond: equal known bits survive, rest X
  // Icarus 13.0 (v13_0) and 12.0: 1'bx ? 4'b1100 : 4'b1010 = 1xx0
  Val mm = v_mux(allx(1), mk(0b1100,4), mk(0b1010,4)); assert(mm.v==0b1000 && mm.x==0b0110 && mm.z==0);

  // Shifts and ?: below: checked against Icarus Verilog 13.0 (stable) (v13_0),
  // tag v13_0 dfeee909. Icarus 12.0 printed the same string for every case.
  // 4'bzzzz << 1 = zzz0. Vacated bit is 0; shifted Z stays Z.
  Val zzz0 = v_shl(Z, mk(1));
  assert(zzz0.v==0 && zzz0.x==0xE && zzz0.z==0xE);
  // 4'bzz10 << 1 = z100
  Val zz10 = make(0b0010, 0b1100, 4, 0b1100);
  Val z100 = v_shl(zz10, mk(1));
  assert(z100.v==0b0100 && z100.x==0b1000 && z100.z==0b1000);
  // 4'bzz10 >> 1 = 0zz1
  Val zz1 = v_shr(zz10, mk(1));
  assert(zz1.v==0b0001 && zz1.x==0b0110 && zz1.z==0b0110);
  // 4'bzzzz >> 1 = 0zzz
  Val zzzz_r = v_shr(Z, mk(1));
  assert(zzzz_r.v==0 && zzzz_r.x==0x7 && zzzz_r.z==0x7);
  // 4'b10zz << 1 = 0zz0
  Val tenzz = make(0b1000, 0b0011, 4, 0b0011);
  Val zerzz = v_shl(tenzz, mk(1));
  assert(zerzz.v==0 && zerzz.x==0b0110 && zerzz.z==0b0110);
  // << 0 keeps Z; << 4 of a 4-bit value is 0000
  assert(v_shl(zz10, mk(0)).z==0xC && v_shl(zz10, mk(0)).v==0b0010);
  // Unsized 4 in Icarus is a 32-bit count; mk() defaults to 1 bit, so width is explicit.
  assert(v_shl(zz10, mk(4, 32)).v==0 && v_shl(zz10, mk(4, 32)).x==0 && v_shl(zz10, mk(4, 32)).z==0);
  // X or Z shift count, including a partially-X count, is all X (no Z).
  // Icarus: 4'b1010 << 1'bx / 1'bz / 2'b0x, and >> 1'bx / 1'bz, all print xxxx.
  assert(v_shl(mk(0xA,4), allx(1)).x==0xF && v_shl(mk(0xA,4), allx(1)).z==0);
  assert(v_shl(mk(0xA,4), allz(1)).x==0xF && v_shl(mk(0xA,4), allz(1)).z==0);
  assert(v_shl(Z, allx(1)).x==0xF && v_shl(Z, allx(1)).z==0);
  assert(v_shr(mk(0xA,4), allx(1)).x==0xF && v_shr(mk(0xA,4), allz(1)).z==0);
  assert(v_shl(mk(0xA,4), make(0, 0b01, 2)).x==0xF && v_shl(mk(0xA,4), make(0, 0b01, 2)).z==0);
  // 1'bx ? 4'bzzzz : 4'bzzzz = zzzz. Both-Z bits stay Z.
  Val mzz = v_mux(allx(1), Z, Z);
  assert(mzz.v==0 && mzz.x==0xF && mzz.z==0xF);
  // 1'bz condition is the same merge. Icarus: zzzz.
  assert(v_mux(allz(1), Z, Z).z==0xF && v_mux(allz(1), Z, Z).x==0xF);
  // 1'bx ? 4'b1z0x : 4'b1z1x = 1zxx
  Val arm_a = make(0b1000, 0b0101, 4, 0b0100); // 1z0x
  Val arm_b = make(0b1010, 0b0101, 4, 0b0100); // 1z1x
  Val m1z = v_mux(allx(1), arm_a, arm_b);
  assert(m1z.v==0b1000 && m1z.x==0b0111 && m1z.z==0b0100);
  // Identical mixed arms stay as they are. Icarus: 1z0x
  Val msame = v_mux(allx(1), arm_a, arm_a);
  assert(msame.v==0b1000 && msame.x==0b0101 && msame.z==0b0100);
  // Z against X is not equal. Icarus: 4'bzzzz ?vs? 4'bxxxx = xxxx
  assert(v_mux(allx(1), Z, X).x==0xF && v_mux(allx(1), Z, X).z==0);
  // 1'bx ? 4'b0z10 : 4'bz010 = xx10
  Val p = make(0b0010, 0b0100, 4, 0b0100);
  Val q = make(0b0010, 0b1000, 4, 0b1000);
  Val mx = v_mux(allx(1), p, q);
  assert(mx.v==0b0010 && mx.x==0b1100 && mx.z==0);
  // 1'bx ? 4'bzzx1 : 4'bzxx1 = zxx1
  Val r1 = make(0b0001, 0b1110, 4, 0b1100);
  Val r2 = make(0b0001, 0b1110, 4, 0b1000);
  Val mr = v_mux(allx(1), r1, r2);
  assert(mr.v==0b0001 && mr.x==0b1110 && mr.z==0b1000);
  // Known condition selects that arm, Z included. Icarus: 1'b1 selects zzzz, 1'b0 selects 0000.
  assert(v_mux(mk(1), Z, mk(0,4)).z==0xF);
  assert(v_mux(mk(0), Z, mk(0,4)).v==0 && v_mux(mk(0), Z, mk(0,4)).x==0 && v_mux(mk(0), Z, mk(0,4)).z==0);
  // Equal known arms. Icarus: 1'bx ? 4'b1010 : 4'b1010 = 1010
  assert(v_mux(allx(1), mk(0xA,4), mk(0xA,4)).v==0xA && v_mux(allx(1), mk(0xA,4), mk(0xA,4)).x==0);
  std::cout<<"ok\n";
}
