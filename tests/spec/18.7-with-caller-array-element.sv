// IEEE 1800-2023 18.7: in randomize() with {...}, a name that is not a
// member of the object being randomized is the caller's. An element of the
// caller's queue or fixed-size array, by an index the caller knows (a loop
// variable), reads the caller's value.
//! icarus-skip: Icarus 13 does not support constraints
class packet;
  rand bit [6:0] addr;
  rand bit [7:0] code;
endclass
class sequencer;
  bit [6:0] targets[$] = '{7'h20, 7'h2a, 7'h31};
  bit [7:0] codes[3] = '{8'h07, 8'h11, 8'hf0};
  int hits = 0;
  task run();
    packet p = new;
    foreach (targets[i]) begin
      if (!p.randomize() with { addr == targets[i]; code == codes[i]; }) $display("randomize failed %0d", i);
      else $display("%0d addr %h code %h", i, p.addr, p.code);
      if (p.addr == targets[i] && p.code == codes[i]) hits++;
    end
    for (int j = 0; j < 2; j++) begin
      if (!p.randomize() with { addr == targets[j + 1] + 1; }) $display("randomize failed j %0d", j);
      else $display("j %0d addr %h", j, p.addr);
    end
    $display("hits %0d", hits);
  endtask
endclass
module top;
  initial begin
    sequencer s;
    s = new;
    s.run();
  end
endmodule
//> 0 addr 20 code 07
//> 1 addr 2a code 11
//> 2 addr 31 code f0
//> j 0 addr 2b
//> j 1 addr 32
//> hits 3
