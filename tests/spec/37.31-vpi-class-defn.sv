// IEEE 1800-2023 37.31: a class defn's vpiVariables gives its static and
// automatic properties (detail 1) with vpiAutomatic, as declarations whose
// value vpi_get_value refuses (detail 2); vpiNamedEvent its events,
// vpiParameter its port-list and body parameters with vpiLocalParam
// (detail 7), vpiTypedef its typedefs, vpiInternalScope its tasks and
// functions; vpiAutomatic of the class defn is 1.
//! icarus-skip: Icarus 13 has no SystemVerilog classes through VPI
//! args: data/vpi/classdefn.c
module top;
  class Fifo #(parameter int DEPTH = 4);
    localparam int W = 8;
    typedef logic [W-1:0] word_t;
    word_t data[$];
    static int made;
    event full;
    function void push(word_t w); data.push_back(w); endfunction
    task drain(); data.delete(); endtask
  endclass
  Fifo #(8) f;
  initial begin
    f = new;
    f.push(8'h5);
    $defn;
  end
endmodule
//> class Fifo automatic 1
//>   variable data type 116 automatic 1
//>   variable made type 612 automatic 0
//>   event full type 34
//>   parameter DEPTH = 8 local 0
//>   parameter W = 8 local 1
//>   typedef word_t
//>   scope push
//>   scope drain
//>   parent Fifo
//>   value refused: 1
