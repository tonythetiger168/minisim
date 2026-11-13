// IEEE 1800-2023 6.11.2, 10.7: a 4-state value assigned to a 2-state
// variable (bit, byte, int, a 2-state class property, or a continuous
// assignment to one) has its x and z bits turned into 0; a cast to a 2-state
// type does the same.
class holder;
  bit [3:0] nib;
  int n;
endclass
module top;
  wire        hiz = 1'bz;
  logic [7:0] mixed = 8'b10xz_01zx;
  logic       unk = 1'bx;
  bit   [7:0] cont;
  assign cont = mixed;
  bit b; byte y; int i; shortint s;
  initial begin
    holder h;
    h = new;
    #1;
    b = hiz;
    y = {8{hiz}};
    i = unk;
    s = mixed;
    h.nib = mixed[7:4];
    h.n = mixed;
    $display("b=%b y=%b i=%0d s=%b", b, y, i, s);
    $display("cont=%b nib=%b n=%0d", cont, h.nib, h.n);
    $display("cast=%b eq=%b", bit'(unk), bit'(hiz) == 1'b0);
  end
endmodule
//> b=0 y=00000000 i=0 s=0000000010000100
//> cont=10000100 nib=1000 n=132
//> cast=0 eq=1
