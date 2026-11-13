// The IP body a third-party eTool would encrypt: tests/protect_interop.sh
// builds the envelopes around it with the openssl command line tool alone.
module interop_ip (
    input  wire       clk,
    input  wire [7:0] a,
    output wire [7:0] y
);
    reg [7:0] inner_state;
    always @(posedge clk) inner_state <= a;
    assign y = inner_state ^ 8'h5a;
endmodule
