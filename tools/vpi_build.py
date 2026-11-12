"""Shared flat-RTL real-cocotb host build. No implicit reuse of an old binary."""
import argparse,json,subprocess,shutil
import fcntl
from pathlib import Path
from pyslang_lower import Lowerer,cpp_header
from vpi_manifest import build_manifest,manifest_header
from vpi_provenance import capture,verify,digest
from vpi_runtime import libraries,identity
ROOT=Path(__file__).resolve().parents[1]
def build(source,output,test_files=()):
    out = Path(output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Keep the lock file so contenders always lock the same inode.
    with (out / '.build.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError(f'VPI build already in progress in {out}') from error
        return _build(Path(source).resolve(), out, test_files)


def _build(source, out, test_files):
    model=Lowerer(source).lower();manifest=build_manifest(source)
    if model['source_sha256']!=manifest['source_sha256']:raise RuntimeError('RTL changed during lowering')
    (out/'design.hpp').write_text(cpp_header(model));(out/'design.json').write_text(json.dumps(model,indent=2))
    (out/'vpi_manifest.hpp').write_text(manifest_header(manifest));(out/'vpi_manifest.json').write_text(json.dumps(manifest,indent=2))
    compiler=shutil.which('g++')
    if not compiler:raise RuntimeError('g++ unavailable')
    inputs=[source,out/'design.hpp',out/'vpi_manifest.hpp',ROOT/'tests/vpi_generic_main.cpp',ROOT/'src/minisim.cpp',Path(compiler).resolve()]
    inputs+=sorted((ROOT/'src').glob('*.hpp'))+sorted((ROOT/'src').glob('*.inc'))
    inputs+=[ROOT/'tools'/name for name in ('vpi_build.py','pyslang_lower.py','vpi_manifest.py','vpi_provenance.py','vpi_runtime.py')]
    inputs+=[*libraries(),*[Path(p).resolve() for p in test_files]]
    inputs=list(dict.fromkeys(inputs));before={str(p):digest(p) for p in inputs}
    if before[str(source)]!=model['source_sha256']:raise RuntimeError('RTL changed after lowering')
    temporary=out/'simulator.building'
    cmd=[compiler,'-std=c++20','-O0','-rdynamic','-I'+str(ROOT/'src'),'-I'+str(out),'-I/usr/include/iverilog',str(ROOT/'tests/vpi_generic_main.cpp'),'-ldl','-o',str(temporary)]
    with (out/'build.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
    if any(digest(p)!=sha for p,sha in before.items()):raise RuntimeError('VPI build input changed during compilation')
    # Resolve runtime metadata before publishing the replacement binary.
    runtime=identity()
    if any(digest(p)!=sha for p,sha in before.items()):raise RuntimeError('VPI build input changed during runtime discovery')
    temporary.replace(out/'simulator')
    (out/'build-command.json').write_text(json.dumps(cmd,indent=2));(out/'runtime.json').write_text(json.dumps(runtime,indent=2))
    capture(out/'simulator',inputs,out/'provenance.json');verify(out/'provenance.json')
    return out/'simulator'
def main():
    p=argparse.ArgumentParser();p.add_argument('source',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args();print(build(a.source,a.output))
if __name__=='__main__':main()
