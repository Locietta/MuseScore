#!/usr/bin/env python3
"""Install or verify the engine's pinned assets; no QRC or desktop tools required."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'engrave/assets.json'


def relative(name):
    p = PurePosixPath(name)
    if not name or p.is_absolute() or '..' in p.parts or '\\' in name or ':' in name:
        raise ValueError(f'Unsafe asset path: {name}')
    return p


def checked(data, entry, name):
    if len(data) != entry['size'] or hashlib.sha256(data).hexdigest() != entry['sha256']:
        raise ValueError(f'Asset hash mismatch: {name}')
    return data


def install(destination, verify=False):
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest['version'] != 1:
        raise ValueError('Unsupported asset manifest version')
    for name, entry in manifest['files'].items():
        relative(name)
        relative(entry['source'])
    if verify:
        if (destination / 'assets.json').read_bytes() != manifest_bytes:
            raise ValueError('Installed manifest does not match the engine')
        for name, entry in manifest['files'].items():
            checked((destination / name).read_bytes(), entry, name)
        return
    # Never adopt or overwrite an existing directory. A failed install leaves
    # no partially published directory and cannot damage an embedder's assets.
    if destination.exists():
        raise FileExistsError(f'{destination}: use --verify or a fresh destination')
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='.engrave-assets-', dir=destination.parent))
    try:
        for name, entry in manifest['files'].items():
            # Committed blobs preserve text bytes even in CRLF worktrees.
            data = subprocess.check_output(['git', '-C', str(ROOT), 'cat-file', 'blob', 'HEAD:' + entry['source']])
            checked(data, entry, name)
            target = staging / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (staging / 'assets.json').write_bytes(manifest_bytes)
        staging.rename(destination)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    install(args.destination.resolve(), args.verify)
    print('Verified' if args.verify else 'Installed', args.destination)
