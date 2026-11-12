// IEEE 1800-2023 37.17, 37.18, 37.29: an unpacked struct variable and its
// members (vpiMember: read, written, nested), a dynamic array, a queue and
// an associative array (vpiArrayVar with vpiArrayType, vpiSize, elements
// by index and by iteration, read and written), a class variable and the
// object it refers to (vpiClassObj, its properties through vpiVariables).
//! icarus-skip: Icarus 13 has no SystemVerilog containers or classes in VPI
//! args: data/vpi/containers.c
module top;
  typedef struct { logic [3:0] a; int b; struct { bit c; byte d; } in; } s_t;
  class Pkt;
    int len;
    logic [7:0] kind;
    static int count = 0;
    function new(int l); len = l; kind = 8'h42; count++; endfunction
  endclass
  s_t s;
  int dyn[];
  byte q[$];
  int aa[int];
  Pkt p, none;
  initial begin
    s.a = 4'h9; s.b = -3; s.in.c = 1; s.in.d = 8'd7;
    dyn = new[3]; dyn[0] = 10; dyn[1] = 11; dyn[2] = 12;
    q.push_back(8'd1); q.push_back(8'd2);
    aa[5] = 50; aa[-1] = -10;
    p = new(64);
    $show;
    $display("s.b %0d s.in.d %0d dyn[1] %0d q[1] %0d aa[5] %0d p.len %0d", s.b, s.in.d, dyn[1], q[1], aa[5], p.len);
  end
endmodule
//> top.s: type 618 size 45
//>   top.s.a type 48 size 4 = 9
//>   top.s.b type 612 size 32 = -3
//>   top.s.in type 618 size 9
//>     top.s.in.c type 620 size 1 = 1
//>     top.s.in.d type 614 size 8 = 7
//> top.dyn: type 116 array type 2 size 3
//>   top.dyn[0] = 10
//>   top.dyn[1] = 11
//>   top.dyn[2] = 12
//> top.q: type 116 array type 4 size 2
//>   top.q[0] = 1
//>   top.q[1] = 2
//> top.aa: type 116 array type 3 size 2
//>   top.aa[-1] = -10
//>   top.aa[5] = 50
//> dyn[7]: none, aa[6]: none
//> top.p: type 615, object type 621
//>   len type 612 = 64
//>   kind type 48 = 66
//>   count type 612 = 1
//> null handle object: none
//> s.b -5 s.in.d 99 dyn[1] 21 q[1] 22 aa[5] 55 p.len 128
