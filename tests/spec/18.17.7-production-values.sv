// IEEE 1800-2023 18.17.7: randsequence productions with input arguments
// and return values. A production's value is named in the calling rule's
// code block by the production's name, or name[i] when it is used more
// than once. Recursive productions keep their own arguments and values,
// also inside a class method.
//! icarus-skip: Icarus 13 does not support randsequence
class Gen;
  int log[$];
  task run(int base);
    int total;
    randsequence (top)
      top : put(base) put(base + 1) twice(base) { total = twice; } ;
      void put(int v) : { log.push_back(v); } ;
      int twice(int v) : { return 2 * v; } ;
    endsequence
    log.push_back(total);
  endtask
endclass

module top;
  int r, s;
  Gen g = new;
  initial begin
    randsequence (main)
      main : fact(5) { r = fact; } ;
      int fact(int n) : if (n <= 1) one else rec(n) { return n <= 1 ? one : rec; } ;
      int one : { return 1; } ;
      int rec(int n) : fact(n - 1) { return n * fact; } ;
    endsequence
    $display("%0d", r);
    randsequence (top2)
      top2 : sum3 { s = sum3; } ;
      int sum3 : val(1) val(10) val(100) { return val[1] + val[2] + val[3]; } ;
      int val(int k) : { return k * 2; } ;
    endsequence
    $display("%0d", s);
    g.run(20);
    $display("%p", g.log);
  end
endmodule

//> 120
//> 222
//> '{20, 21, 40}
