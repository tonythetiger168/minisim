// MW_pulse_sync - carries single-cycle pulses from clk_s's domain to
//   clk_d's. Each pulse_s toggles a level in the source domain; the level
//   crosses through STAGES flops and each change of it gives one pulse_d,
//   one clk_d cycle wide. Pulses must be at least STAGES + 1 clk_d cycles
//   apart (busy is high while one is still on its way back).
module MW_pulse_sync #(
    parameter int STAGES = 2
) (
    input  logic clk_s,
    input  logic rst_s_n,
    input  logic pulse_s,
    output logic busy,
    input  logic clk_d,
    input  logic rst_d_n,
    output logic pulse_d
);
    logic tog_s, tog_back;
    logic [STAGES:0] d;
    always_ff @(posedge clk_s or negedge rst_s_n)
        if (!rst_s_n)     tog_s <= 1'b0;
        else if (pulse_s) tog_s <= ~tog_s;
    always_ff @(posedge clk_d or negedge rst_d_n)
        if (!rst_d_n) d <= '0;
        else          d <= {d[STAGES-1:0], tog_s};
    assign pulse_d = d[STAGES] ^ d[STAGES-1];
    // the level as the destination has taken it, back in the source domain
    MW_sync #(.WIDTH(1), .STAGES(STAGES)) u_back (.clk(clk_s), .rst_n(rst_s_n), .d(d[STAGES-1]), .q(tog_back));
    assign busy = tog_s ^ tog_back;
endmodule
