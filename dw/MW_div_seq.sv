// MW_div_seq - sequential divider, one quotient bit per clock.
//   Pulse start with a, b and tc valid. done rises A_WIDTH clocks later
//   and stays up, with quotient, remainder and divide_by_0 held, until the
//   next start. Results are as MW_div.
module MW_div_seq #(
    parameter int A_WIDTH = 8,
    parameter int B_WIDTH = 8
) (
    input  logic               clk,
    input  logic               rst_n,
    input  logic               start,
    input  logic [A_WIDTH-1:0] a,
    input  logic [B_WIDTH-1:0] b,
    input  logic               tc,
    output logic               done,
    output logic [A_WIDTH-1:0] quotient,
    output logic [B_WIDTH-1:0] remainder,
    output logic               divide_by_0
);
    localparam int CW = $clog2(A_WIDTH + 1);
    logic [A_WIDTH-1:0] q, a_r;
    logic [B_WIDTH-1:0] mb;
    logic [B_WIDTH:0]   r, rs, d;
    logic [CW-1:0]      cnt;
    logic               busy, sa, sb, tc_r, dz;

    assign rs = {r[B_WIDTH-1:0], q[A_WIDTH-1]};
    assign d  = rs - {1'b0, mb};

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            q <= '0; r <= '0; mb <= '0; a_r <= '0; cnt <= '0;
            busy <= 1'b0; done <= 1'b0; sa <= 1'b0; sb <= 1'b0; tc_r <= 1'b0; dz <= 1'b0;
        end else if (start) begin
            // q holds the dividend's magnitude and fills with quotient
            // bits from the bottom as it shifts out of the top.
            q    <= (tc & a[A_WIDTH-1]) ? (~a + 1'b1) : a;
            mb   <= (tc & b[B_WIDTH-1]) ? (~b + 1'b1) : b;
            r    <= '0;
            a_r  <= a;
            sa   <= tc & a[A_WIDTH-1];
            sb   <= tc & b[B_WIDTH-1];
            tc_r <= tc;
            dz   <= (b == '0);
            cnt  <= CW'(A_WIDTH);
            busy <= 1'b1;
            done <= 1'b0;
        end else if (busy) begin
            if (!d[B_WIDTH]) begin
                r <= d;
                q <= {q[A_WIDTH-2:0], 1'b1};
            end else begin
                r <= rs;
                q <= {q[A_WIDTH-2:0], 1'b0};
            end
            cnt <= cnt - 1'b1;
            if (cnt == CW'(1)) begin
                busy <= 1'b0;
                done <= 1'b1;
            end
        end
    end

    assign divide_by_0 = dz;
    always_comb begin
        if (dz) begin
            if (!tc_r)   quotient = '1;
            else if (sa) quotient = {1'b1, {(A_WIDTH-1){1'b0}}};
            else         quotient = {1'b0, {(A_WIDTH-1){1'b1}}};
            remainder = '0;
            for (int i = 0; i < B_WIDTH; i++)
                remainder[i] = (i < A_WIDTH) ? a_r[i] : sa;
        end else begin
            quotient  = (sa ^ sb) ? (~q + 1'b1) : q;
            remainder = sa ? (~r[B_WIDTH-1:0] + 1'b1) : r[B_WIDTH-1:0];
        end
    end
    generate
        if (A_WIDTH < 2) begin : g_bad
            $error("MW_div_seq: A_WIDTH must be at least 2");
        end
    endgenerate
endmodule
