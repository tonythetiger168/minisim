// A packed struct and a memory in a block with an asynchronous reset that
// resets only the struct: the memory holds while rst is high.
module flat (input logic clk, rst, input logic [1:0] a, input logic [3:0] d, output logic [3:0] q, output logic v);
    typedef struct packed { logic v; logic [2:0] t; } item_t;
    item_t item;
    logic [3:0] mem [0:1];
    always_ff @(posedge clk or posedge rst)
        if (rst) item <= '0;
        else begin
            item <= '{v: d[3], t: d[2:0]};
            mem[a[0]] <= d;
        end
    assign q = mem[a[1]] ^ {1'b0, item.t};
    assign v = item.v;
endmodule
