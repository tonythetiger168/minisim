// A constant slice of at most 64 bits of a wide signal (a packed struct's
// field, a TL-UL port's a_address) is read in place by msim -compile
// (expr.hpp sliceOf) instead of copying the whole value (v_slice(R(...))).
module sliceof;
  typedef struct packed { logic [2:0] op; logic [31:0] addr; logic [63:0] data; logic [7:0] mask; logic v; } req_t;
  req_t r, q;
  logic clk = 0;
  logic [31:0] acc = 0;
  logic [107:0] raw;
  assign raw = r;
  always_comb q = r;
  always @(posedge clk) begin
    r.op <= r.op + 1; r.addr <= r.addr * 3 + 7; r.data <= {r.data[62:0], r.data[63] ^ r.addr[5]}; r.mask <= acc[7:0]; r.v <= ~r.v;
    acc <= acc * 31 + q.addr[31:4] + q.data[40:9] + q.mask + q.op + raw[107:100] + raw[70:60];
  end
  initial begin
    r = '0; r.data = 64'hdeadbeef12345678;
    repeat (500) begin #5 clk = 1; #5 clk = 0; end
    $display("acc=%h", acc); $finish;
  end
endmodule
