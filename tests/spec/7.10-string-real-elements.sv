// IEEE 1800-2023 7.5, 7.10, 7.8: containers of string and real elements;
// sort() orders strings, pop_back() returns a string, a string key.
//! icarus-skip: Icarus 13 has no sort() and no string-keyed associative arrays
module top;
  string sq[$];
  real rd[];
  string s;
  real r;
  int aa[string];
  initial begin
    sq.push_back("pear"); sq.push_back("apple"); sq.push_front("fig");
    sq.sort();
    foreach (sq[i]) $write("%s ", sq[i]);
    $display("");
    s = sq.pop_back();
    $display("%s %0d", s, sq.size());
    rd = new[3];
    rd[0] = 1.5; rd[2] = rd[0] * 2;
    $display("%f %f %f", rd[0], rd[1], rd[2]);
    r = rd[2] + 0.25;
    $display("%f", r);
    aa[s] = 7;
    $display("%0d", aa["pear"]);
  end
endmodule

//> apple fig pear 
//> pear 2
//> 1.500000 0.000000 3.000000
//> 3.250000
//> 7
