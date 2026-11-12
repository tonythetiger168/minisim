"""Inspect the installed cocotb wheel's real VPI ABI; never claim integration."""
import argparse
import hashlib
from importlib.metadata import version
import json
from pathlib import Path
import re
import subprocess
from cocotb_tools import config


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--output',type=Path,default=Path('build/cocotb-interface.json'))
    a=p.parse_args()
    library=Path(config.lib_name_path('vpi','icarus')).resolve()
    if not library.is_file(): raise RuntimeError('Installed cocotb VPI library missing')
    symbols=subprocess.check_output(['nm','-D','--undefined-only',str(library)],text=True)
    required=sorted(set(re.findall(r'\b(vpi_[A-Za-z0-9_]+)(?:@\S+)?\s*$',symbols,re.M)))
    if not required: raise RuntimeError('No VPI imports found; refusing empty interface report')
    import cocotb
    include=Path(cocotb.__file__).parent/'share/include'
    header=Path('/usr/include/iverilog/vpi_user.h')
    if not header.exists(): raise RuntimeError('Standard VPI header missing')
    probe=Path(__file__).resolve().parents[1]/'tests/vpi_header_probe.cpp'
    command=['g++','-std=c++20','-fsyntax-only','-I',str(header.parent),str(probe)]
    subprocess.run(command,check=True,capture_output=True,text=True,timeout=30)
    report=dict(cocotb_version=version('cocotb'),pyuvm_version=version('pyuvm'),
                library=str(library),library_sha256=hashlib.sha256(library.read_bytes()).hexdigest(),
                required_vpi_symbols=required,required_symbol_count=len(required),
                gpi_public_header_available=(include/'gpi.h').exists(),
                gpi_private_header_available=any(include.rglob('gpi_priv*')),
                standard_vpi_header=str(header) if header.exists() else None,
                header_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
                header_compile_check='pass',header_compile_command=command,
                route='Implement real VPI entry points and bootstrap the installed cocotb VPI library',
                status='interface_audit_only',
                limitations=['Icarus-flavor ABI is a candidate reuse route, not a validated Minisim backend',
                             'Imported symbols are necessary but not sufficient; unsupported runtime paths must fail explicitly',
                             'No cocotb scheduler or pyuvm test executed by this probe'])
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))


if __name__=='__main__': main()
