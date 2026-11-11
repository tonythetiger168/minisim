// (* parallel_case *) on a one-hot style case: synthesis may build it as AND-OR
// with no priority (FLAT). The two differ only when two arms match at once,
// which the attribute says does not happen.
module parcase(input logic a, b, c, input logic [3:0] x, y, z, output logic [3:0] o);
`ifdef FLAT
`ifdef BUG
  assign o = ({4{a}} & x) | ({4{b}} & y) | ({4{c}} & y);
`else
  assign o = ({4{a}} & x) | ({4{b}} & y) | ({4{c}} & z);
`endif
`else
  always @* begin
    o = '0;
    (* parallel_case *)
    case (1'b1)
      a: o = x;
      b: o = y;
      c: o = z;
    endcase
  end
`endif
endmodule
