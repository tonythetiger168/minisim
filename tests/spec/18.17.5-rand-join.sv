// IEEE 1800-2023 18.17.5: rand join interleaves the items of the listed
// productions at random, keeping each production's own items (and its
// code block) in order. With the default bias every production with items
// left is equally likely at each step; with 0.0 shorter ones go first.
//! icarus-skip: Icarus 13 does not support randsequence
module top;
  string s;
  int seen[string], bad, yfirst;
  function automatic bit ordered(string x, string a, string b);
    int i = -1, j = -1;
    for (int k = 0; k < x.len(); k++) begin
      if (x[k] == a[0] && i < 0) i = k;
      if (x[k] == b[0]) j = k;
    end
    return i >= 0 && j >= 0 && i < j;
  endfunction
  initial begin
    repeat (400) begin
      s = "";
      randsequence (TOP)
        TOP : rand join S1 S2 ;
        S1 : A B ;
        S2 : C D { s = {s, "."}; } ;
        A : { s = {s, "A"}; } ;
        B : { s = {s, "B"}; } ;
        C : { s = {s, "C"}; } ;
        D : { s = {s, "D"}; } ;
      endsequence
      seen[s] = 1;
      if (s.len() != 5 || !ordered(s, "A", "B") || !ordered(s, "C", "D") || !ordered(s, "D", ".")) bad++;
    end
    // all 10 interleavings of AB with CD. show up
    if (seen.num() != 10) bad++;
    repeat (200) begin
      s = "";
      randsequence (T2)
        T2 : rand join (0.0) X Y ;
        X : A B A B A B ;
        Y : C | D ;
        A : { s = {s, "A"}; } ;
        B : { s = {s, "B"}; } ;
        C : { s = {s, "C"}; } ;
        D : { s = {s, "D"}; } ;
      endsequence
      if (s.len() != 7) bad++;
      if (s[0] == "C" || s[0] == "D") yfirst++;
    end
    // 1 item against 6: Y first about 6 times in 7
    if (yfirst < 150) bad++;
    $display("bad %0d", bad);
  end
endmodule

//> bad 0
