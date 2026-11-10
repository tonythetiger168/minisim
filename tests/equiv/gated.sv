// A register bank with a load enable, as RTL. gated_gates.v gates the clock
// instead of recirculating the data.
module gated (input logic clk, rst_n, en, scan_en, input logic [3:0] d, output logic [3:0] q);
    always_ff @(posedge clk or negedge rst_n)
        if (!rst_n) q <= '0;
        else if (en | scan_en) q <= d;
endmodule
