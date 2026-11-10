// Hierarchy, parameters, generate loops, instance arrays, gate primitives,
// loops with break and continue, part selects with a variable base.
module full_adder (input logic a, b, ci, output logic s, co);
    wire t;
    xor g1 (t, a, b);
    xor g2 (s, t, ci);
    assign co = (a & b) | (t & ci);
endmodule

module ripple #(parameter int W = 4) (input logic [W-1:0] a, b, input logic ci, output logic [W-1:0] s, output logic co);
    logic [W:0] c;
    assign c[0] = ci;
    for (genvar i = 0; i < W; i++) begin : g
        full_adder fa (.a(a[i]), .b(b[i]), .ci(c[i]), .s(s[i]), .co(c[i+1]));
    end
    assign co = c[W];
endmodule

module pipe #(parameter int W = 8, parameter int N = 2) (input logic clk, input logic [W-1:0] d, output logic [W-1:0] q);
    logic [W-1:0] st [N];
    always_ff @(posedge clk) begin
        st[0] <= d;
        for (int i = 1; i < N; i++) st[i] <= st[i-1];
    end
    assign q = st[N-1];
endmodule

module hier (
    input  logic        clk,
    input  logic [11:0] a, b,
    input  logic [2:0]  sel,
    output logic [11:0] sum,
    output logic        co,
    output logic [3:0]  first_one,
    output logic [3:0]  nib,
    output logic [4:0]  evens,
    output logic [11:0] delayed
);
    logic [11:0] s;
    logic c1;
    ripple #(.W(12)) add (.a(a), .b(b), .ci(1'b0), .s(s), .co(c1));
    pipe #(.W(13), .N(3)) p (.clk(clk), .d({c1, s}), .q({co, sum}));
    pipe #(.W(12)) p2 (.clk, .d(a ^ b), .q(delayed));
    always_comb begin
        first_one = 4'd15;
        for (int i = 0; i < 12; i++)
            if (a[i]) begin
                first_one = i[3:0];
                break;
            end
        evens = '0;
        for (int i = 0; i < 12; i++) begin
            if (i % 2) continue;
            evens += b[i];
        end
    end
    assign nib = sel < 3 ? a[sel*4 +: 4] : b[11 - sel -: 4];
endmodule
