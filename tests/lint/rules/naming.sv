//! args: -only naming -disable NAME018 -top BadName -set name.max_length=20 -severity NAME001=error
module BadName #(parameter width = 4,    //~ NAME001 NAME003
                 parameter Depth = 2)
               (input logic ck, input logic resetb, input logic start, output logic done_o,   //~ NAME008 NAME008 NAME008 NAME006
                input logic sysclock_i, wipe_n_i, output logic result);                        //~ NAME008 NAME005
  logic BadSig;                           //~ NAME002
  logic good_sig, Good_Sig;               //~ NAME002 NAME009
  logic a_very_long_signal_name_indeed;   //~ NAME010
  logic signal;                           //~ NAME012
  typedef logic [3:0] nibble;             //~ NAME013
  typedef logic [3:0] nibble_t;
  logic strobe;                           //~ NAME004
  always_ff @(posedge strobe or negedge resetb)
    if (!resetb) BadSig <= 1'b0;
    else         BadSig <= start;
  always_ff @(posedge ck) good_sig <= BadSig;
  always_ff @(posedge sysclock_i or negedge wipe_n_i)
    if (!wipe_n_i) Good_Sig <= 1'b0;
    else           Good_Sig <= start;
  naming_leaf leaf(.a_i(good_sig), .y_o(done_o));     //~ NAME007
  naming_leaf u_leaf(.a_i(good_sig), .y_o(result));
  if (width > 2) begin                    //~ NAME011
    logic inner;
    assign inner = start;
  end
  if (Depth > 1) begin : g_named
    logic inner;
    assign inner = start;
  end
endmodule

module naming_leaf(input logic a_i, output logic y_o);
  assign y_o = a_i;
endmodule
