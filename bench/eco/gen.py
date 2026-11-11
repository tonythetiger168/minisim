#!/usr/bin/env python3
"""A pipelined adder datapath as a gate netlist, for timing meco.
   gen.py STAGES BITS old.v new.v   (new.v has three gates changed)"""
import sys
stages, bits = int(sys.argv[1]), int(sys.argv[2])

def emit(path, eco, new):
    L = ['module top (clk, rst_n, a, b, y);', '  input clk, rst_n;', '  input [%d:0] a, b;' % (bits - 1),
         '  output [%d:0] y;' % (bits - 1)]
    p = 'g' if new else 'u'      # synthesis names its gates afresh
    w = 'm' if new else 'n'
    body = []
    for s in range(stages):
        x = ['a[%d]' % i for i in range(bits)] if s == 0 else ['q%d_%d' % (s - 1, i) for i in range(bits)]
        yv = ['b[%d]' % i for i in range(bits)] if s == 0 else ['q%d_%d' % (s - 1, (i * 7 + 3) % bits) for i in range(bits)]
        c = "1'b0"
        for i in range(bits):
            t = '%s%d_%d' % (w, s, i)
            k = (s, i)
            g1 = 'OR2_X1' if eco and k == (stages // 3, bits // 2) else 'AND2_X1'
            g2 = 'NOR2_X1' if eco and k == (stages // 2, 1) else 'OR2_X1'
            g3 = 'XOR2_X1'
            body += ['  XOR2_X1 %sx%d_%d (.A(%s), .B(%s), .Y(%s_p));' % (p, s, i, x[i], yv[i], t),
                     '  %s %ss%d_%d (.A(%s_p), .B(%s), .Y(%s_s));' % (g3, p, s, i, t, c, t),
                     '  %s %sa%d_%d (.A(%s), .B(%s), .Y(%s_g));' % (g1, p, s, i, x[i], yv[i], t),
                     '  AND2_X1 %sb%d_%d (.A(%s_p), .B(%s), .Y(%s_h));' % (p, s, i, t, c, t),
                     '  %s %so%d_%d (.A(%s_g), .B(%s_h), .Y(%s_c));' % (g2, p, s, i, t, t, t)]
            d = '%s_s' % t
            if eco and k == (2 * stages // 3, bits - 1):
                body.append('  XOR2_X1 %se%d_%d (.A(%s_s), .B(%s), .Y(%s_e));' % (p, s, i, t, x[0], t))
                d = '%s_e' % t
            body.append('  DFFR_X1 \\r%d_reg[%d] (.CK(clk), .D(%s), .RN(rst_n), .Q(q%d_%d));' % (s, i, d, s, i))
            c = '%s_c' % t
    body += ['  BUF_X1 %sy%d (.A(q%d_%d), .Y(y[%d]));' % (p, i, stages - 1, i, i) for i in range(bits)]
    open(path, 'w').write('\n'.join(L + body + ['endmodule']) + '\n')

emit(sys.argv[3], False, False)
emit(sys.argv[4], True, True)
