"""Generate the limited flat VPI host interface from the elaborated RTL ports."""
import argparse,json
from pathlib import Path
from pyslang_lower import Lowerer,Unsupported,kind
def build_manifest(source):
    lowerer=Lowerer(source);model=lowerer.lower();top=lowerer.compilation.getRoot().topInstances[0]
    ports={}
    for port in top.body:
        if kind(port)!='Port':continue
        direction=str(port.direction).split('.')[-1]
        if direction not in ('In','Out'):raise Unsupported('VPI manifest requires input/output ports')
        if port.internalSymbol is None or port.internalSymbol.name!=port.name:raise Unsupported('VPI port alias is unsupported')
        width=port.type.bitWidth
        if model['signals'].get(port.name)!=width:raise Unsupported('VPI port does not match generated signal')
        ports[port.name]={'direction':'input' if direction=='In' else 'output','width':width}
    if not ports:raise Unsupported('empty VPI interface')
    return {'schema':'minisim-flat-vpi-manifest-v1','top':model['top'],'source_sha256':model['source_sha256'],'ports':ports}
def manifest_header(manifest):
    outputs=[name for name,port in manifest['ports'].items() if port['direction']=='output']
    return 'const std::string dut_top='+json.dumps(manifest['top'])+';\nconst std::vector<std::string> dut_outputs={'+','.join(json.dumps(x) for x in outputs)+'};\n'
def main():
    p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    m=build_manifest(a.source);a.output.mkdir(parents=True,exist_ok=True)
    (a.output/'vpi_manifest.json').write_text(json.dumps(m,indent=2)+'\n');(a.output/'vpi_manifest.hpp').write_text(manifest_header(m));print(json.dumps(m))
if __name__=='__main__':main()
