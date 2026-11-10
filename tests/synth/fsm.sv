// An enum state machine, a packed struct, casez, a memory, negedge and async-set flops.
package fsm_pkg;
    typedef enum logic [1:0] {IDLE, LOAD, RUN, DONE} state_t;
    typedef struct packed {
        logic       valid;
        logic [2:0] tag;
        logic [7:0] data;
    } item_t;
endpackage

module fsm import fsm_pkg::*; (
    input  logic       clk, rst, start,
    input  logic [7:0] din,
    input  logic [1:0] addr,
    output logic [7:0] dout,
    output logic       busy, done,
    output logic [2:0] prio,
    output logic       nflag, sflag
);
    state_t state, next;
    item_t  item;
    logic [7:0] mem [0:3];
    logic [3:0] count;

    always_comb begin
        next = state;
        case (state)
            IDLE: if (start) next = LOAD;
            LOAD: next = RUN;
            RUN:  if (count == 4'd9) next = DONE;
            DONE: next = IDLE;
        endcase
    end
    always_ff @(posedge clk or posedge rst) begin
        if (rst) begin
            state <= IDLE;
            count <= '0;
            item  <= '0;
        end else begin
            state <= next;
            if (state == LOAD) begin
                count <= '0;
                item  <= '{valid: 1'b1, tag: din[2:0], data: din};
            end else if (state == RUN) begin
                count <= count + 4'd1;
                item.data <= item.data + {5'b0, item.tag};
                mem[count[1:0]] <= item.data ^ din;
            end else if (state == DONE)
                item.valid <= 1'b0;
        end
    end
    always_comb begin
        casez (din)
            8'b1???????: prio = 3'd7;
            8'b01??????: prio = 3'd6;
            8'b001?????: prio = 3'd5;
            8'b0001????: prio = 3'd4;
            8'b00001???: prio = 3'd3;
            8'b000001??: prio = 3'd2;
            8'b0000001?: prio = 3'd1;
            default:     prio = 3'd0;
        endcase
    end
    // a falling-edge flop and one with an asynchronous set
    always_ff @(negedge clk) nflag <= start ^ din[0];
    always_ff @(posedge clk or posedge rst)
        if (rst) sflag <= 1'b1;
        else sflag <= din[7];
    assign dout = item.valid ? mem[addr] : item.data;
    assign busy = state != IDLE;
    assign done = state == DONE;
endmodule
