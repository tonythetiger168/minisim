// MW_cmp - magnitude comparator with all six results.
//   tc = 0 compares unsigned, tc = 1 compares two's complement.
module MW_cmp #(
    parameter int WIDTH = 8
) (
    input  logic [WIDTH-1:0] a,
    input  logic [WIDTH-1:0] b,
    input  logic             tc,
    output logic             lt,
    output logic             gt,
    output logic             eq,
    output logic             le,
    output logic             ge,
    output logic             ne
);
    // Flipping both sign bits turns a signed compare into an unsigned one.
    logic [WIDTH-1:0] x, y;
    always_comb begin
        x = a;
        y = b;
        x[WIDTH-1] = a[WIDTH-1] ^ tc;
        y[WIDTH-1] = b[WIDTH-1] ^ tc;
    end
    // Tree compare: each node carries (lt, eq) of its span.
    logic [WIDTH-1:0] l, e;
    always_comb begin
        logic lt_acc, eq_acc;
        lt_acc = 1'b0;
        eq_acc = 1'b1;
        for (int i = WIDTH - 1; i >= 0; i--) begin
            l[i] = ~x[i] & y[i];
            e[i] = ~(x[i] ^ y[i]);
            lt_acc = lt_acc | (eq_acc & l[i]);
            eq_acc = eq_acc & e[i];
        end
        lt = lt_acc;
        eq = eq_acc;
    end
    assign gt = ~lt & ~eq;
    assign le = lt | eq;
    assign ge = ~lt;
    assign ne = ~eq;
endmodule
