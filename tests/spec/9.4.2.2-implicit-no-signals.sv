// IEEE 1800-2023 9.4.2.2: @* waits on the nets and variables its statement
// reads. A statement that reads none gives an event control that never
// triggers, so `always @* foo <= 0;` never runs.
module top;
    reg foo;
    always @* foo <= 0;
    initial #1 $display("foo is %b", foo);
endmodule
//> foo is x
