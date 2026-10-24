"""Verify payload hashes before publishing. Never prints file contents or credentials."""
from pathlib import Path
import csv
import hashlib

root=Path(__file__).resolve().parents[1]
manifest=root/'SOURCE_MANIFEST.csv'
rows=list(csv.DictReader(manifest.open(encoding='utf-8',newline='')))
allowed={'SOURCE_MANIFEST.csv'}
for row in rows:
    path=Path(row['path'])
    if path.is_absolute() or '..' in path.parts:
        raise SystemExit('Unsafe manifest path')
    p=root/path
    if not p.is_file() or p.is_symlink() or hashlib.sha256(p.read_bytes()).hexdigest()!=row['sha256']:
        raise SystemExit('Missing/changed payload: '+row['path'])
    allowed.add(row['path'])
for p in root.rglob('*'):
    if not p.is_file():continue
    rel=p.relative_to(root)
    # These are local developer outputs, never listed or staged by the manifest.
    if any(part in {'.git','.pio','__pycache__','.venv'} for part in rel.parts):continue
    if rel.as_posix()=='Nexus_S1_Firmware/include/local_secrets.h':continue
    if rel.as_posix() not in allowed:
        raise SystemExit('Unreviewed file outside manifest: '+rel.as_posix())
print('PASS: '+str(len(rows))+' payload file hashes verified; local secrets/build outputs are outside the manifest.')
