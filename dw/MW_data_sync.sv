// MW_data_sync - moves a WIDTH-bit word from clk_s's domain to clk_d's
//   with a four-phase request/acknowledge handshake. send (while busy is
//   low) captures data_s and raises the request; the destination takes the
//   word once the request has crossed STAGES flops and pulses data_avail
//   for one clk_d cycle with the word on data_d (held until the next one).
//   busy stays high until the acknowledge has come back down, so a send
//   while busy is ignored.
module MW_data_sync #(
    parameter int WIDTH  = 8,
    parameter int STAGES = 2
) (
    input  logic             clk_s,
    input  logic             rst_s_n,
    input  logic             send,
    input  logic [WIDTH-1:0] data_s,
    output logic             busy,
    input  logic             clk_d,
    input  logic             rst_d_n,
    output logic [WIDTH-1:0] data_d,
    output logic             data_avail
);
    logic             req, ack_s, req_d, ack;
    logic [WIDTH-1:0] hold;
    MW_sync #(.WIDTH(1), .STAGES(STAGES)) u_req (.clk(clk_d), .rst_n(rst_d_n), .d(req), .q(req_d));
    MW_sync #(.WIDTH(1), .STAGES(STAGES)) u_ack (.clk(clk_s), .rst_n(rst_s_n), .d(ack), .q(ack_s));
    always_ff @(posedge clk_s or negedge rst_s_n) begin
        if (!rst_s_n) begin
            req  <= 1'b0;
            hold <= '0;
        end else if (!req && !ack_s && send) begin
            req  <= 1'b1;
            hold <= data_s;
        end else if (req && ack_s) begin
            req <= 1'b0;
        end
    end
    assign busy = req | ack_s;
    always_ff @(posedge clk_d or negedge rst_d_n) begin
        if (!rst_d_n) begin
            ack        <= 1'b0;
            data_d     <= '0;
            data_avail <= 1'b0;
        end else begin
            data_avail <= 1'b0;
            if (req_d && !ack) begin
                ack        <= 1'b1;
                data_d     <= hold;     // stable: it changes only while req is low
                data_avail <= 1'b1;
            end else if (!req_d && ack) begin
                ack <= 1'b0;
            end
        end
    end
endmodule
