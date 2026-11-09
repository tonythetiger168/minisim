// IEEE 1800-2023 18.4: rand and randc class properties. randomize() gives
// each rand variable a value of its type; a randc variable goes through
// every value of its range once before any value repeats (18.4.2); enum
// variables take only named values; non-rand properties keep their value.
//! icarus-skip: Icarus 13 does not support randomize()
module top;
  typedef enum bit [3:0] {IDLE = 1, BUSY = 4, DONE = 9} st_t;
  class Packet;
    rand bit [7:0] addr;
    rand bit signed [3:0] off;
    randc bit [2:0] tag;
    rand st_t st;
    rand bit [1:0] lanes [4];
    int fixed = 42;
  endclass
  Packet p;
  int seen[8], cycleOk = 1, enumOk = 1, ok = 1, laneHits[4];
  initial begin
    p = new;
    for (int round = 0; round < 3; round++) begin
      foreach (seen[i]) seen[i] = 0;
      for (int k = 0; k < 8; k++) begin
        ok &= p.randomize();
        seen[p.tag]++;
        if (!(p.st inside {IDLE, BUSY, DONE})) enumOk = 0;
        foreach (p.lanes[i]) laneHits[p.lanes[i]]++;
      end
      foreach (seen[i]) if (seen[i] != 1) cycleOk = 0;
    end
    $display("%0d %0d %0d %0d", ok, cycleOk, enumOk, p.fixed);
    $display("%0d", laneHits[0] > 0 && laneHits[1] > 0 && laneHits[2] > 0 && laneHits[3] > 0);
  end
endmodule

//> 1 1 1 42
//> 1
