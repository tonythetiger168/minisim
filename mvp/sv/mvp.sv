// mvp.sv - the design side of a minisim virtual prototype: an AXI4-Lite slave
// that forwards every access to a C or C++ model, and an interrupt port the
// models drive. See include/mvp.h for the model side and docs/VPROTO.md.
//
//   msim soc.sv soc_tb.sv $(msim -mvp-sv) ram.c uart.c -top soc_tb
//
// Every module here declares `timeunit 1ps`, so the time handed to the models
// is in picoseconds whatever timescale the rest of the design uses.

package mvp_pkg;
    timeunit 1ps / 1ps;

    // The runtime in the model library (mvp/mvp.c). `context` because these
    // call back into the design through mvp_bus_* and may call exported
    // functions of their own.
    import "DPI-C" context function int mvp_dpi_init(input int verbose);
    import "DPI-C" context function int mvp_dpi_read(input longint addr, input int bytes,
                                                     output longint data);
    import "DPI-C" context function int mvp_dpi_write(input longint addr, input int bytes,
                                                      input longint data, input int strb);
    import "DPI-C" context function void mvp_dpi_tick(input longint now_ps);
    import "DPI-C" context function int mvp_dpi_irq();

    // mvp.h's status codes, as AXI responses (A3.4.4): OKAY or SLVERR.
    localparam logic [1:0] RESP_OKAY = 2'b00, RESP_SLVERR = 2'b10;

    function automatic logic [1:0] resp(int rc);
        return rc == 0 ? RESP_OKAY : RESP_SLVERR;
    endfunction
endpackage

// One clock edge of the virtual prototype: the models' notion of time, and the
// callbacks they armed with mvp_after_ps. Instance it once per design, on the
// clock the models should see. mvp_axil_slave ticks as well, so a design that
// has a slave on every clock needs no mvp_clock; it is for the models' timers
// in a design whose slaves are idle.
module mvp_clock #(
    parameter bit VERBOSE = 0  // print the memory map at time 0
) (
    input logic clk
);
    timeunit 1ps / 1ps;
    import mvp_pkg::*;

    initial void'(mvp_dpi_init(int'(VERBOSE)));
    always @(posedge clk) mvp_dpi_tick(longint'($time));
endmodule

// An AXI4-Lite slave (IHI 0022, B1) in front of the memory map in C. One
// access at a time, which is what a model that answers in zero time needs:
// the C call happens in the cycle the address is accepted, and the response
// goes out the cycle after.
//
// ADDR_OFFSET is added to every address before it reaches C, so a slave behind
// an interconnect that strips the high bits still lands in the right device.
module mvp_axil_slave #(
    parameter int ADDR_W = 32,
    parameter int DATA_W = 32,      // 32 or 64
    parameter longint ADDR_OFFSET = 0,
    parameter bit VERBOSE = 0
) (
    input  logic                  clk,
    input  logic                  rst_n,
    // Write address
    input  logic [ADDR_W-1:0]     awaddr,
    input  logic                  awvalid,
    output logic                  awready,
    // Write data
    input  logic [DATA_W-1:0]     wdata,
    input  logic [DATA_W/8-1:0]   wstrb,
    input  logic                  wvalid,
    output logic                  wready,
    // Write response
    output logic [1:0]            bresp,
    output logic                  bvalid,
    input  logic                  bready,
    // Read address
    input  logic [ADDR_W-1:0]     araddr,
    input  logic                  arvalid,
    output logic                  arready,
    // Read data
    output logic [DATA_W-1:0]     rdata,
    output logic [1:0]            rresp,
    output logic                  rvalid,
    input  logic                  rready
);
    timeunit 1ps / 1ps;
    import mvp_pkg::*;

    localparam int BYTES = DATA_W / 8;

    initial begin
        if (DATA_W != 32 && DATA_W != 64)
            $fatal(1, "mvp_axil_slave: DATA_W must be 32 or 64, not %0d", DATA_W);
        void'(mvp_dpi_init(int'(VERBOSE)));
    end

    logic [ADDR_W-1:0] waddr;
    logic              waddr_seen, wdata_seen;
    logic [DATA_W-1:0] wbuf;
    logic [BYTES-1:0]  wstrb_buf;

    // The write happens once both channels have handed over their half, in
    // either order (B1.2: there is no ordering between AW and W).
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            awready    <= 1'b1;
            wready     <= 1'b1;
            bvalid     <= 1'b0;
            bresp      <= RESP_OKAY;
            waddr_seen <= 1'b0;
            wdata_seen <= 1'b0;
        end else begin
            automatic logic have_addr = waddr_seen;
            automatic logic have_data = wdata_seen;
            mvp_dpi_tick(longint'($time));

            if (bvalid && bready) bvalid <= 1'b0;

            if (awvalid && awready) begin
                waddr      <= awaddr;
                waddr_seen <= 1'b1;
                awready    <= 1'b0;
                have_addr   = 1'b1;
            end
            if (wvalid && wready) begin
                wbuf       <= wdata;
                wstrb_buf  <= wstrb;
                wdata_seen <= 1'b1;
                wready     <= 1'b0;
                have_data   = 1'b1;
            end

            if (have_addr && have_data) begin
                automatic logic [ADDR_W-1:0] a = (awvalid && awready) ? awaddr : waddr;
                automatic logic [DATA_W-1:0] d = (wvalid && wready) ? wdata : wbuf;
                automatic logic [BYTES-1:0]  s = (wvalid && wready) ? wstrb : wstrb_buf;
                automatic int rc = mvp_dpi_write(longint'(a) + ADDR_OFFSET, BYTES,
                                                 longint'(d), int'(s));
                bresp      <= resp(rc);
                bvalid     <= 1'b1;
                waddr_seen <= 1'b0;
                wdata_seen <= 1'b0;
                awready    <= 1'b1;
                wready     <= 1'b1;
            end
        end
    end

    // Reads: the model answers in the cycle the address is accepted.
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            arready <= 1'b1;
            rvalid  <= 1'b0;
            rresp   <= RESP_OKAY;
            rdata   <= '0;
        end else begin
            if (rvalid && rready) begin
                rvalid  <= 1'b0;
                arready <= 1'b1;
            end
            if (arvalid && arready) begin
                automatic longint data;
                automatic int rc = mvp_dpi_read(longint'(araddr) + ADDR_OFFSET, BYTES, data);
                rdata   <= DATA_W'(data);
                rresp   <= resp(rc);
                rvalid  <= 1'b1;
                arready <= 1'b0;
            end
        end
    end
endmodule

// The interrupt lines the models drive with mvp_set_irq, sampled every clock.
// Level sensitive: a line stays until its model clears it.
module mvp_irq_slave #(
    parameter int N = 1
) (
    input  logic         clk,
    input  logic         rst_n,
    output logic [N-1:0] irq
);
    timeunit 1ps / 1ps;
    import mvp_pkg::*;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) irq <= '0;
        else        irq <= N'(mvp_dpi_irq());
    end
endmodule
