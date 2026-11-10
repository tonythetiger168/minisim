// memu_mvp_host.sv - the host end of memu_axil_slave_xtor (memu_xtor.sv) for a
// virtual prototype: needs mvp.sv, so compile it with msim -mvp.
//
//   msim -mvp $(memu -xtor-sv) soc_emu.f soc_emu_tb.sv ram.c uart.c
//
// The host end of memu_axil_slave_xtor: the design's accesses reach the C and
// C++ models of a virtual prototype (msim -mvp, docs/VPROTO.md), which see the
// design's time, not the emulator's. Also the interrupt lines the models
// drive, as mvp_irq_slave gives them to a simulated design.
//
// UNIT_PS is the unit of the wrapper's emu_time, which memu prints in its
// report and at the top of the wrapper.
module memu_mvp_host #(
    parameter int ADDR_W = 32,
    parameter int DATA_W = 32,     // 32 or 64
    parameter longint ADDR_OFFSET = 0,
    parameter longint UNIT_PS = 1,
    parameter int IRQS = 1,
    parameter bit VERBOSE = 0
) (
    input  logic                  emu_clk,
    input  logic [63:0]           emu_next_time,
    input  logic                  emu_edge,   // from the transactor
    input  logic                  rst_n,
    input  logic                  req_valid,
    input  logic                  req_write,
    input  logic [ADDR_W-1:0]     req_addr,
    input  logic [DATA_W-1:0]     req_wdata,
    input  logic [DATA_W/8-1:0]   req_strb,
    output logic                  rsp_valid = 1'b0,
    output logic [DATA_W-1:0]     rsp_rdata = '0,
    output logic                  rsp_err = 1'b0,
    output logic [IRQS-1:0]       irq = '0
);
    import mvp_pkg::*;

    initial void'(mvp_dpi_init(int'(VERBOSE)));

    // The models' clock moves once per edge of the design's clock, before the
    // first access of that edge.
    bit ticked = 1'b0;
    function automatic void tick();
        if (!ticked) mvp_dpi_tick(longint'(emu_next_time) * UNIT_PS);
        ticked = 1'b1;
    endfunction

    always @(posedge emu_clk) begin
        rsp_valid <= 1'b0;
        if (!rst_n) begin
            irq <= '0;
        end else if (emu_edge) begin
            tick();
            ticked = 1'b0;
            irq <= IRQS'(mvp_dpi_irq());
        end else if (req_valid && !rsp_valid) begin
            automatic longint data = 0;
            automatic int rc;
            tick();
            if (req_write)
                rc = mvp_dpi_write(longint'(req_addr) + ADDR_OFFSET, DATA_W / 8, longint'(req_wdata),
                                   int'(req_strb));
            else
                rc = mvp_dpi_read(longint'(req_addr) + ADDR_OFFSET, DATA_W / 8, data);
            rsp_rdata <= DATA_W'(data);
            rsp_err   <= rc != 0;
            rsp_valid <= 1'b1;
        end
    end
endmodule
