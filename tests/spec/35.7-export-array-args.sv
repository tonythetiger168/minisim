// IEEE 1800-2023 35.7 / 35.5.6: sized unpacked array arguments of an
// exported function or task. C passes a pointer to the first element (left
// index first); inputs are copied in, outputs and inouts copied back.
//! args: data/dpi/exportarr.c
//! icarus-skip: Icarus 13 does not support DPI
module top;
  export "DPI-C" function sv_weighted;
  export "DPI-C" function sv_fill;
  export "DPI-C" task sv_scale;
  import "DPI-C" context function int c_run();
  function int sv_weighted(input int a[4]);
    int s;
    s = 0;
    foreach (a[i]) s += a[i] * (i + 1);
    return s;
  endfunction
  function void sv_fill(output byte o[3], input string tag);
    o = '{8'h11, 8'h22, 8'h33};
    $display("fill %s", tag);
  endfunction
  task sv_scale(inout real r[2]);
    r[0] *= 2;
    r[1] += 0.5;
  endtask
  initial $display("c_run %0d", c_run());
endmodule
//> fill x
//> C: sum=30 o=11 22 33 r=3 2.5
//> c_run 30
