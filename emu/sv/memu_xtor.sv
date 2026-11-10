// memu_xtor.sv - transactors for a design that memu put on FPGAs: the part
// that sits beside the design on the board, and the part that runs on the
// host. See docs/EMULATION.md.
//
// A transactor works at the design's clock edges, which the emulation wrapper
// announces: while emu_ready is high the design is at rest, emu_next_pos says
// whether the event it is about to run is a rising edge of the transactor's
// clock, and holding emu_run low keeps it from starting. That is how a
// transactor takes as long as its host needs without the design noticing:
// the design's time does not pass while emu_run is low.
//
//   memu_axil_slave_xtor   the design is the master; every access goes to the
//                          host (memu_mvp_host.sv hands it to the msim -mvp models)
//   memu_axil_master_xtor  the host is the master: a test, or a UVM driver
//                          through memu_host_if, reads and writes the design
//
// The two *_xtor modules are synthesizable and use nothing but emu_clk. Both
// also run in plain simulation, so one testbench serves the simulation and
// the emulation: tie emu_ready and emu_next_pos high and let memu_sim_clock
// make the design's clock from emu_edge.

// The design's AXI4-Lite master port, answered by the host. To the design it
// behaves cycle for cycle like mvp_axil_slave (mvp/sv/mvp.sv): one access at a
// time, the response the cycle after the address.
//
// The host side is a request held until it is answered:
//   req_valid, req_write, req_addr, req_wdata, req_strb   stable while req_valid
//   rsp_valid   high for one emu_clk cycle, with rsp_rdata and rsp_err
// On a board that is a message over whatever link the host has; nothing here
// depends on how long it takes.
module memu_axil_slave_xtor #(
    parameter ADDR_W = 32,
    parameter DATA_W = 32
) (
    input                   emu_clk,
    input                   emu_rst,
    input                   emu_ready,
    input                   emu_next_pos,  // the wrapper's emu_next_pos bit of this port's clock
    input                   emu_run_i,     // the other reasons to run: tie high, or chain transactors
    output                  emu_run,       // to the wrapper
    output                  emu_edge,      // this emu_clk edge is the design's clock edge
    input                   rst_n,         // the design's reset, as the design gets it

    input  [ADDR_W-1:0]     awaddr,
    input                   awvalid,
    output                  awready,
    input  [DATA_W-1:0]     wdata,
    input  [DATA_W/8-1:0]   wstrb,
    input                   wvalid,
    output                  wready,
    output [1:0]            bresp,
    output                  bvalid,
    input                   bready,
    input  [ADDR_W-1:0]     araddr,
    input                   arvalid,
    output                  arready,
    output [DATA_W-1:0]     rdata,
    output [1:0]            rresp,
    output                  rvalid,
    input                   rready,

    output                  req_valid,
    output                  req_write,
    output [ADDR_W-1:0]     req_addr,
    output [DATA_W-1:0]     req_wdata,
    output [DATA_W/8-1:0]   req_strb,
    input                   rsp_valid,
    input  [DATA_W-1:0]     rsp_rdata,
    input                   rsp_err
);
    reg                awready_r, wready_r, bvalid_r, arready_r, rvalid_r;
    reg                berr_r, rerr_r;
    reg [DATA_W-1:0]   rdata_r;
    reg [ADDR_W-1:0]   waddr;
    reg [DATA_W-1:0]   wbuf;
    reg [DATA_W/8-1:0] wstrb_buf;
    reg                waddr_seen, wdata_seen;

    // The reset is asynchronous in the design's time: it shows at once.
    assign awready = awready_r | ~rst_n;
    assign wready  = wready_r | ~rst_n;
    assign bvalid  = bvalid_r & rst_n;
    assign bresp   = {berr_r & rst_n, 1'b0};
    assign arready = arready_r | ~rst_n;
    assign rvalid  = rvalid_r & rst_n;
    assign rresp   = {rerr_r & rst_n, 1'b0};
    assign rdata   = rst_n ? rdata_r : {DATA_W{1'b0}};

    wire aw_fire = awvalid & awready_r;
    wire w_fire  = wvalid & wready_r;
    wire do_w    = (waddr_seen | aw_fire) & (wdata_seen | w_fire);
    wire do_r    = arvalid & arready_r;

    // What the host answered for the edge the design is waiting at.
    reg                w_done, w_err, r_done, r_err;
    reg [DATA_W-1:0]   r_data;

    wire at_edge = emu_ready & emu_next_pos & rst_n & ~emu_rst;
    wire want_w  = at_edge & do_w & ~w_done;
    wire want_r  = at_edge & do_r & ~r_done;
    assign emu_run   = emu_run_i & ~(want_w | want_r);
    assign emu_edge  = emu_ready & emu_next_pos & emu_run & ~emu_rst;
    assign req_valid = (want_w | want_r) & ~rsp_valid;
    assign req_write = want_w;  // the write first, as mvp_axil_slave does
    assign req_addr  = want_w ? (aw_fire ? awaddr : waddr) : araddr;
    assign req_wdata = w_fire ? wdata : wbuf;
    assign req_strb  = w_fire ? wstrb : wstrb_buf;

    always @(posedge emu_clk) begin
        if (emu_rst | ~rst_n) begin
            awready_r  <= 1'b1;
            wready_r   <= 1'b1;
            bvalid_r   <= 1'b0;
            berr_r     <= 1'b0;
            arready_r  <= 1'b1;
            rvalid_r   <= 1'b0;
            rerr_r     <= 1'b0;
            rdata_r    <= {DATA_W{1'b0}};
            waddr_seen <= 1'b0;
            wdata_seen <= 1'b0;
            w_done     <= 1'b0;
            r_done     <= 1'b0;
        end else if (emu_edge) begin
            // One rising edge of the design's clock.
            w_done <= 1'b0;
            r_done <= 1'b0;
            if (bvalid_r & bready) bvalid_r <= 1'b0;
            if (aw_fire) begin
                waddr      <= awaddr;
                waddr_seen <= 1'b1;
                awready_r  <= 1'b0;
            end
            if (w_fire) begin
                wbuf       <= wdata;
                wstrb_buf  <= wstrb;
                wdata_seen <= 1'b1;
                wready_r   <= 1'b0;
            end
            if (do_w) begin
                berr_r     <= w_err;
                bvalid_r   <= 1'b1;
                waddr_seen <= 1'b0;
                wdata_seen <= 1'b0;
                awready_r  <= 1'b1;
                wready_r   <= 1'b1;
            end
            if (rvalid_r & rready) begin
                rvalid_r  <= 1'b0;
                arready_r <= 1'b1;
            end
            if (do_r) begin
                rdata_r   <= r_data;
                rerr_r    <= r_err;
                rvalid_r  <= 1'b1;
                arready_r <= 1'b0;
            end
        end else if (rsp_valid) begin
            if (want_w) begin
                w_done <= 1'b1;
                w_err  <= rsp_err;
            end else if (want_r) begin
                r_done <= 1'b1;
                r_data <= rsp_rdata;
                r_err  <= rsp_err;
            end
        end
    end
endmodule

// The design's AXI4-Lite slave port, driven by the host. The host gives one
// command at a time and the design's clock runs only while there is one, so a
// test sees the same cycle counts on every run and on every host:
//   cmd_valid, cmd_op, cmd_addr, cmd_data, cmd_strb   held until rsp_valid
//     cmd_op 0  let cmd_data clock cycles pass
//            1  write cmd_data to cmd_addr
//            2  read cmd_addr
//   rsp_valid   high for one emu_clk cycle, with rsp_data and rsp_resp; the
//               host takes cmd_valid away (or gives the next command) then
// FREE_RUN = 1 lets the clock run between commands instead.
module memu_axil_master_xtor #(
    parameter ADDR_W = 32,
    parameter DATA_W = 32,
    parameter FREE_RUN = 0
) (
    input                       emu_clk,
    input                       emu_rst,
    input                       emu_ready,
    input                       emu_next_pos,
    input                       emu_run_i,
    output                      emu_run,
    output                      emu_edge,

    output reg [ADDR_W-1:0]     awaddr,
    output reg                  awvalid,
    input                       awready,
    output reg [DATA_W-1:0]     wdata,
    output reg [DATA_W/8-1:0]   wstrb,
    output reg                  wvalid,
    input                       wready,
    input  [1:0]                bresp,
    input                       bvalid,
    output reg                  bready,
    output reg [ADDR_W-1:0]     araddr,
    output reg                  arvalid,
    input                       arready,
    input  [DATA_W-1:0]         rdata,
    input  [1:0]                rresp,
    input                       rvalid,
    output reg                  rready,

    input                       cmd_valid,
    input  [1:0]                cmd_op,
    input  [ADDR_W-1:0]         cmd_addr,
    input  [DATA_W-1:0]         cmd_data,
    input  [DATA_W/8-1:0]       cmd_strb,
    output reg                  rsp_valid,
    output reg [DATA_W-1:0]     rsp_data,
    output reg [1:0]            rsp_resp
);
    localparam [2:0] IDLE = 3'd0, WAIT = 3'd1, WRITE = 3'd2, WRESP = 3'd3, READ = 3'd4, RRESP = 3'd5,
                     DONE = 3'd6, REST = 3'd7;
    reg [2:0]        state;
    reg [DATA_W-1:0] left;

    // The clock stops when there is nothing to do: before a command, and from
    // its last cycle until the host has the answer.
    wire stall = (state == IDLE && !cmd_valid && FREE_RUN == 0) || state == DONE || state == REST;
    assign emu_run  = emu_run_i & ~stall;
    assign emu_edge = emu_ready & emu_next_pos & emu_run & ~emu_rst;

    always @(posedge emu_clk) begin
        if (emu_rst) begin
            state     <= IDLE;
            awvalid   <= 1'b0;
            wvalid    <= 1'b0;
            bready    <= 1'b0;
            arvalid   <= 1'b0;
            rready    <= 1'b0;
            rsp_valid <= 1'b0;
            awaddr    <= {ADDR_W{1'b0}};
            araddr    <= {ADDR_W{1'b0}};
            wdata     <= {DATA_W{1'b0}};
            wstrb     <= {(DATA_W/8){1'b0}};
        end else if (state == DONE) begin
            rsp_valid <= 1'b1;
            state     <= REST;
        end else if (state == REST) begin
            // The host has seen rsp_valid and has changed cmd_valid by now.
            rsp_valid <= 1'b0;
            state     <= IDLE;
        end else if (emu_edge) begin
            case (state)
                IDLE: if (cmd_valid) begin
                    rsp_data <= {DATA_W{1'b0}};
                    rsp_resp <= 2'b00;
                    if (cmd_op == 2'd1) begin
                        awaddr  <= cmd_addr;
                        awvalid <= 1'b1;
                        wdata   <= cmd_data;
                        wstrb   <= cmd_strb;
                        wvalid  <= 1'b1;
                        bready  <= 1'b1;
                        state   <= WRITE;
                    end else if (cmd_op == 2'd2) begin
                        araddr  <= cmd_addr;
                        arvalid <= 1'b1;
                        rready  <= 1'b1;
                        state   <= READ;
                    end else if (cmd_data <= 1) begin
                        state <= DONE;  // this edge was the one cycle
                    end else begin
                        left  <= cmd_data - 2;
                        state <= WAIT;
                    end
                end
                WAIT: begin
                    left <= left - 1'b1;
                    if (left == 0) state <= DONE;
                end
                WRITE, WRESP: begin
                    if (awvalid & awready) awvalid <= 1'b0;
                    if (wvalid & wready) wvalid <= 1'b0;
                    if (bvalid & bready) begin
                        bready   <= 1'b0;
                        rsp_resp <= bresp;
                        state    <= DONE;
                    end
                end
                READ, RRESP: begin
                    if (arvalid & arready) arvalid <= 1'b0;
                    if (rvalid & rready) begin
                        rready   <= 1'b0;
                        rsp_data <= rdata;
                        rsp_resp <= rresp;
                        state    <= DONE;
                    end
                end
                default: ;
            endcase
        end
    end
endmodule

`ifndef SYNTHESIS
// ----------------------------------------------------------------- host side
// What follows runs on the host, in the simulator: it is not for the FPGA.

// The design's clock for a transactor in plain simulation: one rising edge
// for every emu_clk edge the transactor calls a clock edge.
module memu_sim_clock (
    input  logic emu_clk,
    input  logic emu_edge,
    output logic clk = 1'b0
);
    always @(posedge emu_clk) if (emu_edge) clk = 1'b1;
    always @(negedge emu_clk) clk = 1'b0;
endmodule

// The host end of memu_axil_master_xtor. A test calls the tasks; a UVM driver
// holds a `virtual memu_host_if` and calls them from its run_phase.
interface memu_host_if #(
    parameter int ADDR_W = 32,
    parameter int DATA_W = 32
) (
    input logic emu_clk
);
    logic                cmd_valid = 1'b0;
    logic [1:0]          cmd_op = '0;
    logic [ADDR_W-1:0]   cmd_addr = '0;
    logic [DATA_W-1:0]   cmd_data = '0;
    logic [DATA_W/8-1:0] cmd_strb = '0;
    logic                rsp_valid;
    logic [DATA_W-1:0]   rsp_data;
    logic [1:0]          rsp_resp;

    task automatic command(input logic [1:0] op, input logic [ADDR_W-1:0] addr,
                           input logic [DATA_W-1:0] data, input logic [DATA_W/8-1:0] strb);
        cmd_op    <= op;
        cmd_addr  <= addr;
        cmd_data  <= data;
        cmd_strb  <= strb;
        cmd_valid <= 1'b1;
        do @(posedge emu_clk); while (rsp_valid !== 1'b1);
        cmd_valid <= 1'b0;
    endtask

    // Let n cycles of the design's clock pass.
    task automatic idle(input int n);
        if (n > 0) command(2'd0, '0, DATA_W'(n), '0);
    endtask

    task automatic write(input logic [ADDR_W-1:0] addr, input logic [DATA_W-1:0] data,
                         output logic [1:0] resp, input logic [DATA_W/8-1:0] strb = '1);
        command(2'd1, addr, data, strb);
        resp = rsp_resp;
    endtask

    task automatic read(input logic [ADDR_W-1:0] addr, output logic [DATA_W-1:0] data,
                        output logic [1:0] resp);
        command(2'd2, addr, '0, '0);
        data = rsp_data;
        resp = rsp_resp;
    endtask
endinterface
`endif
