// IEEE 1800-2023 35.5.4, 27.5: an export "DPI-C" whose subroutine is in a
// generate block that is not taken, or in a module nothing instantiates, has
// no instance and is left out; the rest of the design runs as usual.
//! args: -top top
//! icarus-skip: Icarus 13 does not support DPI
module spare_mem;
  export "DPI-C" function spare_read;
  function int spare_read(input int index);
    return index;
  endfunction
endmodule
module top;
  localparam bit WITH_CACHE = 0;
  if (WITH_CACHE) begin : g_cache
    export "DPI-C" function cache_key;
    function automatic int cache_key(output bit [127:0] key);
      key = 128'h1;
      return 1;
    endfunction
  end
  initial $display("ok");
endmodule
//> ok
