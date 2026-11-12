// IEEE 1800-2023 23.3.3.3: a ref port is the variable it connects to. A
// write made through either name is seen at once through the other, for
// integral, struct, class handle and unpacked array variables alike.
//! icarus-skip: Icarus does not support ref ports
typedef struct packed { logic [7:0] lo; logic [7:0] hi; } word_t;
class Token; int id; endclass
module worker(ref int count, ref word_t w, ref Token t, ref byte mem[3]);
  task automatic tick(); count += 2; w.hi = w.lo ^ 8'hff; endtask
  initial begin
    #2 tick();
    $display("worker count=%0d", count);
    t = new; t.id = 11;
    mem[1] = 8'h5a;
  end
endmodule
module top;
  int count = 3;
  word_t w = '{lo: 8'h0f, hi: 8'h00};
  Token t;
  byte mem[3];
  worker k(.count(count), .w(w), .t(t), .mem(mem));
  initial begin
    #2;
    #0 $display("top count=%0d w=%h id=%0d mem=%p", count, w, t.id, mem);
    count = 100;
    $display("seen through k: %0d", k.count);
  end
endmodule

//> worker count=5
//> top count=5 w=0ff0 id=11 mem='{0, 90, 0}
//> seen through k: 100
