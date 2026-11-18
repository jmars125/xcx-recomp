"""Builds xcx-unpack, the mod manager's RAR reader: installer/unpack.c with the RAR and RAR5 readers of libarchive.

    python tools/build_unpack.py [--cc CLANG] [--out FILE] [-- EXTRA CLANG FLAGS]

The libarchive files are in deps/libarchive/pristine/libarchive, unmodified, each pinned by SHA-256 in
deps/libarchive/pins.json; deps/libarchive/config.h is ours. The build checks every pin first and stops on a mismatch.
tools/make_release.py and tools/make_release_linux.py put the result in the release's bin/; in a checkout,
installer/archive.py runs build() on first use, into build/unpack/.
"""
import argparse
import binascii
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEP = ROOT / 'deps' / 'libarchive'
SOURCE = ROOT / 'installer' / 'unpack.c'
EXE = '.exe' if os.name == 'nt' else ''
NAME = 'xcx-unpack'

# The libarchive translation units the two RAR readers need (archive_windows.c is empty off Windows).
UNITS = [
    'archive_acl.c', 'archive_blake2s_ref.c', 'archive_blake2sp_ref.c', 'archive_check_magic.c', 'archive_crc32.c',
    'archive_entry.c', 'archive_entry_sparse.c', 'archive_entry_xattr.c', 'archive_options.c', 'archive_ppmd7.c',
    'archive_random.c', 'archive_read.c', 'archive_read_open_filename.c', 'archive_read_set_options.c',
    'archive_read_support_format_rar.c', 'archive_read_support_format_rar5.c', 'archive_string.c',
    'archive_string_sprintf.c', 'archive_time.c', 'archive_util.c', 'archive_virtual.c', 'archive_windows.c',
]


def check_pins():
    """Every pinned file has its hash and no unpinned file sits beside them; raises ValueError otherwise."""
    pins = json.loads((DEP / 'pins.json').read_text(encoding='utf-8'))
    for name, digest in pins['files'].items():
        path = DEP / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f'libarchive pin mismatch: {name}')
    have = {p.relative_to(DEP).as_posix() for p in (DEP / 'pristine').rglob('*') if p.is_file()}
    extra = have - set(pins['files'])
    if extra:
        raise ValueError(f'libarchive files without a pin: {", ".join(sorted(extra))}')
    return pins


def for_linux(extra):
    return any('linux' in str(e) for e in extra) or (os.name != 'nt' and sys.platform.startswith('linux'))


def commands(cc, out, objs, extra=()):
    """The compiler runs: our unpack.c with every warning as an error, libarchive's files as they are (their warnings
    are upstream's, and the files stay unmodified), then the link. On Linux all of it with the stack protector and
    _FORTIFY_SOURCE, as distributions build."""
    lib = DEP / 'pristine' / 'libarchive'
    base = [str(cc), '-O2', '-DHAVE_CONFIG_H', '-I', str(DEP), '-I', str(lib), *map(str, extra)]
    if for_linux(extra):
        base += ['-fstack-protector-strong', '-D_FORTIFY_SOURCE=2']
    # (a link flag among extra, such as -fuse-ld=lld, is unused while compiling)
    runs = [base + ['-Wall', '-Wextra', '-Werror', '-Wno-unused-command-line-argument', '-c', str(SOURCE),
                    '-o', str(objs / 'unpack.o')]]
    runs += [base + ['-w', '-c', str(lib / u), '-o', str(objs / (u[:-2] + '.o'))] for u in UNITS]
    link = [str(cc), *map(str, extra), str(objs / 'unpack.o'), *(str(objs / (u[:-2] + '.o')) for u in UNITS),
            '-o', str(out)]
    if os.name == 'nt' and not for_linux(extra):
        link.append('-lbcrypt')   # archive_random.c: BCryptGenRandom
        link.append('-municode')   # unpack.c's wmain
    return runs + [link + ['-Wl,-s']]


def build(cc, out, extra=(), env=None):
    """Compiles xcx-unpack to out with the clang cc; returns out. Raises ValueError on a pin mismatch and
    RuntimeError with the compiler's output when the build fails."""
    check_pins()
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + '.part')
    with tempfile.TemporaryDirectory(dir=out.parent, prefix='.unpack-obj-') as objs:
        for cmd in commands(cc, tmp, Path(objs), extra):
            r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=env)
            if r.returncode:
                tmp.unlink(missing_ok=True)
                raise RuntimeError((r.stdout + r.stderr).strip()[-4000:])
    os.replace(tmp, out)
    return out


def fixture(name, out_dir):
    """One of libarchive's test archives (uuencoded in deps/libarchive/pristine/libarchive/test), decoded."""
    data = bytearray()
    src = DEP / 'pristine' / 'libarchive' / 'test' / (name + '.uu')
    for line in src.read_text(encoding='ascii').splitlines()[1:]:
        if line in ('end', '`', ''):
            break
        data += binascii.a2b_uu(line)
    path = Path(out_dir) / name
    path.write_bytes(data)
    return path


def smoke(exe):
    """Runs a built xcx-unpack on two of libarchive's RAR5 test archives (stored and compressed) and on one with a
    symlink, which it must refuse; raises RuntimeError when it does not do all three right."""
    with tempfile.TemporaryDirectory() as tmp:
        for name, file, size in (('test_read_format_rar5_stored.rar', 'helloworld.txt', 29),
                                 ('test_read_format_rar5_compressed.rar', 'test.bin', 1200)):
            path, out = fixture(name, tmp), Path(tmp) / ('out-' + name)
            out.mkdir()
            r = subprocess.run([str(exe), 'extract', str(path), str(out), str(1 << 20)], capture_output=True,
                               text=True, encoding='utf-8', errors='replace')
            got = out / file
            if r.returncode or not got.is_file() or got.stat().st_size != size:
                raise RuntimeError(f'xcx-unpack failed on {name}: {r.stdout}{r.stderr}')
        link = fixture('test_read_format_rar5_symlink.rar', tmp)
        out = Path(tmp) / 'out-link'
        out.mkdir()
        r = subprocess.run([str(exe), 'extract', str(link), str(out), str(1 << 20)], capture_output=True, text=True,
                           encoding='utf-8', errors='replace')
        if r.returncode == 0 or 'unsafe_name' not in r.stdout:
            raise RuntimeError(f'xcx-unpack did not refuse a symlink: {r.stdout}{r.stderr}')


def copy_license(licenses):
    """libarchive's licence into a release's licenses/."""
    Path(licenses).mkdir(parents=True, exist_ok=True)
    shutil.copy2(DEP / 'COPYING', Path(licenses) / 'libarchive-COPYING.txt')


def stamp():
    """A hash of everything the tool is built from, so a checkout rebuilds it when one of them changes."""
    h = hashlib.sha256()
    for p in [SOURCE, DEP / 'config.h', DEP / 'pins.json', Path(__file__)]:
        h.update(p.read_bytes())
    return h.hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--cc', default='clang', help='clang to build with (default: clang on PATH)')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'unpack' / (NAME + EXE))
    ap.add_argument('extra', nargs='*', help='more clang flags, after --')
    args = ap.parse_args()
    try:
        print(build(args.cc, args.out, args.extra))
    except (ValueError, RuntimeError) as e:
        sys.exit(str(e))


if __name__ == '__main__':
    main()
