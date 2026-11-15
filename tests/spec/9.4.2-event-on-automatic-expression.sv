// IEEE 1800-2023 9.4.2, 9.4.2.1, 13.3.2: an event control in an automatic
// task may name an expression of the call's own automatic variables (a
// bit of a local vector, an element of a local array), a list of such
// items with edges, or a part of a static variable. Two calls that
// overlap each wait on their own variables. A local event triggered in
// a fork branch belongs to its own call.
module top;
    reg [3:0] g;
    task automatic watch(input integer id);
        reg [7:0] mem [0:1];
        reg [2:0] p;
        event go;
        fork
            begin
                #2 mem[0] = id;
                #2 p[1] = 1'b1;
                #2 p[2] = 1'b0;
                #2 ->go;
            end
            begin
                @(mem[0]) $display("%0t call %0d mem[0]=%0d", $time, id, mem[0]);
                @(posedge p[1] or negedge p[2]) $display("%0t call %0d edge p=%b", $time, id, p);
                @(negedge p[2] or posedge p[0]) $display("%0t call %0d edge p=%b", $time, id, p);
                @go $display("%0t call %0d go, id still %0d", $time, id, id);
            end
        join
    endtask
    task automatic static_bit;
        @(posedge g[2]) $display("%0t g[2] rose", $time);
    endtask
    initial begin g = 0; #3 g[1] = 1; #2 g[2] = 1; end
    initial static_bit;
    initial #10 watch(1);
    initial #11 watch(2);
endmodule
//> 5 g[2] rose
//> 12 call 1 mem[0]=1
//> 13 call 2 mem[0]=2
//> 14 call 1 edge p=x1x
//> 15 call 2 edge p=x1x
//> 16 call 1 edge p=01x
//> 17 call 2 edge p=01x
//> 18 call 1 go, id still 1
//> 19 call 2 go, id still 2
