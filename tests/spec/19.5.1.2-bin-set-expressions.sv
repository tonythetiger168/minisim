// IEEE 1800-2023 19.5.1.2: bins given by a set expression, an array or a
// queue of values: `bins b[] = arr` makes one bin per value, `bins b = arr`
// one bin holding all of them. The values are read when the covergroup is
// constructed.
//! icarus-skip: Icarus 13 has no covergroups
module top;
  bit [3:0] a;
  int vals[] = '{1, 3, 5};
  int fx[2] = '{7, 9};
  int q[$] = {2, 4};
  covergroup cg;
    ca: coverpoint a { bins odd[] = vals; bins fixed = fx; bins ev[] = q; }
  endgroup
  cg c = new;
  initial begin
    for (int i = 0; i < 6; i++) begin
      a = i;
      c.sample();
    end
    $display("%0.2f", c.ca.get_inst_coverage());
    a = 9;
    c.sample();
    $display("%0.2f", c.ca.get_inst_coverage());
  end
endmodule

//> 83.33
//> 100.00
