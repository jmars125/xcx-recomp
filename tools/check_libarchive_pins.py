"""Checks deps/libarchive against libarchive's own release: every pinned file byte for byte, and every pin.

    python tools/check_libarchive_pins.py                       download the release named in pins.json and compare
    python tools/check_libarchive_pins.py --archive FILE        compare with a release archive on disk instead
    python tools/check_libarchive_pins.py --update VERSION      move to another release: copy the same files from it
                                                                into deps/libarchive and write pins.json again

The release archive is checked against the SHA-256 in pins.json (with --update, against --sha256, which the release
page gives) before anything is read from it. A pinned path maps to the archive as deps/libarchive/COPYING ->
libarchive-<version>/COPYING and deps/libarchive/pristine/libarchive/<file> -> libarchive-<version>/libarchive/<file>.
"""
import argparse
import hashlib
import json
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEP = ROOT / 'deps' / 'libarchive'
PINS = DEP / 'pins.json'
URL = 'https://github.com/libarchive/libarchive/releases/download/v{v}/libarchive-{v}.tar.xz'


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fetch(version, out):
    url = URL.format(v=version)
    print(f'downloading {url}')
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'xcx-pins-check'}), timeout=120) as r:
        out.write_bytes(r.read(64 << 20))
    return out


def member_name(pinned, version):
    top = f'libarchive-{version}/'
    if pinned == 'COPYING':
        return top + 'COPYING'
    prefix = 'pristine/libarchive/'
    if not pinned.startswith(prefix):
        raise ValueError(f'{pinned}: not a path this script maps to the release archive')
    return top + 'libarchive/' + pinned[len(prefix):]


def read_release(archive, version, names):
    """{pinned path: bytes} for names, from the release archive."""
    out = {}
    with tarfile.open(archive, 'r:xz') as tar:
        for pinned in names:
            try:
                f = tar.extractfile(member_name(pinned, version))
            except KeyError:
                f = None
            if f is None:
                raise ValueError(f'{pinned}: not in {Path(archive).name}')
            out[pinned] = f.read()
    return out


def check(archive):
    pins = json.loads(PINS.read_text(encoding='utf-8'))
    got = sha256(Path(archive).read_bytes())
    if got != pins['archive_sha256']:
        raise ValueError(f'{Path(archive).name} has SHA-256 {got}, pins.json says {pins["archive_sha256"]}')
    release = read_release(archive, pins['version'], pins['files'])
    bad = []
    for pinned, digest in pins['files'].items():
        here = (DEP / pinned).read_bytes() if (DEP / pinned).is_file() else None
        if here != release[pinned]:
            bad.append(f'{pinned}: differs from the release')
        elif sha256(here) != digest:
            bad.append(f'{pinned}: matches the release, but its pin is wrong')
    have = {p.relative_to(DEP).as_posix() for p in (DEP / 'pristine').rglob('*') if p.is_file()}
    bad += [f'{p}: not pinned' for p in sorted(have - set(pins['files']))]
    if bad:
        raise ValueError('\n'.join(bad))
    print(f'{len(pins["files"])} files match libarchive {pins["version"]} byte for byte, and every pin is right')


def update(archive, version, archive_sha):
    pins = json.loads(PINS.read_text(encoding='utf-8'))
    got = sha256(Path(archive).read_bytes())
    if got != archive_sha:
        raise ValueError(f'{Path(archive).name} has SHA-256 {got}, not the {archive_sha} given')
    release = read_release(archive, version, pins['files'])   # the same files; one gone from the release stops here
    for pinned, data in release.items():
        (DEP / pinned).write_bytes(data)
    pins.update(version=version, archive=f'libarchive-{version}.tar.xz', archive_sha256=archive_sha,
                files={k: sha256(v) for k, v in release.items()})
    PINS.write_text(json.dumps(pins, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(f'deps/libarchive now holds libarchive {version}; rebuild and run tests/mods_archive_test.py, and update '
          'the version in THIRD_PARTY_NOTICES.md and deps/libarchive/README.md')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--archive', type=Path, help='a release archive on disk (default: download it)')
    ap.add_argument('--update', metavar='VERSION', help='move deps/libarchive to this release')
    ap.add_argument('--sha256', help='with --update: the release archive\'s SHA-256, from its release page')
    args = ap.parse_args()
    if args.update and not args.sha256:
        ap.error('--update needs --sha256')
    version = args.update or json.loads(PINS.read_text(encoding='utf-8'))['version']
    try:
        with tempfile.TemporaryDirectory() as tmp:
            archive = args.archive or fetch(version, Path(tmp) / f'libarchive-{version}.tar.xz')
            if args.update:
                update(archive, args.update, args.sha256)
            else:
                check(archive)
    except (OSError, ValueError, tarfile.TarError) as e:
        sys.exit(str(e))


if __name__ == '__main__':
    main()
