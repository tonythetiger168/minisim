// IEEE 1800-2023 9.4.2: an event control waits for a change of value.
// Writing an unknown value onto a variable that already holds that unknown
// value, by a nonblocking or a blocking assignment, is no change and does
// not wake a process waiting on any change of it.
module top;
    reg [3:0] c;
    reg [9:0] w;
    int nc = 0, nw = 0;
    always @(c) nc++;
    always @(w) nw++;
    initial begin
        #1 c <= c + 1;        // x + 1 is x: no change
        #1 c = c ^ 4'b0001;   // still x
        #1 c <= 4'bx;
        #1 w <= {10{1'bx}};
        #1 $display("c wakes %0d", nc);
        $display("w wakes %0d", nw);
        c = 4'd3;
        #1 $display("c wakes %0d", nc);
        c <= 4'd3;
        #1 $display("c wakes %0d", nc);
    end
endmodule
//> c wakes 0
//> w wakes 0
//> c wakes 1
//> c wakes 1
