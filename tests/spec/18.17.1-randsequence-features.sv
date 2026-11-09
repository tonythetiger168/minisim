// IEEE 1800-2023 18.17: randsequence. Alternatives picked by weight (:=,
// 18.17.1), if-else, case and repeat productions (18.17.2-18.17.4),
// recursive productions, and break / return in code blocks (18.17.6).
//! icarus-skip: Icarus 13 does not support randsequence
module top;
  int n[3], depth, total, cnt;
  string log;
  initial begin
    repeat (300) randsequence (top)
      top : a := 1 | b := 2 | c := 0;
      a : { n[0]++; };
      b : { n[1]++; };
      c : { n[2]++; };
    endsequence
    $display("%0d %0d %0d", n[0] > 50, n[1] > n[0], n[2]);
    for (int mode = 0; mode < 3; mode++)
      randsequence (main)
        main : sel tail;
        sel : if (mode == 0) x else y;
        x : { log = {log, "x"}; };
        y : case (mode) 1: p; default: q; endcase;
        p : { log = {log, "p"}; };
        q : { log = {log, "q"}; };
        tail : repeat (2) dot;
        dot : { log = {log, "."}; };
      endsequence
    $display("%s", log);
    randsequence (list)
      list : item | item list;
      item : { depth++; if (depth == 5) break; };
    endsequence
    $display("%0d", depth <= 5);
    randsequence (seq)
      seq : one two three;
      one : { total += 1; };
      two : { total += 10; return; total += 1000; };
      three : { total += 100; };
    endsequence
    $display("%0d", total);
  end
endmodule

//> 1 1 0
//> x..p..q..
//> 1
//> 111
