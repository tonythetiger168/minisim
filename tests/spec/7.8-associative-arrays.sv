// IEEE 1800-2023 7.8, 7.9: associative arrays with integer and string keys.
// exists, num, delete, first/last/next/prev, and foreach in key order.
//! icarus-skip: Icarus 13 does not support associative arrays
module top;
  int aa[int];
  int sa[string];
  logic [7:0] ua[bit [3:0]];
  int k;
  string s;
  initial begin
    aa[5] = 50;
    aa[-3] = 30;
    aa[100] = 1000;
    $display("%0d %0d %0d", aa.num(), aa.size(), aa[5]);
    $display("%0d %0d", aa.exists(5), aa.exists(6));
    $display("%0d", aa[6]);
    foreach (aa[i]) $write("%0d=%0d ", i, aa[i]);
    $display("");
    if (aa.first(k)) $display("first %0d", k);
    if (aa.next(k)) $display("next %0d", k);
    if (aa.last(k)) $display("last %0d", k);
    if (aa.prev(k)) $display("prev %0d", k);
    k = 100;
    $display("%0d", aa.next(k));
    aa.delete(5);
    $display("%0d %0d", aa.num(), aa.exists(5));
    aa[-3] += 1;
    $display("%0d", aa[-3]);
    aa.delete();
    $display("%0d", aa.num());
    sa["bob"] = 2;
    sa["alice"] = 1;
    sa["carol"] = 3;
    foreach (sa[n]) $write("%s:%0d ", n, sa[n]);
    $display("");
    if (sa.first(s)) $display("%s", s);
    $display("%0d %0d", sa.exists("bob"), sa.exists("dave"));
    ua[4'hf] = 8'h11;
    ua[4'h1] = 8'h22;
    foreach (ua[i]) $write("%h=%h ", i, ua[i]);
    $display("");
    $display("%h", ua[4'h3]);
  end
endmodule

//> 3 3 50
//> 1 0
//> 0
//> -3=30 5=50 100=1000 
//> first -3
//> next 5
//> last 100
//> prev 5
//> 0
//> 2 0
//> 31
//> 0
//> alice:1 bob:2 carol:3 
//> alice
//> 1 0
//> 1=22 f=11 
//> xx
