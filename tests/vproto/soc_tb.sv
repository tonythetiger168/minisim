// soc_tb.sv - the virtual prototype test: an AXI4-Lite master drives the C
// models (mvp/models/ram.c and uart.c) through mvp_axil_slave, and checks the
// memory, the byte strobes, the UART's character time and interrupt, and the
// response to an address no model holds.
`timescale 1ns / 1ps

module soc_tb;
    localparam int AW = 32, DW = 32;
    localparam longint UART = 'h1000;

    logic clk = 0, rst_n = 0;
    always #5 clk = ~clk;  // 100 MHz

    logic [AW-1:0]   awaddr, araddr;
    logic            awvalid, awready, wvalid, wready, bvalid, bready;
    logic            arvalid, arready, rvalid, rready;
    logic [DW-1:0]   wdata, rdata;
    logic [DW/8-1:0] wstrb;
    logic [1:0]      bresp, rresp;
    logic            irq;

    mvp_clock #(.VERBOSE(1)) tick (.clk(clk));

    mvp_axil_slave #(.ADDR_W(AW), .DATA_W(DW)) dut (
        .clk, .rst_n,
        .awaddr, .awvalid, .awready,
        .wdata, .wstrb, .wvalid, .wready,
        .bresp, .bvalid, .bready,
        .araddr, .arvalid, .arready,
        .rdata, .rresp, .rvalid, .rready
    );

    mvp_irq_slave #(.N(1)) irqs (.clk, .rst_n, .irq(irq));

    int errors = 0;

    task automatic check(string what, logic [31:0] got, logic [31:0] want);
        if (got !== want) begin
            $display("FAIL %s: got %h, want %h", what, got, want);
            errors++;
        end
    endtask

    // One AXI4-Lite write: both halves of the request in the same cycle.
    task automatic wr(input logic [AW-1:0] a, input logic [DW-1:0] d,
                      input logic [DW/8-1:0] strb, output logic [1:0] r);
        @(posedge clk);
        awaddr <= a; awvalid <= 1'b1;
        wdata  <= d; wstrb  <= strb; wvalid <= 1'b1;
        bready <= 1'b1;
        @(posedge clk);
        while (!(awready && awvalid)) @(posedge clk);
        awvalid <= 1'b0; wvalid <= 1'b0;
        while (!bvalid) @(posedge clk);
        r = bresp;
        @(posedge clk);
        bready <= 1'b0;
    endtask

    task automatic rd(input logic [AW-1:0] a, output logic [DW-1:0] d, output logic [1:0] r);
        @(posedge clk);
        araddr <= a; arvalid <= 1'b1; rready <= 1'b1;
        @(posedge clk);
        while (!(arready && arvalid)) @(posedge clk);
        arvalid <= 1'b0;
        while (!rvalid) @(posedge clk);
        d = rdata;
        r = rresp;
        @(posedge clk);
        rready <= 1'b0;
    endtask

    logic [DW-1:0] d;
    logic [1:0]    r;

    // Wait for THR to be empty, then send one byte.
    task automatic putc(input logic [7:0] c);
        logic [DW-1:0] lsr;
        logic [1:0]    rsp;
        for (int i = 0; i < 20000; i++) begin
            rd(UART + 'h4, lsr, rsp);
            if (lsr[0]) break;
        end
        if (!lsr[0]) begin
            $display("FAIL putc: THR never emptied");
            errors++;
            return;
        end
        wr(UART + 'h0, DW'(c), '1, rsp);
        check("putc resp", rsp, 2'b00);
    endtask

    initial begin
        awvalid = 0; wvalid = 0; bready = 0; arvalid = 0; rready = 0;
        wstrb = '1;
        repeat (3) @(posedge clk);
        rst_n = 1;

        // The memory answers in the cycle the address is accepted.
        wr('h0000, 32'hdead_beef, '1, r);
        check("write ram resp", r, 2'b00);
        rd('h0000, d, r);
        check("read ram", d, 32'hdead_beef);
        check("read ram resp", r, 2'b00);

        // Byte strobes: only the enabled bytes change.
        wr('h0004, 32'h1122_3344, '1, r);
        wr('h0004, 32'haabb_ccdd, 4'b0101, r);
        rd('h0004, d, r);
        // wstrb 0101 enables bytes 0 and 2, which are 0xdd and 0xbb.
        check("byte strobes", d, 32'h11bb_33dd);

        // An address no model holds is SLVERR, and the design keeps running.
        rd('h8000_0000, d, r);
        check("decode resp", r, 2'b10);

        // The UART: empty at the start, and the write takes a character time.
        rd(UART + 'h4, d, r);
        check("uart LSR idle", d, 32'h1);
        wr(UART + 'h8, 32'h1, '1, r);        // IER: interrupt when THR empties
        wr(UART + 'h0, 32'h48, '1, r);       // 'H'
        rd(UART + 'h4, d, r);
        check("uart LSR busy", d, 32'h2);
        check("irq before the character went out", irq, 1'b0);

        // 10 bits at 115200 baud is about 86.8 us; wait for it.
        for (int i = 0; i < 20000 && !irq; i++) @(posedge clk);
        check("irq after the character", irq, 1'b1);
        rd(UART + 'h4, d, r);
        check("uart LSR empty again", d, 32'h1);
        rd(UART + 'hc, d, r);
        check("uart IIR pending", d, 32'h1);
        $display("tx took %0t", $time);

        wr(UART + 'hc, 32'h0, '1, r);        // clear it
        @(posedge clk);
        @(posedge clk);
        check("irq cleared", irq, 1'b0);

        // The rest of the line, polling THRE between characters the way a
        // driver without interrupts does.
        wr(UART + 'h8, 32'h0, '1, r);
        putc(8'h69);                         // 'i'
        putc(8'h0a);                         // '\n'

        if (errors == 0) $display("VPROTO PASSED");
        else             $display("VPROTO FAILED: %0d errors", errors);
        $finish;
    end

    initial begin
        #2ms;
        $display("VPROTO FAILED: timeout");
        $finish;
    end
endmodule
