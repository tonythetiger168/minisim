"""Shared environment and tool identity for the limited real-cocotb VPI host."""
import os,sys,re,hashlib,platform,subprocess,shutil
from importlib.metadata import version,PackageNotFoundError
from pathlib import Path
import find_libpython
from cocotb_tools import config
def libraries():
    python=find_libpython.find_libpython()
    if not python or not Path(python).is_file():raise RuntimeError('libpython unavailable')
    plugin=Path(config.lib_name_path('vpi','icarus')).resolve()
    if not plugin.is_file():raise RuntimeError('cocotb VPI library unavailable')
    return Path(python).resolve(),plugin
def environment(top,module,python_paths,xml,extra=None):
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*',top):raise ValueError('unsupported flat top name')
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.]*',module):raise ValueError('invalid test module')
    python,plugin=libraries()
    result={**os.environ,'MINISIM_COCOTB_VPI':str(plugin),'GPI_USERS':str(python)+';'+config.pygpi_entry_point(),'LIBPYTHON_LOC':str(python),'PYGPI_PYTHON_BIN':sys.executable,'PYTHONPATH':os.pathsep.join(str(Path(x).resolve()) for x in python_paths),'COCOTB_TEST_MODULES':module,'COCOTB_TOPLEVEL':top,'TOPLEVEL_LANG':'verilog','COCOTB_RESULTS_FILE':str(Path(xml).resolve()),'COCOTB_RANDOM_SEED':'168'}
    if extra:result.update(extra)
    return result
def identity():
    python,plugin=libraries();compiler=shutil.which('g++')
    if not compiler:raise RuntimeError('g++ unavailable')
    packages={}
    for package in ('cocotb','pyuvm','pyslang'):
        try:packages[package]=version(package)
        except PackageNotFoundError:packages[package]=None
    return {'python_version':platform.python_version(),'python_executable':sys.executable,'packages':packages,'compiler_path':compiler,'compiler_version':subprocess.check_output([compiler,'--version'],text=True,timeout=15).splitlines()[0],'libraries':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (python,plugin)}}
