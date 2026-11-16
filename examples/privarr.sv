// msim -compile keeps elements of unpacked arrays and signals inside
// generate loops (names with '[') in the cycle domain's private storage,
// like any other signal. Elements a run-time index reaches stay in the
// shared table. A generate loop of units, an unpacked wire array written
// element by element, a register array indexed by a variable, and a
// $display of one element by name while the design runs.
module paunit(input clk, input rst, input [7:0] din, output [15:0] q);
  reg [7:0] st [0:2];
  wire [7:0] nx [0:2];
  assign nx[0] = st[0] + din;
  assign nx[1] = st[1] ^ nx[0];
  assign nx[2] = {st[2][6:0], st[2][7]} + nx[1];
  always @(posedge clk)
    if (rst) begin st[0] <= 0; st[1] <= 8'h5a; st[2] <= 8'h3c; end
    else begin st[0] <= nx[0]; st[1] <= nx[1]; st[2] <= nx[2]; end
  assign q = {st[2], st[1] ^ st[0]};
endmodule
module privarr;
  reg clk = 0, rst = 1;
  wire [15:0] q [0:15];
  reg [15:0] hist [0:7];
  reg [2:0] wp = 0;
  genvar g;
  generate
    for (g = 0; g < 16; g = g + 1) begin : gu
      wire [7:0] seed = 8'd3 * g + 8'd1;
      paunit u(.clk(clk), .rst(rst), .din(seed), .q(q[g]));
    end
  endgenerate
  wire [15:0] mix = q[0] ^ q[1] ^ q[2] ^ q[3] ^ q[4] ^ q[5] ^ q[6] ^ q[7] ^
                    q[8] ^ q[9] ^ q[10] ^ q[11] ^ q[12] ^ q[13] ^ q[14] ^ q[15];
  always @(posedge clk) begin   // run-time index: hist stays shared
    hist[wp] <= mix;
    wp <= wp + 1;
  end
  always #1 clk = ~clk;
  integer r;
  reg [15:0] acc;
  initial begin
    #4 rst = 0;
    for (r = 0; r < 4; r = r + 1) begin
      #100;
      acc = hist[0] ^ hist[3] ^ hist[7];
      $display("t=%0t mix=%h q5=%h st=%h hist=%h", $time, mix, q[5], gu[9].u.st[1], acc);
    end
    $finish;
  end
endmodule
