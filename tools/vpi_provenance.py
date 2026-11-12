"""Content identity checks; capture must be called by the successful build path."""
import hashlib,json
from pathlib import Path
def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def capture(binary,inputs,output):
    binary=Path(binary).resolve();paths=[Path(p).resolve() for p in inputs]
    if not paths or len(set(paths))!=len(paths) or binary in paths:raise ValueError('invalid provenance inputs')
    record={'schema':'minisim-vpi-provenance-v1','binary':{'path':str(binary),'sha256':digest(binary)},'inputs':[{'path':str(p),'sha256':digest(p)} for p in paths]}
    Path(output).write_text(json.dumps(record,indent=2)+'\n');return record
def verify(record_path):
    record=json.loads(Path(record_path).read_text())
    if record.get('schema')!='minisim-vpi-provenance-v1' or not record.get('inputs'):raise ValueError('invalid provenance schema or empty inputs')
    entries=[record['binary']]+record['inputs'];paths=[x['path'] for x in entries]
    if len(set(paths))!=len(paths):raise ValueError('duplicate provenance path')
    for entry in entries:
        if digest(entry['path'])!=entry['sha256']:raise ValueError('stale VPI artifact: '+entry['path'])
    return record
