// Function calls in combinational blocks. msim expands them in place so the
// block runs in the static schedule; a function's arguments, result and
// locals become private to each expanded call only when no value can travel
// between callers through them.
//   - enc / fld / pack: automatic functions, nested, with a local built by
//     part selects and a unique case that returns from every arm
//   - keep: a static function whose result is not written on every path, so
//     a call can return what an earlier call left there (stays shared; it
//     has one caller, as two would race for the stored value)
//   - bump: writes a module variable (a side effect another block reads)
//   - enc is also called from a clocked block
module combcall;
  logic clk = 0;
  logic [7:0] cnt = 0;
  always #5 clk = ~clk;
  always_ff @(posedge clk) cnt <= cnt + 8'd7;

  function automatic logic [3:0] fld(input logic [2:0] k);
    unique case (k)
      3'd0, 3'd1: return 4'd3;
      3'd2:       return 4'd9 + {1'b0, k};
      3'd3, 3'd4: return {k, 1'b1};
      default:    return 4'd0;
    endcase
  endfunction

  function automatic logic [15:0] pack(input logic [2:0] k, input logic [4:0] off);
    logic [15:0] w;
    logic signed [7:0] neg;
    neg = -signed'({1'b0, off, 2'b00});
    w[3:0]   = fld(k);
    w[7:4]   = 4'b1010;
    w[15:8]  = unsigned'(neg);
    return w;
  endfunction

  function automatic logic [15:0] enc(input logic [7:0] v, input logic alt = 1'b0);
    return alt ? pack(v[2:0], v[7:3]) ^ 16'h00ff : pack(v[5:3], v[4:0]);
  endfunction

  function logic [7:0] keep(input logic [7:0] v);
    if (v[0]) keep = v ^ 8'h3c;
  endfunction

  logic [7:0] last;
  function automatic logic [7:0] bump(input logic [7:0] v);
    last = v + 8'd1;
    return v << 1;
  endfunction

  logic [15:0] a, b, c;
  logic [7:0]  k1, s, t;
  always_comb begin
    a = enc(cnt);
    b = 16'h0;
    if (cnt[7]) b = enc(~cnt, 1'b1);
    else        b = {8'h0, fld(cnt[2:0]), fld(cnt[6:4])};
  end
  assign c = enc(cnt ^ 8'h5a, cnt[1]);
  always_comb k1 = keep(cnt);
  always_comb s = bump(cnt);
  always_comb t = last ^ 8'hf0;

  logic [15:0] r = 0;
  always_ff @(posedge clk) r <= enc(cnt + 8'd3) + r;

  int unsigned sum = 0;
  always @(posedge clk) sum <= sum * 31 + a + b + c + ($isunknown(k1) ? 8'd1 : k1) + s + t + r;
  initial begin
    #3000 $display("checksum=%0d", sum);
    $finish;
  end
endmodule
