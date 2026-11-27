"""Reads the archives mods come in, for the mod manager (installer/mods.py): zip with Python's zipfile, RAR (4 and 5)
with xcx-unpack (installer/unpack.c over libarchive's readers, tools/build_unpack.py).

    kind_of(path)                   'zip', 'rar', '7z' or None, from the file's first bytes
    members(path, layout)           [Member]: every entry with its name, type and size
    extract(path, layout, picks, out, limit)   writes the picked members to out/<game path>

Every name is checked before anything is written: no absolute paths, drive letters or '..', and nothing but files
and folders (a link, device or encrypted entry stops the install). 7z is recognised so the player hears what it is,
but not read yet.
"""
import json
import os
import shutil
import subprocess
import tempfile
import zlib
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

CHUNK = 1 << 20
MAGIC = {b'PK\x03\x04': 'zip', b'PK\x05\x06': 'zip', b'Rar!\x1a\x07\x00': 'rar', b'Rar!\x1a\x07\x01\x00': 'rar',
         b"7z\xbc\xaf'\x1c": '7z'}
UNPACK = 'xcx-unpack' + ('.exe' if os.name == 'nt' else '')
MAX_ENTRIES = 100000   # far above any mod (installer/unpack.c, MAX_ENTRIES)
# COM and LPT with a superscript 1-3 too, which Windows 11 also takes for devices
DEVICES = {'CON', 'PRN', 'AUX', 'NUL', *(f'{d}{n}' for d in ('COM', 'LPT') for n in '0123456789¹²³')}


class ArchiveError(Exception):
    """A failure with a catalog id (installer/text/*.json) and its parameters, like mods.ModError."""

    def __init__(self, id, **params):
        super().__init__(id)
        self.id, self.params = id, params


@dataclass(frozen=True)
class Member:
    name: str          # as stored, '\' turned into '/', no trailing '/'
    type: str          # 'file', 'dir', 'link' or 'other'
    size: int
    safe: bool         # a relative name without '..', drive letters or control characters
    encrypted: bool = False


def kind_of(path):
    try:
        with open(path, 'rb') as f:
            head = f.read(8)
    except OSError as e:
        raise ArchiveError('archive.unreadable', detail=str(e)) from e
    return next((k for magic, k in MAGIC.items() if head.startswith(magic)), None)


def safe_name(name):
    """name with '/' separators when the manager may write it, else None (the rule of installer/unpack.c, safe_name):
    no absolute path, drive letter, control character, character Windows refuses in a name (< > " | ? *) or '.'/'..'
    part, no Windows device name (CON, aux.bdat) and no part ending in a dot or space (Windows drops that end, so two
    names could become one)."""
    name = name.replace('\\', '/').rstrip('/')
    if not name or name.startswith('/') or any(ord(c) < 0x20 or c in ':<>"|?*' for c in name):
        return None
    for part in name.split('/'):
        if not part or part[-1] in '. ' or part.split('.')[0].rstrip(' ').upper() in DEVICES:
            return None
    return name


# ---- xcx-unpack ----------------------------------------------------------------------------------------------------

def unpack_tool(layout):
    """The xcx-unpack of this setup: bin/ of a release; in a checkout build/unpack/, built (again) when its sources
    changed. Raises ArchiveError('archive.no_rar') when there is none and it cannot be built."""
    if not layout.dev:
        exe = layout.root / 'bin' / UNPACK
        if exe.is_file():
            return exe
        raise ArchiveError('archive.no_rar', detail=str(exe))
    import importlib.util
    spec = importlib.util.spec_from_file_location('build_unpack', layout.tools / 'build_unpack.py')
    build_unpack = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build_unpack)
    exe = layout.root / 'build' / 'unpack' / UNPACK
    stamp = exe.with_name(UNPACK + '.stamp')
    want = build_unpack.stamp(layout.clang)
    if exe.is_file() and stamp.is_file() and stamp.read_text().strip() == want:
        return exe
    try:
        build_unpack.build(layout.clang, exe)
    except (ValueError, RuntimeError, OSError) as e:
        raise ArchiveError('archive.no_rar', detail=str(e)[-400:]) from e
    stamp.write_text(want + '\n')
    return exe


def run_tool(exe, *args):
    """Runs xcx-unpack; its one JSON object on stdout, or ArchiveError with the id it reported."""
    flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    try:
        r = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, encoding='utf-8',
                           errors='replace', creationflags=flags, timeout=3600)
    except subprocess.TimeoutExpired as e:
        raise ArchiveError('archive.bad', detail=str(e)) from e
    except OSError as e:
        raise ArchiveError('archive.no_rar', detail=str(e)) from e
    try:
        out = json.loads(r.stdout)
    except ValueError:
        raise ArchiveError('archive.bad', detail=(r.stderr or r.stdout).strip()[-400:]) from None
    if r.returncode or 'error' in out:
        error, detail = out.get('error') or 'archive.bad', out.get('detail', '')
        if error == 'archive.unsafe_name':
            raise ArchiveError('mods.zip_unsafe', name=detail)
        if error == 'archive.too_big':
            raise ArchiveError('mods.zip_too_big')
        raise ArchiveError(error, detail=detail)
    return out


# ---- listing -------------------------------------------------------------------------------------------------------

def members(path, layout=None):
    """Every entry of the archive at path, in its order."""
    kind = kind_of(path)
    if kind == 'zip':
        try:
            with zipfile.ZipFile(path) as z:
                if len(z.infolist()) > MAX_ENTRIES:
                    raise ArchiveError('archive.too_many')
                out = []
                for info in z.infolist():
                    mode = (info.external_attr >> 16) & 0o170000
                    raw = info.filename.replace('\\', '/')
                    typ = 'link' if mode == 0o120000 else 'dir' if info.is_dir() else 'file'
                    out.append(Member(raw.rstrip('/'), typ, info.file_size, safe_name(raw) is not None,
                                      bool(info.flag_bits & 1)))
                return out
        except (OSError, zipfile.BadZipFile, ValueError) as e:
            raise ArchiveError('archive.bad', detail=str(e)) from e
    if kind == 'rar':
        listing = run_tool(unpack_tool(layout), 'list', path)
        return [Member(e['name'].replace('\\', '/').rstrip('/'), e['type'], int(e['size']), bool(e['safe']),
                       bool(e.get('encrypted'))) for e in listing['entries']]
    if kind == '7z':
        raise ArchiveError('archive.no_7z')
    raise ArchiveError('archive.unknown')


# ---- unpacking -----------------------------------------------------------------------------------------------------

def extract(path, layout, picks, out, limit):
    """Writes each picked member (name -> game path) to out/<game path>. out must not hold those files yet. Returns
    the number of files written."""
    out, picks = Path(out), dict(picks)
    if not picks:
        return 0
    kind = kind_of(path)
    if kind == 'zip':
        # the sizes checked in this open of the archive (the plan was read in another), each entry's output held to
        # its size by zipfile, which also checks its CRC; every picked member must be there
        try:
            with zipfile.ZipFile(path) as z:
                todo = [(info, picks[n]) for info in z.infolist()
                        if (n := info.filename.replace('\\', '/').rstrip('/')) in picks]
                if sum(info.file_size for info, _ in todo) > limit:
                    raise ArchiveError('mods.zip_too_big')
                if len({game for _, game in todo}) != len(picks):
                    raise ArchiveError('archive.bad', detail='the archive changed since it was read')
                for info, game in todo:
                    target = out / game
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with z.open(info) as src, open(target, 'xb') as dst:
                        shutil.copyfileobj(src, dst, CHUNK)
        except (zipfile.BadZipFile, zlib.error, NotImplementedError, RuntimeError, EOFError) as e:
            # a bad CRC or truncated data, a method zipfile can't read (Deflate64), an encrypted member
            raise ArchiveError('archive.bad', detail=str(e)) from e
        return len(todo)
    if kind != 'rar':
        raise ArchiveError('archive.unknown')
    # xcx-unpack writes the picked entries (named in a file; the rest are skipped and don't count toward limit) below a
    # folder of its own next to out (the same disk, so the files move instead of being copied), then they move on
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out.parent, prefix='.unpack-') as tmp:
        names, files = Path(tmp) / 'names.txt', Path(tmp) / 'files'
        names.write_text(''.join(n + '\n' for n in picks), encoding='utf-8', newline='\n')
        files.mkdir()
        run_tool(unpack_tool(layout), 'extract', path, files, limit, names)
        for name, game in picks.items():
            src = files.joinpath(*PurePosixPath(name).parts)
            target = out / game
            target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists():
                raise ArchiveError('archive.duplicate', name=game)
            os.replace(src, target)
    return len(picks)
