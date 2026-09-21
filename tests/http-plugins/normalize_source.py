"""Normalize Windows checkout line endings inside the disposable Linux image."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
extensions = {'.sh', '.m4', '.ac', '.am', '.in', '.c', '.h', '.cpp', '.xml', '.xsd'}
for path in root.rglob('*'):
    if path.is_file() and '.git' not in path.parts and (path.suffix in extensions or path.name == 'bootstrap'):
        data = path.read_bytes()
        if b'\r\n' in data:
            path.write_bytes(data.replace(b'\r\n', b'\n'))
