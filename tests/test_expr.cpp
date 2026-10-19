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
  std::cout<<"ok\n";
}
