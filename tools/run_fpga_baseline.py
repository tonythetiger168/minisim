"""Small offline iCE40 mapping/P&R/bitstream experiment; never programs hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=Path('build/fpga-baseline'))
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    def run(argv, name):
        p = subprocess.run(argv, capture_output=True, text=True, timeout=120)
        (out / (name + '.log')).write_text(p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError(f'{name}: exit {p.returncode}; see log')
        return p.stdout + p.stderr
    for tool in ['yosys', 'nextpnr-ice40', 'icepack', 'iceunpack', 'iverilog', 'vvp']:
        if not shutil.which(tool):
            raise RuntimeError(f'missing tool: {tool}')
    rtl = '''module top(input clk, input rst, output [7:0] q);
reg [7:0] count;
always @(posedge clk) if(rst) count <= 0; else count <= count + 1;
assign q = count;
endmodule
'''
    (out / 'dut.v').write_text(rtl)
    (out / 'ref.v').write_text(rtl.replace('module top(', 'module reference('))
    (out / 'tb.v').write_text('''module tb;
reg clk=0, rst=1; wire [7:0] a,b;
top dut(clk,rst,a); reference refdut(clk,rst,b);
integer i;
initial begin
for(i=0;i<300;i=i+1) begin
rst=(i==0 || i==157); #5; clk=1; #1;
if(a !== b || (^a === 1'bx)) $fatal(1,"gate/reference mismatch");
#4; clk=0;
end
$display("PASS 300 gate/reference samples"); $finish;
end
endmodule
''')
    share = Path('/usr/share/yosys/ice40/cells_sim.v')
    if not share.exists():
        raise RuntimeError('Yosys simulation cells not found at ' + str(share))
    results = []
    for name, flags in [('default', ''), ('abc2', '-abc2'), ('nocarry', '-nocarry')]:
        stem = out / name
        run(['yosys', '-p', f'read_verilog {out}/dut.v; synth_ice40 {flags} -top top -json {stem}.json; write_verilog -noattr {stem}.v'], name + '-synth')
        run(['iverilog', '-g2012', '-DNO_ICE40_DEFAULT_ASSIGNMENTS', '-s', 'tb', '-o', str(stem)+'.vvp', str(out/'tb.v'), str(out/'ref.v'), str(stem)+'.v', str(share)], name+'-compile')
        trace = run(['vvp', str(stem)+'.vvp'], name+'-simulation')
        if 'PASS 300 gate/reference samples' not in trace:
            raise RuntimeError('missing positive gate simulation result')
        run(['nextpnr-ice40', '--hx1k', '--package', 'tq144', '--json', str(stem)+'.json', '--asc', str(stem)+'.asc', '--report', str(stem)+'-pnr.json', '--freq', '12', '--seed', '1', '--pcf-allow-unconstrained'], name+'-pnr')
        run(['icepack', str(stem)+'.asc', str(stem)+'.bin'], name+'-pack')
        run(['iceunpack', str(stem)+'.bin', str(stem)+'-roundtrip.asc'], name+'-unpack')
        run(['icepack', str(stem)+'-roundtrip.asc', str(stem)+'-roundtrip.bin'], name+'-repack')
        binary = Path(str(stem)+'.bin').read_bytes()
        if binary != Path(str(stem)+'-roundtrip.bin').read_bytes():
            raise RuntimeError('bitstream roundtrip mismatch')
        pnr = json.loads(Path(str(stem)+'-pnr.json').read_text())
        clocks = pnr.get('fmax', {})
        if not clocks or any(v['achieved'] < v['constraint'] for v in clocks.values()):
            raise RuntimeError('missing clock timing or timing constraint failure')
        cells = json.loads(Path(str(stem)+'.json').read_text())['modules']['top']['cells']
        types = [cell['type'] for cell in cells.values()]
        results.append(dict(variant=name, flags=flags, samples=300,
                            mapped_luts=types.count('SB_LUT4'),
                            mapped_ffs=sum(t.startswith('SB_DFF') for t in types),
                            bitstream_bytes=len(binary), sha256=hashlib.sha256(binary).hexdigest(),
                            utilization=pnr['utilization'], fmax=pnr.get('fmax'), roundtrip=True))
    report = dict(target='iCE40 HX1K TQ144', clock_mhz=12, seed=1,
                  constraints='auto IO placement; not a board pinout',
                  limitations='300 two-state samples, not formal equivalence or hardware validation; best among measured candidates only',
                  versions={'yosys':run(['yosys','-V'],'yosys-version').strip(),
                            'nextpnr':run(['nextpnr-ice40','--version'],'nextpnr-version').strip()},
                  source_sha256=hashlib.sha256(rtl.encode()).hexdigest(), results=results)
    report['pareto_candidates'] = [r['variant'] for r in results if not any(
        x['utilization']['ICESTORM_LC']['used'] <= r['utilization']['ICESTORM_LC']['used']
        and x['bitstream_bytes'] <= r['bitstream_bytes']
        and (x['utilization']['ICESTORM_LC']['used'] < r['utilization']['ICESTORM_LC']['used']
             or x['bitstream_bytes'] < r['bitstream_bytes']) for x in results)]
    (out/'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
