// IEEE 1800-2023 19.5.1.2: a bin whose values come from a container
// (`bins b = q;`, `bins b[] = q;`) on a coverpoint wider than 64 bits. The
// container's values are taken when the covergroup is made, widened to
// the coverpoint type.
//! icarus-skip: Icarus 13 does not support covergroups
module t;
  bit [95:0] v;
  int q[$] = '{3, 7, 11};
  longint unsigned d[] = '{64'hffff_ffff_ffff_fff0, 5};
  covergroup cg;
    coverpoint v {
      bins one = q;        // a single bin of 3, 7 and 11
      bins each[] = d;     // one bin per value
    }
  endgroup
  cg c;
  task automatic hit(bit [95:0] x);
    v = x; c.sample();
    $display("%.2f", c.get_coverage());
  endtask
  initial begin
    c = new;
    q.push_back(20);   // after new: not in the bins
    hit(20);
    hit(3);                          // one
    hit(5);                          // each[5]
    hit(96'h1_0000_0000_0000_0005);  // above 2^64: in no bin
    hit(11);                         // one (already)
    hit(64'hffff_ffff_ffff_fff0);    // each[...]
  end
endmodule
//> 0.00
//> 33.33
//> 66.67
//> 66.67
//> 66.67
//> 100.00
