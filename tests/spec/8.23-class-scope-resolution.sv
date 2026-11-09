// IEEE 1800-2023 8.23: The class scope operator :: names static members,
// typedefs and enums declared inside a class.
module top;
  class Base;
    typedef enum {BIN, OCT, DEC, HEX} radix;
    typedef int my_int;
    static int count = 4;
    static function string fmt(radix r);
      case (r) BIN: return "b"; OCT: return "o"; DEC: return "d"; default: return "h"; endcase
    endfunction
  endclass
  Base::radix r;
  Base::my_int k;
  initial begin
    r = Base::HEX;
    k = Base::count + 1;
    $display("%s %s %0d %0d", Base::fmt(r), Base::fmt(Base::OCT), k, r);
  end
endmodule

//> h o 5 3
