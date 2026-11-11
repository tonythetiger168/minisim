// A set-type register that is not visible at any port.
module invint (input logic clk, rst_n, d, output logic q);
    logic s;
    always_ff @(posedge clk or negedge rst_n)
        if (!rst_n) s <= 1'b1;
        else s <= d;
    assign q = s ^ d;
endmodule
