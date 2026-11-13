// IEEE 1800-2023 12.5, 12.5.3: case statements with many constant labels
// (enum names, items with several labels, unique, nested, a default and
// none), a selector with x bits, a case inside a clocked loop, and an
// always_comb that drives several outputs from one case. msim finds the
// item through a table; every result must equal the label-by-label compare.
module top;
  typedef enum logic [11:0] {
    MSTATUS = 12'h300, MISA = 12'h301, MIE = 12'h304, MTVEC = 12'h305,
    MSCRATCH = 12'h340, MEPC = 12'h341, MCAUSE = 12'h342, MTVAL = 12'h343,
    MCYCLE = 12'hb00, MINSTRET = 12'hb02, MHARTID = 12'hf14
  } csr_e;
  csr_e addr;
  logic [31:0] rdata, aux;
  logic ill, cnt;
  always_comb begin
    rdata = '0; ill = 1'b0; cnt = 1'b0; aux = 32'h0;
    unique case (addr)
      MSTATUS: rdata = 32'h1800;
      MISA: rdata = 32'h40101104;
      MIE, MTVEC: begin rdata = {20'h0, addr}; aux = 32'h1; end
      MSCRATCH: rdata = 32'hcafe;
      MEPC, MCAUSE, MTVAL: begin
        rdata = 32'h100 | {28'h0, addr[3:0]};
        case (addr[1:0])
          2'b00: aux = 32'ha0;
          2'b01: aux = 32'ha1;
          2'b10: aux = 32'ha2;
          2'b11: aux = 32'ha3;
        endcase
      end
      MCYCLE, MINSTRET: begin rdata = 32'h77; cnt = 1'b1; end
      MHARTID: rdata = 32'h0;
      default: ill = 1'b1;
    endcase
  end
  // No default, and a label that repeats (the first item wins).
  logic [3:0] op;
  logic [7:0] res;
  always_comb begin
    res = 8'hff;
    case (op)
      4'd0: res = 8'd10;
      4'd1: res = 8'd11;
      4'd2: res = 8'd12;
      4'd3, 4'd1: res = 8'd13;
      4'd9: res = 8'd19;
    endcase
  end
  // A case inside a loop of a clocked process.
  logic clk = 0;
  logic [1:0] sel [0:3];
  logic [1:0] acc [0:3];
  always @(posedge clk) begin
    for (int i = 0; i < 4; i++)
      case (sel[i])
        2'd0: acc[i] <= 2'd3;
        2'd1: acc[i] <= 2'd2;
        2'd2: acc[i] <= 2'd1;
        2'd3: acc[i] <= 2'd0;
      endcase
  end
  initial begin
    foreach (sel[i]) sel[i] = 2'(i);
    foreach (acc[i]) acc[i] = 0;
    #1 clk = 1; #1 clk = 0;
    $display("acc %h %h %h %h", acc[0], acc[1], acc[2], acc[3]);
    sel[0] = 2'bx1;
    #1 clk = 1; #1 clk = 0;
    $display("acc %b %b %b %b", acc[0], acc[1], acc[2], acc[3]);
    for (int k = 0; k < 14; k++) begin
      case (k)
        0: addr = MSTATUS; 1: addr = MISA; 2: addr = MIE; 3: addr = MTVEC;
        4: addr = MSCRATCH; 5: addr = MEPC; 6: addr = MCAUSE; 7: addr = MTVAL;
        8: addr = MCYCLE; 9: addr = MINSTRET; 10: addr = MHARTID;
        11: addr = csr_e'(12'h7c0); 12: addr = csr_e'(12'hx00); default: addr = csr_e'(12'h341);
      endcase
      op = 4'(k);
      #1 $display("%h %h %h %b %b %h", addr, rdata, aux, ill, cnt, res);
    end
  end
endmodule

//> acc 3 2 1 0
//> acc 11 10 01 00
//> 300 00001800 00000000 0 0 0a
//> 301 40101104 00000000 0 0 0b
//> 304 00000304 00000001 0 0 0c
//> 305 00000305 00000001 0 0 0d
//> 340 0000cafe 00000000 0 0 ff
//> 341 00000101 000000a1 0 0 ff
//> 342 00000102 000000a2 0 0 ff
//> 343 00000103 000000a3 0 0 ff
//> b00 00000077 00000000 0 1 ff
//> b02 00000077 00000000 0 1 13
//> f14 00000000 00000000 0 0 ff
//> 7c0 00000000 00000000 1 0 ff
//> x00 00000000 00000000 1 0 ff
//> 341 00000101 000000a1 0 0 ff
