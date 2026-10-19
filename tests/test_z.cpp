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
  Val mm = v_mux(allx(1), mk(0b1100,4), mk(0b1010,4)); assert(mm.v==0b1000 && mm.x==0b0110 && mm.z==0);
  std::cout<<"ok\n";
}
