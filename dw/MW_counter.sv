// MW_counter - up/down counter with synchronous load.
//   load takes data; otherwise en counts up (up_dn = 1) or down (up_dn = 0).
//   tercnt = 1 while the count is at its last value in the current
//   direction (all ones going up, 0 going down).
module MW_counter #(
    parameter int WIDTH = 8
) (
    input  logic             clk,
    input  logic             rst_n,
    input  logic             load,
    input  logic [WIDTH-1:0] data,
    input  logic             en,
    input  logic             up_dn,
    output logic [WIDTH-1:0] count,
    output logic             tercnt
);
    logic [WIDTH-1:0] nxt;
    MW_incdec #(.WIDTH(WIDTH)) u_id (.a(count), .inc_dec(~up_dn), .sum(nxt));
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)    count <= '0;
        else if (load) count <= data;
        else if (en)   count <= nxt;
    end
    assign tercnt = up_dn ? (&count) : ~(|count);
endmodule
