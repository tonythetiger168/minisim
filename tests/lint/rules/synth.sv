//! args: -only synth -enable SYN011 -top synth
// Synthesizability. A marker at the end of a line expects that rule on the line.
module synth(input logic clk, rst_n, input logic [7:0] a, b, output logic [7:0] y, q,
             output logic t, inout wire pad);
  real r;                                   //~ SYN006
  string s;                                 //~ SYN013
  int dyn[];                                //~ SYN013
  logic [7:0] m, d, e;
  logic bus;
  event ev;                                 //~ SYN013

  initial q = 8'd0;                         //~ SYN001
  initial $display("hello");                // no assignment: nothing to report

  assign #2 y = a & b;                      //~ SYN002
  assign bus = a[0] ? 1'bz : b[0];          //~ SYN016
  assign pad = a[1] ? 1'bz : b[1];          // an inout port may be tri-stated

  always_ff @(posedge clk) begin
    m <= #1 a;                              //~ SYN002
    d <= a / b;                             //~ SYN014
    e <= a / 8'd4;                          // a power of two is a shift
    if (a == 8'bx) m <= 8'd0;               //~ SYN009
    if (a === b) m <= 8'd1;                 //~ SYN010
    if (b[0]) m <= $random;                 //~ SYN003
    if (b[1]) m <= 'x;                      //~ SYN011
    assert (a !== 8'hxx);                   // assertions may compare with X
  end

  always @(posedge clk or a) t <= a[0];     //~ SYN007

  always begin
    @(posedge clk);                         //~ SYN012
    force bus = 1'b0;                       //~ SYN004
    @(posedge clk);                         //~ SYN012
    release bus;                            //~ SYN004
  end

  always @(posedge clk) begin
    fork                                    //~ SYN005
      d <= a;
    join
    -> ev;                                  //~ SYN005
    wait (a[0]) e <= b;                     //~ SYN005
  end

  always @(posedge clk) begin
    forever begin                           //~ SYN008
      @(negedge clk) e <= a;                //~ SYN008
    end
  end

  synth_leaf u_leaf(.i(a[0]), .o());
  defparam u_leaf.P = 2;                    //~ SYN015
  always_comb m[0] = u_leaf.inner;          //~ SYN017

  // synthesis translate_off
  always @(posedge clk) #1 d <= $random;    // inside translate_off: waived
  // synthesis translate_on
endmodule

module synth_leaf #(parameter P = 1) (input logic i, output logic o);
  logic inner;
  assign inner = i;
  assign o = inner;
endmodule

// A testbench (no ports) is not design code.
module synth_tb;
  logic clk = 0;
  real period = 5.0;
  always #5 clk = ~clk;
  initial begin
    #100 $finish;
  end
endmodule
