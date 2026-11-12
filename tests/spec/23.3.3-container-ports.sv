// IEEE 1800-2023 23.3.3 with 7.5-7.9: queue, dynamic array and associative
// array ports. A child reading an input container sees each change the
// parent makes; a container a child writes through an output port is the
// parent's.
//! icarus-skip: Icarus does not support container ports
module sizer(input int q[$], output int n, output int total);
  always @(q.size()) n = q.size();
  initial begin
    #5 total = 0;
    foreach (q[i]) total += q[i];
  end
endmodule
module maker(output int d[]);
  initial begin
    d = new[3];
    foreach (d[i]) d[i] = 7 * (i + 1);
  end
endmodule
module namer(ref string names[int]);
  initial #3 names[42] = "answer";
endmodule
module top;
  int q[$];
  int d[];
  int n, total;
  string names[int];
  sizer s(.q(q), .n(n), .total(total));
  maker m(.d(d));
  namer a(.names(names));
  initial begin
    q.push_back(3); q.push_back(4);
    #1 $display("n=%0d", n);
    q.push_back(5);
    #1 $display("n=%0d d=%p", n, d);
    #2 $display("names=%p", names);
    #2 $display("total=%0d", total);
  end
endmodule

//> n=2
//> n=3 d='{7, 14, 21}
//> names='{42:"answer"}
//> total=12
