// IEEE 1800-2023 37.17, 37.18, 37.29: containers whose elements are not
// plain values. An element of a queue of unpacked structs is a vpiStructVar
// whose members (vpiMember) read and write the element; an element of a
// dynamic array of class handles is a vpiClassVar that gives its object
// (vpiClassObj) and the object's properties; an element of an associative
// array of queues is itself a container (vpiArrayVar, vpiArrayType,
// vpiSize, elements by index), named q[1], m["x"][0].
//! icarus-skip: Icarus 13 has no SystemVerilog containers or classes in VPI
//! args: data/vpi/nested.c
module top;
  typedef struct { int a; logic [7:0] b; } s_t;
  class C;
    int v;
    function new(int x); v = x; endfunction
  endclass
  s_t q[$];
  C cs[];
  int m[string][$];
  initial begin
    s_t e;
    e.a = 5; e.b = 8'h3c; q.push_back(e);
    e.a = 7; e.b = 8'h01; q.push_back(e);
    cs = new[2]; cs[0] = new(11); cs[1] = new(22);
    m["x"].push_back(1); m["x"].push_back(2); m["y"].push_back(9);
    $nested;
    $display("q[1].a %0d q[0].b %h m[x][1] %0d", q[1].a, q[0].b, m["x"][1]);
  end
endmodule
//> top.q: type 116 array type 4 size 2 elements 618
//>   top.q[0] type 618: a=5 b=60
//>   top.q[1] type 618: a=7 b=1
//> top.cs: type 116 array type 2 size 2 elements 615
//>   top.cs[0] type 615 -> object v=11
//>   top.cs[1] type 615 -> object v=22
//> top.m: type 116 array type 3 size 2 elements 116
//>   top.m["x"] type 116 array type 4 size 2: 1 2
//>   top.m["y"] type 116 array type 4 size 1: 9
//> q[1].a = 70, m["x"][1] = 20
//> q[1].a 70 q[0].b 3c m[x][1] 20
