// MW_fifo_sync - single-clock FIFO.
//   push writes data_in when not full; pop removes the oldest word when not
//   empty. data_out shows the oldest word whenever empty = 0 (first word
//   falls through). A push when full or a pop when empty is ignored and
//   raises error for that clock. count is the number of words held.
//   almost_full / almost_empty compare count with AF_LEVEL / AE_LEVEL:
//   almost_full = (count >= DEPTH - AF_LEVEL), almost_empty = (count <= AE_LEVEL).
module MW_fifo_sync #(
    parameter int WIDTH    = 8,
    parameter int DEPTH    = 16,
    parameter int AF_LEVEL = 1,
    parameter int AE_LEVEL = 1,
    localparam int AW      = (DEPTH > 1) ? $clog2(DEPTH) : 1,
    localparam int CW      = $clog2(DEPTH + 1)
) (
    input  logic             clk,
    input  logic             rst_n,
    input  logic             push,
    input  logic [WIDTH-1:0] data_in,
    input  logic             pop,
    output logic [WIDTH-1:0] data_out,
    output logic             empty,
    output logic             full,
    output logic             almost_empty,
    output logic             almost_full,
    output logic [CW-1:0]    count,
    output logic             error
);
    logic [WIDTH-1:0] mem [DEPTH];
    logic [AW-1:0]    wp, rp;
    logic             do_push, do_pop;

    assign do_push = push & ~full;
    assign do_pop  = pop & ~empty;
    assign error   = (push & full) | (pop & empty);

    always_ff @(posedge clk) begin
        if (do_push) mem[wp] <= data_in;
    end
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            wp <= '0;
            rp <= '0;
            count <= '0;
        end else begin
            if (do_push) wp <= (wp == AW'(DEPTH - 1)) ? '0 : wp + 1'b1;
            if (do_pop)  rp <= (rp == AW'(DEPTH - 1)) ? '0 : rp + 1'b1;
            if (do_push & ~do_pop)      count <= count + 1'b1;
            else if (do_pop & ~do_push) count <= count - 1'b1;
        end
    end
    assign data_out     = mem[rp];
    assign empty        = (count == '0);
    assign full         = (count == CW'(DEPTH));
    assign almost_empty = (count <= CW'(AE_LEVEL));
    assign almost_full  = (count >= CW'(DEPTH - AF_LEVEL));
endmodule
