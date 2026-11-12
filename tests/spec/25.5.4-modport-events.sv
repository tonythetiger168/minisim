// IEEE 1800-2023 25.5.4 and 25.7.4: a modport expression port can be waited
// on (@(s.high)) as well as read and written, and an exported task and
// function reached through an importing modport run the exporting module's
// body, a task with a delay included.
//! icarus-skip: Icarus does not support modport expressions or exported tasks
interface link_if;
  logic [7:0] word;
  logic [3:0] tag;
  extern task send(input int v);
  extern function int scale(int v);
  modport split(input .low(word[3:0]), input .high(word[7:4]), output .t(tag));
  modport server(export send, export scale);
  modport client(import send, import scale);
endinterface
module splitter(link_if.split s);
  wire [3:0] lo = s.low;
  always @(s.high) begin
    s.t = s.high ^ 4'hf;
    $display("%0t high=%h lo=%h", $time, s.high, lo);
  end
endmodule
module server(link_if.server p);
  int total = 0;
  task p.send(input int v);
    #1 total += v;
  endtask
  function int p.scale(int v);
    return v * 3;
  endfunction
endmodule
module client(link_if.client c);
  initial begin
    #10 c.send(c.scale(5));
    c.send(2);
  end
endmodule
module top;
  link_if l();
  splitter sp(l);
  server sv(l);
  client cl(l);
  initial begin
    l.word = 8'h3a;
    #1 l.word = 8'h7a;
    #1 $display("tag=%h", l.tag);
    #20 $display("total=%0d", sv.total);
  end
endmodule

//> 0 high=3 lo=a
//> 1 high=7 lo=a
//> tag=8
//> total=17
