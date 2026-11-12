// msim -xprop: nested if, temporaries, loops, casez, a latch, memories, a default
// assignment before an if, unique case, messages skipped in an unknown branch.
//! args: -xprop
module top;
  logic a, b, c; logic [7:0] y, z, v, lat, tmp, cnt; logic [3:0] sel;
  logic [7:0] mem [0:3]; logic [1:0] ad; logic clk, we; logic [7:0] d, q2, q3, q4; logic [7:0] arr [0:3];
  function automatic [7:0] inc(input [7:0] x); return x + 8'd1; endfunction
  // nested
  always_comb begin
    y = 8'h00;
    if (a) begin
      if (b) y = 8'h0f; else y = 8'h0c;
    end else y = 8'hf0;
  end
  // temp + partial + function + loop
  always_comb begin
    tmp = 8'h11;
    z = 8'hff;
    if (c) begin
      tmp = inc(d);
      z[3:0] = tmp[3:0];
    end
    for (int i = 4; i < 8; i++) if (a) z[i] = 1'b0;
    v = tmp;
  end
  // casez with wildcards, unique case, priority if
  always_comb begin
    cnt = 8'd0;
    casez (sel)
      4'b1???: cnt = 8'd1;
      4'b01??: cnt = 8'd2;
      4'b001?: cnt = 8'd3;
      default: cnt = 8'd4;
    endcase
  end
  // latch
  always_latch if (a) lat = d;
  // flops: array, delayed nba, eager, display in branch
  always @(posedge clk) begin
    if (we) mem[ad] <= d;
    q2 <= 8'h00;
    if (we) q2 <= d; else if (a) q2 <= 8'h01;
    if (we) $display("  write %h", d);
    unique case (ad)
      2'd0: q3 <= 8'h10;
      2'd1: q3 <= 8'h11;
      2'd2: q3 <= 8'h12;
      2'd3: q3 <= 8'h13;
    endcase
    for (int i = 0; i < 4; i++) begin
      if (d[i]) begin q4[i] <= 1'b1; end
      else if (we) q4[i] <= 1'b0;
    end
  end
  task tick; begin clk = 0; #2 clk = 1; #2; end endtask
  initial begin
    a = 0; b = 0; c = 0; d = 8'h3c; sel = 4'b0000; we = 0; ad = 0; #1;
    $display("y=%b z=%b v=%b cnt=%0d lat=%b", y, z, v, cnt, lat);
    a = 1'bx; #1 $display("a=x: y=%b z=%b lat=%b", y, z, lat);
    a = 1; b = 1'bx; #1 $display("b=x: y=%b lat=%b", y, lat);
    a = 1'bx; #1 $display("a=x b=x: y=%b lat=%b", y, lat);
    a = 0; c = 1'bx; #1 $display("c=x: z=%b v=%b", z, v);
    sel = 4'bx000; #1 $display("sel=x000 cnt=%b", cnt);
    sel = 4'b1x0x; #1 $display("sel=1x0x cnt=%b", cnt);
    sel = 4'b0x1z; #1 $display("sel=0x1z cnt=%b", cnt);
    sel = 4'b00x0; #1 $display("sel=00x0 cnt=%b", cnt);
    c = 0; a = 0;
    we = 1; ad = 1; d = 8'ha5; tick; $display("mem1=%h  q2=%h q3=%h q4=%b", mem[1], q2, q3, q4);
    we = 1'bx; d = 8'ha6; tick; $display("we=x: mem1=%b q2=%b q3=%h q4=%b", mem[1], q2, q3, q4);
    we = 0; ad = 2'b1x; d = 8'h0f; tick; $display("ad=1x: q2=%b q3=%b q4=%b", q2, q3, q4);
    we = 1; ad = 2'bxx; d = 8'h55; tick; $display("ad=xx: mem=%h %h %h %h q3=%b", mem[0], mem[1], mem[2], mem[3], q3);
    $finish;
  end
endmodule
//> y=11110000 z=11111111 v=00010001 cnt=4 lat=xxxxxxxx
//> a=x: y=xxxxxx00 z=xxxx1111 lat=xxxxxxxx
//> b=x: y=000011xx lat=00111100
//> a=x b=x: y=xxxxxxxx lat=00111100
//> c=x: z=111111x1 v=00x1xx01
//> sel=x000 cnt=00000x0x
//> sel=1x0x cnt=00000001
//> sel=0x1z cnt=00000xxx
//> sel=00x0 cnt=00000xxx
//>   write a5
//> mem1=a5  q2=a5 q3=11 q4=xxxx0101
//> we=x: mem1=101001xx q2=x0x00xx0 q3=11 q4=xxxxx11x
//> ad=1x: q2=00000000 q3=0001001x q4=xxxx1111
//>   write 55
//> ad=xx: mem=xx xX xx xx q3=000100xx
