#!/usr/bin/env python3
"""Package the validated static site with a SHA-256 manifest for ColdLab."""
from pathlib import Path
import hashlib, io, json, re, sys, tarfile

root = Path('dist').resolve()
revision = sys.argv[1]
archive = Path(sys.argv[2])
if not re.fullmatch(r'[a-f0-9]{7,40}-[0-9]{1,12}', revision):
    raise SystemExit('Invalid release revision')
if not (root / 'index.html').is_file() or not (root / 'index.html').stat().st_size:
    raise SystemExit('Missing artifact: index.html')

files = {}
for path in sorted(root.rglob('*')):
    if path.is_symlink() or not (path.is_file() or path.is_dir()):
        raise ValueError('Links or special files in output')
    if not path.is_file():
        continue
    name = path.relative_to(root).as_posix()
    if any(part.startswith('.') for part in Path(name).parts):
        raise ValueError('Hidden file in output: ' + name)
    if path.suffix.lower() in {
        '.qmd', '.md', '.mdx', '.bib', '.key', '.pem', '.yml', '.yaml',
        '.toml', '.ini', '.sh', '.py', '.container', '.network', '.volume', '.pod',
    }:
        raise ValueError('Source/configuration in output: ' + name)
    if name in {'_coldlab_manifest.json', 'release.json'}:
        raise ValueError('Reserved artifact name: ' + name)
    files[name] = hashlib.sha256(path.read_bytes()).hexdigest()

with tarfile.open(archive, 'w:gz') as output:
    for name in files:
        output.add(root / name, arcname=name, recursive=False)
    manifest = json.dumps({'revision': revision, 'files': files}, sort_keys=True).encode()
    member = tarfile.TarInfo('_coldlab_manifest.json')
    member.size = len(manifest)
    output.addfile(member, io.BytesIO(manifest))

print(json.dumps({'revision': revision, 'files': len(files), 'type': 'static-site'}))
