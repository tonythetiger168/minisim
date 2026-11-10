// Latches, don't-cares, a black box and a renamed register, against
// misc_gates.v.
module ram (input logic clk, we, input logic [1:0] a, input logic [3:0] d, output logic [3:0] q);
    logic [3:0] m [4];
    always_ff @(posedge clk) begin
        if (we) m[a] <= d;
        q <= m[a];
    end
endmodule

module misc (
    input  logic       clk, g, we,
    input  logic [1:0] sel, a,
    input  logic [3:0] d,
    output logic [3:0] lat, y, rq,
    output logic       state_o
);
    // A latch: lat is not assigned when g is low.
    always_latch
        if (g) lat = d;

    // sel == 3 never happens; the netlist is free there.
    always_comb
        case (sel)
            2'd0: y = d;
            2'd1: y = ~d;
            2'd2: y = {d[0], d[3:1]};
            default: y = 'x;
        endcase

    ram u_ram (.clk, .we, .a, .d, .q(rq));

    logic toggle;
    always_ff @(posedge clk) toggle <= toggle ^ we;
    assign state_o = toggle;
endmodule
