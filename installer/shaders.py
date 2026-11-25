"""The shader files of an install folder, as the setup's Prepare step (installer/steps.py) and the window
(installer/guilogic.py has_shaders) see them. Kept apart from steps.py, so the window's per-change check imports only
this. The rules follow the game's (runtime/gfx_shader.c); each one names its partner there.
"""
import os
import re
import shutil
from pathlib import Path

CHOICES = ('now', 'background', 'off')   # --prepare-shaders; the window's shader box, in this order
SEED_FILES = ('seed-programs.bin', 'seed-answers.bin')   # tools/prepare_shaders.py; game-derived, install folder only
PREPARED = ('shader-prepared.bin', 'shader-prepared.id')   # xcx.exe --prepare-shaders: the list, its translator
CLEAR_MARK = 'shader-cache.clear'   # the game's menu "Clear shader cache" (gfx_shader.c CLEAR_MARK)
DEFAULT_CACHE = 'shader-cache'
PRP_MAGIC, PRP_HEADER, PRP_REC = b'XCXSPRP\0', 16, 36   # gfx_shader.c PRP_*: magic, version 1, record size; id at +4
PATH_CAP = 1024   # gfx_shader.c PATH_CAP: a longer XCX_SHADER_CACHE is ignored there, so here too
# The names the cache writes (gfx_shader.c clear_listed): <64 hex><kind>, in/<64 hex>.in, and the
# <name>.<pid>.<tid>.tmp of an interrupted atomic write of either
OWN_FILE = re.compile(r'[0-9a-f]{64}(\.spv|\.res\.json|\.use|\.code|\.job)(\.\d+\.\d+\.tmp)?')
OWN_INPUT = re.compile(r'[0-9a-f]{64}\.in(\.\d+\.\d+\.tmp)?')


def ini_value(dest, key):
    """A key's value in dest/xcx.ini as the game reads it (runtime/cfg.c: "key = value" lines, '#' or ';' comments,
    [sections] only group, the key's case and an XCX_ prefix ignored, quotes stripped, the last line wins, a UTF-8 BOM
    skipped), or None."""
    value = None
    try:
        text = (Path(dest) / 'xcx.ini').read_text(encoding='utf-8-sig', errors='replace')
    except OSError:
        return None
    for line in text.splitlines():
        line = line.strip()
        if not line or line[0] in '#;[' or '=' not in line:
            continue
        k, v = (part.strip() for part in line.split('=', 1))
        k = k.lower()
        if k.startswith('xcx_'):
            k = k[4:]
        if k == key:
            value = v[1:-1] if len(v) >= 2 and v[0] == v[-1] == '"' else v
    return value


def shader_cache(dest):
    """The shader cache folder the game uses (gfx_shader.c cache_folder): shader_cache in xcx.ini when it fits the
    game's path buffer, a relative path from the game's folder, else dest/shader-cache. XCX_SHADER_CACHE in the
    environment wins over the file, as in the game (runtime/cfg.c), when the setup runs with the game's environment."""
    dest = Path(dest)
    # set but empty in the environment: the game's default folder, as cfg_get hands the game the empty value
    value = os.environ['XCX_SHADER_CACHE'] if 'XCX_SHADER_CACHE' in os.environ else ini_value(dest, 'shader_cache')
    if value and len(value.encode('utf-8')) < PATH_CAP:
        return dest / value
    return dest / DEFAULT_CACHE


def prepared_missing(dest):
    """(variants in shader-prepared.bin, how many of them have no stored inputs in the shader cache's in/ folder):
    every fresh translation stores them as in/<input id>.in (gfx_shader.c in_store). None without a readable list."""
    dest = Path(dest)
    try:
        data = (dest / PREPARED[0]).read_bytes()
    except OSError:
        return None
    if (len(data) < PRP_HEADER or data[:8] != PRP_MAGIC
            or int.from_bytes(data[8:12], 'little') != 1 or int.from_bytes(data[12:16], 'little') != PRP_REC):
        return None
    try:
        have = {n[:-3] for n in os.listdir(shader_cache(dest) / 'in') if n.endswith('.in')}
    except OSError:
        have = set()
    ids = [data[o + 4:o + PRP_REC].hex() for o in range(PRP_HEADER, len(data) - PRP_REC + 1, PRP_REC)]
    return len(ids), sum(1 for i in ids if i not in have)


def prepared_intact(dest):
    """No prepared list, or one whose translations are mostly still in the shader cache. A cache deleted by hand
    leaves the list behind with none of them; a stray missing file is no reason to prepare everything again. The game
    asks the same of a sample (up to 64 records spread over the list, gfx_shader.c prepared_current), as it checks at
    every start."""
    if not (Path(dest) / PREPARED[0]).exists():
        return True
    counts = prepared_missing(dest)
    return counts is not None and counts[0] > 0 and counts[1] * 2 < counts[0]


def has_shaders(dest):
    """dest holds prepared or translated shaders (the prepared list, or a cache folder with anything in it), so the
    window's Rebuild has something to rebuild. Never raises."""
    try:
        if not str(dest).strip():
            return False
        folder = Path(str(dest).strip())
        cache = shader_cache(folder)
        return (folder / PREPARED[0]).is_file() or (cache.is_dir() and any(cache.iterdir()))
    except (OSError, ValueError):
        return False


def _same(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def clear_cache(dest, log=None):
    """Deletes the shader cache the game uses, for Rebuild and a new translator. The setup's own folder
    (dest/shader-cache) goes whole; a folder xcx.ini names, or the default one when it is a link or junction to
    another drive, loses only the cache's own files (OWN_FILE, in/OWN_INPUT;
    as the game's menu clear does), and never when it is the install folder, a folder above it or a drive root.
    Read-only files go too. Returns the folder handled, or None when it was left alone."""
    dest = Path(dest)
    folder = shader_cache(dest)
    linked = folder.is_symlink() or getattr(folder, 'is_junction', lambda: False)()   # moved to another drive
    if _same(folder, dest / DEFAULT_CACHE) and not linked:
        if folder.is_dir():
            shutil.rmtree(folder, onerror=_writable_and_retry)
        elif folder.exists():
            folder.unlink()
        return folder
    here = os.path.abspath(folder)
    if (_same(here, dest) or os.path.dirname(here) == here
            or os.path.normcase(os.path.abspath(dest)).startswith(os.path.normcase(here.rstrip('\\/')) + os.sep)):
        if log:
            log(f'shader cache {folder} is the install folder, a folder above it or a drive root: left alone')
        return None
    for sub, own in ((folder / 'in', OWN_INPUT), (folder, OWN_FILE)):
        try:
            names = os.listdir(sub)
        except OSError:
            continue
        for name in names:
            if own.fullmatch(name):
                p = sub / name
                try:
                    p.unlink()
                except PermissionError:
                    os.chmod(p, 0o666)
                    p.unlink()
        if sub.is_symlink() or getattr(sub, 'is_junction', lambda: False)():
            continue   # a link or junction stays (rmdir would remove the link itself, empty target or not)
        try:
            sub.rmdir()   # only when nothing else is left in it
        except OSError:
            pass
    return folder


def _writable_and_retry(func, path, exc):
    if func in (os.path.islink, os.lstat, os.open, os.scandir):   # a link rmtree refuses, or a folder it can't open
        raise exc[1] if isinstance(exc, tuple) else exc
    os.chmod(path, 0o666)
    func(path)
