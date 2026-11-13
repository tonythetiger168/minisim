//! args: -only NAME018,NAME019 -top src4
typedef struct packed {
  logic [3:0] a;
  logic v;
} pair_t;
module src4 (
    input logic clk,
    input logic rst_n,
    input logic [3:0] d
);
  logic [3:0] count_q, data;
  logic [1:0][3:0] pipe_q;
  pair_t p_q, cfg;
  logic v;
  int i;
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      count_q <= 0;
      data <= 0; //~ NAME018
    end else begin
      count_q <= count_q + 1;
      data <= d;
      pipe_q[0] <= d;
      p_q.a <= d;
      cfg.v <= 1'b1; //~ NAME018
      {v, pipe_q[1]} <= {1'b0, d}; //~ NAME018
    end
  end
  always_comb begin
    automatic logic [3:0] data = d; //~ NAME019
    for (int i = 0; i < 2; i++) begin //~ NAME019
    end
    for (int k = 0; k < 2; k++) begin
    end
    for (int k = 0; k < 2; k++) begin
    end
  end
  function automatic int f(int d); //~ NAME019
    int k;
    return d + k;
  endfunction
  function automatic int g(int x);
    begin : inner
      int x; //~ NAME019
      x = 1;
    end
    return x;
  endfunction
endmodule
class c;
  int n;
  extern function void set(int n); //~ NAME019
  function void put(int n); //~ NAME019
    this.n = n;
  endfunction
endclass
function void c::set(int n);
  this.n = n;
endfunction
