#!/usr/bin/env python3
"""patch_tracer.py in.sv out.sv: workaround for msim's delta loop in CVA6 rv_tracer.
The address_to_compress always_comb writes a local and drives addr_to_compress_o
through one continuous assign, so it no longer makes a transient '0 on the output."""
import sys
s = open(sys.argv[1]).read()
i = s.index('always_comb begin : address_to_compress'); j = s.index('endmodule', i)
blk = s[i:j].replace('addr_to_compress_o', 'addr_c').replace(
    'always_comb begin : address_to_compress',
    'logic [te_pkg::XLEN-1:0] addr_c;\n    assign addr_to_compress_o = addr_c;\n    always_comb begin : address_to_compress', 1)
open(sys.argv[2], 'w').write(s[:i] + blk + s[j:])
