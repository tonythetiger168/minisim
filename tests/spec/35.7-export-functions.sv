// IEEE 1800-2023 35.7: An exported SV function is called from C with its
// arguments and result mapped as for imports, under its own C name when one
// is given (35.7, 35.5.6).
//! args: data/dpi/context.c
//! icarus-skip: Icarus 13 does not support DPI
module top;
  import "DPI-C" context function int use_exports(input int a);
  export "DPI-C" sv_mul = function mul;
  export "DPI-C" function sv_half;
  export "DPI-C" function sv_name;
  int calls = 0;
  function int mul(input int a, input int b, output int hi);
    automatic longint p = longint'(a) * b;
    calls++;
    hi = p[63:32];
    return p[31:0];
  endfunction
  function real sv_half(input real x); return x / 2; endfunction
  function string sv_name(input int n); return n > 4000 ? "big" : "small"; endfunction
  initial begin
    $display("%0d", use_exports(5000));
    $display("%0d", use_exports(3));
    $display("calls=%0d", calls);
  end
endmodule

//> lo=947912704 hi=1 half=2500 name=big
//> 947912704
//> lo=3145728 hi=0 half=1.5 name=small
//> 3145728
//> calls=2
