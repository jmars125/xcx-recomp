"""The mod manager: install, remove, turn on and turn off mods in an install folder's mods/.

    python -m installer.mods --dest DIR list                 installed mods and the library, as JSON
    python -m installer.mods --dest DIR install ID           from the library (github.com/jmars125/xcx-recomp-mods)
    python -m installer.mods --dest DIR install ID --file FILE   a library mod the player downloaded from its page
    python -m installer.mods --dest DIR add FILE [--id ID] [--top FOLDER] [--yes]   a mod archive downloaded by hand
    python -m installer.mods --dest DIR remove|enable|disable ID

Two kinds of mod, as the library's index-3.json lists them (INDEX_FILE):
  lua    mods/<id>/ with mod.ini and main.lua, run by runtime/mods.c. On when listed in mods/enabled.txt; installed
         ones are also listed in mods/catalog.txt so the game's Mods page shows them while off. Read at start-up.
  romfs  mods/<id>/romfs/<game path>, files in place of the game's own (runtime/romfs_mods.c). On when listed in
         mods/romfs-enabled.txt, which the game reads again twice a second, so they switch while it runs. Earlier
         lines win when two mods have the same file; a mod turned on goes last, at the lowest priority (as in the
         game's Mods tab), and is moved up there if it should win.
A game-file mod comes from its author's page as a zip or RAR (installer/archive.py), ideally in the Switch layout:
romfs/<path>, or romfs/mod/<path> for mods made for the XCXDE mod loader, possibly inside top folders
(atmosphere/contents/<title id>/romfs). exefs/ (Switch code) is not used. An archive without a romfs folder is read
by installer/mod_layout.py, which finds the game's folders in it by the install's own game files and says what it
will install; the window shows that before anything is written. A library download must match the library's size and
SHA-256 before anything is unpacked. Some sites allow downloads only from their own page ("download": "manual" in
the index): the player downloads the file there and picks it, and the same size and SHA-256 check applies. A mod
unpacked into mods/ by hand in any of those layouts is moved to mods/<id>/romfs the next time the mods are listed
(normalise).

A library entry may come in parts ("parts" in the index): a full version and optional pieces, each its own file, as
mods on Nexus Mods often do. Each part the player installs is a mod of its own, <entry id>.<part id>, so it switches
on and off by itself and the game's mods combine like any others; its library.ini names the entry (group) and the
part, and a part that already contains others lists them (includes), so the window can warn when both are on. Files
the player picks are matched to parts by SHA-256, whatever their names or order.

mods/library.json records what this manager installed (kind, version, name) so updates and removals know the kind.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

from . import archive, mod_layout
from .game_paths import GamePaths

LIBRARY = 'https://raw.githubusercontent.com/jmars125/xcx-recomp-mods/main/'
# The library publishes two lists. index.json (format 2) holds only what every released manager installs: Lua mods and
# game-file mods downloaded as one zip; releases up to 0.1.21 and the game's own library list (runtime/mod_library.c)
# read it. index-3.json (format 3) holds every entry, also those downloaded from their own page ("download":
# "manual"), in parts, or as RAR ("archive": "rar"); this manager reads it. An index given by --mods-index may be
# either format.
INDEX_FILE = 'index-3.json'
INDEX_FALLBACK = 'index.json'    # read when the library has no index-3.json yet (it lists Lua and zip entries only)
INDEX_FORMAT = 3
INDEX_FORMATS = (2, 3)
INDEX_MAX_ENTRIES = 5000
AGENT = 'XCX-Setup-mods'
UNPACK_LIMIT = 16 << 30          # bytes a game-file mod may unpack to
LUA_FILES = {'mod.ini', 'main.lua', 'info.ini', 'README.md', 'LICENSE', 'LICENSE.txt'}
CHUNK = 1 << 20


class ModError(Exception):
    """A failure to show the player; the text is English, the window maps the id to its catalog."""

    def __init__(self, id, **params):
        super().__init__(id)
        self.id, self.params = id, params


def valid_id(mid):
    """A mod id: a lowercase letter, then letters, digits and . _ -, at most 64, and no dot at the end (Windows drops
    it, so 'a.' would be the folder of mod 'a'). runtime/mod_library.c, valid_id, is the same."""
    return (isinstance(mid, str) and 0 < len(mid) <= 64 and 'a' <= mid[0] <= 'z' and not mid.endswith('.')
            and all('a' <= ch <= 'z' or '0' <= ch <= '9' or ch in '._-' for ch in mid))


def slug(text):
    """A mod id made from a folder or file name: ASCII letters and digits lowercased, anything else a '-', 'mod-' in
    front when it does not start with a letter, at most 64 characters. runtime/mod_library.c, slug, is the same."""
    s = ''.join(ch if ch.isascii() and (ch.isalnum() or ch in '._') else '-' for ch in text.lower())
    while '--' in s:
        s = s.replace('--', '-')
    s = s.strip('-._')
    s = s if s and 'a' <= s[0] <= 'z' else f'mod-{s}'.rstrip('-')
    return s[:64].rstrip('-._')


# ---- files ---------------------------------------------------------------------------------------------------------

def mods_dir(dest):
    return Path(dest) / 'mods'


def read_list(path):
    try:
        text = path.read_text(encoding='utf-8')
    except FileNotFoundError:
        return []
    return [line.strip() for line in text.splitlines() if line.strip() and not line.startswith('#')]


def write_text(path, text):
    """Writes through a temp file and a rename, so the game never reads half a list. The game may hold the file open
    for a moment while it reads it; the rename is tried again then."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.new')
    tmp.write_text(text, encoding='utf-8', newline='\n')
    for attempt in range(20):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if attempt == 19:
                raise
            time.sleep(0.05)


def write_list(path, ids):
    write_text(path, ''.join(i + '\n' for i in ids))


def load_record(root):
    try:
        data = json.loads((root / 'library.json').read_text(encoding='utf-8'))
        return data if isinstance(data, dict) else {}
    except (FileNotFoundError, ValueError):
        return {}


def save_record(root, record):
    write_text(root / 'library.json', json.dumps(record, indent=1, sort_keys=True) + '\n')


def kind_of(root, mid, record=None):
    rec = (record or load_record(root)).get(mid)
    if rec:
        return rec.get('kind')
    folder = root / mid
    if (folder / 'romfs').is_dir():
        return 'romfs'
    if (folder / 'mod.ini').is_file():
        return 'lua'
    return None


LISTS = {'romfs': 'romfs-enabled.txt', 'lua': 'enabled.txt'}


# ---- mods put in mods/ by hand ---------------------------------------------------------------------------------------
# A player unpacks a mod's download into mods/ as it comes: a folder named after the mod ("Unburger Patch"), the
# Switch SD card layout (atmosphere/contents/<title id>/romfs, <title id>/romfs, contents/<title id>/romfs) or a bare
# romfs/. normalise moves each such folder to mods/<id>/romfs, the one layout the game reads, and names every folder
# it leaves alone with the reason. runtime/mod_library.c, normalise, does the same when the F1 Mods tab reads the
# folder; the two follow the same rules, so either one's result looks the same to the other.

TITLE_ID = mod_layout.TITLE_ID   # Xenoblade Chronicles X: Definitive Edition
SEARCH_DEPTH = 4                  # folders below the top one in which a romfs folder is looked for
WRAPPERS = {'romfs', 'atmosphere', 'contents'}   # top folder names that say nothing about the mod


is_title_id = mod_layout.is_title_id


def find_romfs(folder, depth=SEARCH_DEPTH):
    """The romfs folders below folder (not inside one another), nearest first; at most two are looked for."""
    found, level = [], [folder]
    for _ in range(depth):
        nxt = []
        for d in level:
            try:
                subs = sorted((p for p in d.iterdir() if p.is_dir() and not p.is_symlink()), key=lambda p: p.name)
            except OSError:
                continue
            for p in subs:
                (found if p.name.lower() == 'romfs' else nxt).append(p)
        if found or not nxt:
            break
        level = nxt
    return found


def other_game(rel_parts):
    """The title id of another game in a path below mods/, else None."""
    return next((p for p in rel_parts if is_title_id(p) and p.lower() != TITLE_ID), None)


def free_id(root, base, folder=None):
    """base, or base-2, base-3 ...: the first name not taken in root. folder itself does not count, so on a file
    system that ignores case "Uncensor" becomes "uncensor"."""
    def taken(mid):
        p = root / mid
        if folder is not None and p.exists() and os.path.samefile(p, folder):
            return False
        return p.exists() or p.is_symlink()
    mid, n = base, 2
    while taken(mid):
        suffix = f'-{n}'
        mid, n = base[:64 - len(suffix)] + suffix, n + 1
    return mid


def is_loader(romfs):
    """Whether romfs holds just a folder named mod: the XCXDE mod loader's layout, romfs/mod/<game path>."""
    try:
        entries = list(romfs.iterdir())
    except OSError:
        return False
    return len(entries) == 1 and entries[0].is_dir() and entries[0].name.lower() == 'mod'


# The places of a mod folder's Atmosphere cheat files, as runtime/cheats.c (load_mod) and tools/cheats.py look for them.
CHEAT_PLACES = ('cheats', 'atmosphere/contents/0100453019AA8000/cheats', 'contents/0100453019AA8000/cheats',
                '0100453019AA8000/cheats')


def has_cheats(folder):
    """Whether a folder of the mods folder holds cheat files: the game reads it for the Cheats tab, not as a mod."""
    return any((folder / where).is_dir() for where in CHEAT_PLACES)


def normalise(root):
    """Moves the mods put in root by hand to root/<id>/romfs. Returns [(catalog id, params)]: a 'mods.moved' line for
    each folder moved, a 'mods.skip_*' line for each one left alone."""
    notes = []
    try:
        names = sorted(p.name for p in root.iterdir())
    except OSError:
        return notes
    for name in names:
        top = root / name
        if name.startswith('.') or top.is_symlink():
            continue
        if top.is_file():
            if name.lower().endswith('.zip'):
                notes.append(('mods.skip_zip', {'folder': name}))
            continue
        if not top.is_dir():
            continue
        if valid_id(name) and (top / 'mod.ini').is_file():
            continue
        if (top / 'mod.ini').is_file():
            notes.append(('mods.skip_lua_name', {'folder': name}))
            continue
        if name.lower() == 'romfs':
            romfs = top
        else:
            found = find_romfs(top)
            if not found:
                if not has_cheats(top):   # a folder of cheats only is the Cheats tab's (runtime/cheats.c)
                    notes.append(('mods.skip_no_romfs', {'folder': name}))
                continue
            if len(found) > 1:
                notes.append(('mods.skip_two_romfs', {'folder': name}))
                continue
            romfs = found[0]
        rel = romfs.relative_to(root).parts
        title = other_game(rel)
        if title:
            notes.append(('mods.skip_other_game', {'folder': name, 'title': title}))
            continue
        if valid_id(name) and rel == (name, 'romfs') and not is_loader(romfs):
            continue   # already mods/<id>/romfs
        generic = name.lower() in WRAPPERS or is_title_id(name)
        mid = free_id(root, 'romfs-mod') if generic else name if valid_id(name) else free_id(root, slug(name), top)
        final = root / mid
        try:
            if name.lower() == 'romfs':          # the folder is the mod's romfs
                final.mkdir()
                os.replace(top, final / 'romfs')
            elif generic:                         # atmosphere/..., contents/..., <title id>/...: kept whole inside
                final.mkdir()
                os.replace(top, final / name)
                os.replace(final.joinpath(*rel), final / 'romfs')
            else:                                 # a folder named after the mod
                if mid != name:
                    os.replace(top, final)
                if rel[1:] != ('romfs',):
                    os.replace(final.joinpath(*rel[1:]), final / 'romfs')
            if is_loader(final / 'romfs'):
                tmp = final / '.romfs-loader'
                os.replace(final / 'romfs', tmp)
                os.replace(next(tmp.iterdir()), final / 'romfs')
                tmp.rmdir()
            if not (final / 'library.ini').exists():
                write_info(final, 'romfs', mid if generic or name.lower() == 'romfs' else name, source='folder')
        except OSError as e:
            notes.append(('mods.skip_move_failed', {'folder': name, 'detail': str(e)}))
            continue
        notes.append(('mods.moved', {'folder': name, 'id': mid}))
    return notes


def installed(dest, notes=None):
    """[{id, kind, version, name, enabled}] for every mod folder, the manager's or one put there by hand. Folders put
    there in another layout are moved first (normalise); notes, a list, gets its lines."""
    root = mods_dir(dest)
    if not root.is_dir():
        return []
    found = normalise(root)
    if notes is not None:
        notes.extend(found)
    record = load_record(root)
    on = {k: set(read_list(root / f)) for k, f in LISTS.items()}
    out = []
    for folder in sorted(p for p in root.iterdir() if p.is_dir() and valid_id(p.name)):
        kind = kind_of(root, folder.name, record)
        if not kind:
            continue
        rec = dict(record.get(folder.name, {}), **read_info(folder))   # library.ini is the newer word
        out.append({'id': folder.name, 'kind': kind, 'version': rec.get('version', ''),
                    'name': rec.get('name') or folder.name, 'enabled': folder.name in on.get(kind, ())})
    return out


# ---- library -------------------------------------------------------------------------------------------------------

def opener(url):
    """An https URL opened; a redirect to anything but https is refused before anything is read from it."""
    if not url.startswith('https://'):
        raise ModError('mods.bad_link', url=url)
    r = urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': AGENT}), timeout=60)
    if not r.geturl().startswith('https://'):
        r.close()
        raise ModError('mods.bad_link', url=r.geturl())
    return r


def fetch_index(source=None):
    """The library's entries. source: an https URL or a local index file (--mods-index, tests); default the library
    on GitHub. An http:// or other link is refused (a plain link could be changed on the way)."""
    if source and '://' in source and not source.startswith('https://'):
        raise ModError('mods.index_https', url=source)
    def read(url):
        with opener(url) as r:
            return json.loads(r.read(8 << 20).decode('utf-8'))
    try:
        if source and not source.startswith('https://'):
            data = json.loads(Path(source).read_text(encoding='utf-8'))
        elif source:
            data = read(source)
        else:
            try:
                data = read(LIBRARY + INDEX_FILE)
            except urllib.error.HTTPError as e:
                if e.code != 404:
                    raise
                data = read(LIBRARY + INDEX_FALLBACK)   # a library from before index-3.json
    except (OSError, ValueError) as e:
        raise ModError('mods.index_failed', detail=str(e)) from e
    if (not isinstance(data, dict) or data.get('format') not in INDEX_FORMATS or not isinstance(data.get('mods'), list)
            or len(data['mods']) > INDEX_MAX_ENTRIES):
        raise ModError('mods.index_format')
    out, taken = [], set()
    for m in data['mods']:
        if not (isinstance(m, dict) and valid_id(m.get('id')) and m.get('kind') in LISTS and usable_parts(m)):
            continue
        ids = {m['id']} | ({p['mid'] for p in parts_of(m)} if m['kind'] == 'romfs' else set())
        if ids & taken:   # an id an earlier entry or its parts have: one mod folder can't be two entries
            continue
        taken |= ids
        out.append(m)
    return out


PART_ID = re.compile(r'[a-z][a-z0-9-]{0,31}\Z')   # the library's rule for part ids (xcx-recomp-mods, its tools)
SHA256 = re.compile(r'[0-9a-fA-F]{64}\Z')


def usable_parts(entry):
    """Whether every part of an entry names a file the manager can get: a manual part its page, a downloaded one its
    url, each with a name, SHA-256 and size of the right types; part ids follow the library's rule and make valid mod
    ids. An entry whose "parts" is not a non-empty list, or whose "download" is anything but "manual", is refused, as
    the game's library list refuses them. Lua entries and plain ones pass as before when their size is a number."""
    if entry.get('kind') != 'romfs':
        return True
    size_ok = lambda v: type(v) is int and v > 0
    if entry.get('download') not in (None, 'manual') or 'size' in entry and not size_ok(entry['size']):
        return False
    if 'parts' not in entry and not is_manual(entry):
        return True
    raw = entry.get('parts')
    if raw is not None and not (isinstance(raw, list) and raw and all(isinstance(p, dict) for p in raw)):
        return False
    if raw is not None and not all(isinstance(p.get('id'), str) and PART_ID.match(p['id']) for p in raw):
        return False
    parts = parts_of(entry)
    ok = lambda p: (str(p.get('page' if is_manual(entry) else 'url') or '').startswith('https://')
                    and isinstance(p.get('name'), str) and p['name'].strip() and isinstance(p.get('sha256'), str)
                    and SHA256.match(p['sha256']) and size_ok(p.get('size')) and valid_id(p['mid'])
                    and isinstance(p.get('_includes'), list) and all(isinstance(i, str) for i in p['_includes']))
    return all(ok(p) for p in parts) and len({p['mid'] for p in parts}) == len(parts)


def parts_of(entry):
    """The parts of a library entry as dicts with 'mid', the mod id each installs as. An entry without "parts" is one
    part whose mod id is the entry's own."""
    low = lambda h: h.lower() if isinstance(h, str) else h   # the hash as hashlib writes it
    if entry.get('parts'):
        out = []
        for p in entry['parts']:
            if isinstance(p, dict):
                inc = p.get('includes', [])
                out.append(dict(p, mid=f"{entry['id']}.{p.get('id', '')}", sha256=low(p.get('sha256')),
                                includes=list(inc) if isinstance(inc, list) else [], _includes=inc))
        return out
    return [{'id': '', 'mid': entry['id'], 'name': entry.get('name', entry['id']), 'description': '',
             'file': entry.get('file'), 'page': entry.get('page'), 'url': entry.get('url'),
             'sha256': low(entry.get('sha256')), 'size': entry.get('size'), 'includes': [], '_includes': []}]


def is_manual(entry):
    """Whether a library entry is downloaded by the player from its page ("download": "manual"), not by the manager:
    a site that allows downloads only from its own page, such as Nexus Mods."""
    return entry.get('download') == 'manual'


def file_hash(path, sizes=None):
    """(size, SHA-256) of the file at path; the hash is only read when the size is one of sizes (None: always)."""
    path = Path(path)
    try:
        size = path.stat().st_size
        if sizes is not None and size not in sizes:
            return size, None
        h = hashlib.sha256()
        with open(path, 'rb') as f:
            for block in iter(lambda: f.read(CHUNK), b''):
                h.update(block)
    except OSError as e:
        raise ModError('mods.failed', detail=str(e)) from e
    return size, h.hexdigest()


def copy_hashed(src, out, cap):
    """Copies src to out in one read, at most cap + 1 bytes (a file that grew past the size it was matched by is
    refused by the size this returns, without copying all of it); (size, SHA-256) of what was written."""
    h, size = hashlib.sha256(), 0
    with open(src, 'rb') as f, open(out, 'wb') as o:
        while size <= cap:
            block = f.read(min(CHUNK, cap + 1 - size))
            if not block:
                break
            h.update(block)
            o.write(block)
            size += len(block)
    return size, h.hexdigest()


def match_files(entry, paths):
    """Which part of a manual entry each picked file is, by size and SHA-256: ({mod id: (part, path)}, [paths that
    are no part of it]). Two picks of one part keep the first."""
    parts = parts_of(entry)
    sizes = {int(p['size']) for p in parts}
    matched, unknown = {}, []
    for path in paths:
        size, digest = file_hash(path, sizes)
        part = next((p for p in parts if int(p['size']) == size and p['sha256'] == digest), None)
        if part is None:
            unknown.append(Path(path))
        else:
            matched.setdefault(part['mid'], (part, Path(path)))
    return matched, unknown


def mismatch(path, entry):
    """The ModError('mods.file_mismatch') for a picked file that is none of a manual entry's files: it names the
    files the entry lists. Reads no file (the picks were matched already)."""
    parts = parts_of(entry)
    return ModError('mods.file_mismatch', picked=Path(path).name,
                    file=' / '.join(p.get('file') or p['name'] for p in parts))


def download(url, size, sha256, out, progress=None):
    """Downloads url to out, which must come to exactly size bytes with the given SHA-256."""
    h, got = hashlib.sha256(), 0
    with opener(url) as r, open(out, 'wb') as f:
        while True:
            block = r.read(CHUNK)
            if not block:
                break
            got += len(block)
            if got > size:
                raise ModError('mods.download_mismatch', url=url)
            h.update(block)
            f.write(block)
            if progress:
                progress(got, size)
    if got != size or h.hexdigest() != sha256:
        raise ModError('mods.download_mismatch', url=url)


# ---- unpacking -----------------------------------------------------------------------------------------------------

def setup_layout():
    """The layout of the setup this runs in, for xcx-unpack (installer/archive.py)."""
    from . import layout
    return layout.find()


def preview(dest, path, top=None, layout=None):
    """The mod_layout.Plan for the archive at path: which of its files go where, read against the install's game
    files. Raises ModError; 'layout.several' carries the choices for top."""
    try:
        if archive.kind_of(path) == 'rar' and layout is None:
            layout = setup_layout()
        members = archive.members(path, layout)
        p = mod_layout.plan(members, GamePaths.of_install(dest), top)
    except (archive.ArchiveError, mod_layout.LayoutError) as e:
        raise ModError(e.id, **e.params) from e
    if sum(m.size for m in members if m.name in p.files) > UNPACK_LIMIT:
        raise ModError('mods.zip_too_big')
    return p


def unpack_romfs(path, folder, dest=None, plan=None, layout=None):
    """Unpacks an archive's game files into folder/romfs/ by plan (default: preview's against dest); folder exists
    and holds no romfs/ yet. Returns the number of files."""
    p = plan or preview(dest or folder, path, layout=layout)
    try:
        if archive.kind_of(path) == 'rar' and layout is None:
            layout = setup_layout()
        return archive.extract(path, layout, p.files, Path(folder) / 'romfs', UNPACK_LIMIT)
    except archive.ArchiveError as e:
        raise ModError(e.id, **e.params) from e


# ---- operations ----------------------------------------------------------------------------------------------------

def write_info(staged, kind, name, author='', version='', source='', group='', part='', includes=(), group_name=''):
    """<id>/library.ini: name, author, version and kind of an installed mod. The game's F1 menu writes the same file
    when it installs a mod (runtime/mod_library.c, write_library_ini), so each manager sees the other's installs.
    A part of a library entry also names the entry (group, and group_name for the game's Mods tab, which lists the
    parts together under it), the part and the parts it contains (includes)."""
    # the game keeps a value only when it fits its field (runtime/mod_library.c, ini_value; ModLibRow: name 96 bytes,
    # author 64, version 32, group 65, group_name 96, with the closing 0), so each is cut to fit in UTF-8 bytes, on a
    # character boundary; a value Cyrillic letters (2 bytes each) make long is cut, not dropped
    room = {'name': 95, 'author': 63, 'version': 31, 'group': 64, 'group_name': 95}

    def clean(key, v):
        v = ' '.join(str(v).split())
        cap = room.get(key, 199)
        return v.encode('utf-8')[:cap].decode('utf-8', 'ignore') if len(v.encode('utf-8')) > cap else v
    rows = [('name', name), ('author', author), ('version', version), ('kind', kind), ('source', source)]
    if group:
        rows += [('group', group), ('group_name', group_name or group), ('part', part), ('includes', ','.join(includes))]
    text = ''.join(f'{k} = {clean(k, v)}\n' for k, v in rows)
    (staged / 'library.ini').write_text(text, encoding='utf-8', newline='\n')


def read_info(folder):
    """The keys of <id>/library.ini; {} when there is none."""
    try:
        text = (folder / 'library.ini').read_text(encoding='utf-8')
    except (OSError, UnicodeDecodeError):
        return {}
    out = {}
    for line in text.splitlines():
        key, eq, value = line.partition('=')
        if eq:
            out[key.strip()] = value.strip()
    return out


def place(root, mid, staged):
    """Moves a staged mod folder to mods/<id>, in place of an older copy."""
    final = root / mid
    old = root / f'.old-{mid}'
    if old.exists():
        shutil.rmtree(old)
    if final.exists():
        os.replace(final, old)
    os.replace(staged, final)
    if old.exists():
        shutil.rmtree(old, ignore_errors=True)


def part_name(entry, part):
    """What the player sees for an installed part: 'Tweaks mod: Probe tweak'; the entry's name for a whole entry."""
    name = entry.get('name', entry['id'])
    return f"{name}: {part['name']}" if part.get('id') else name


def install(dest, entry, progress=None, file=None, layout=None, files=None, parts=None, plans=None, matched=None):
    """Installs or updates a library entry. A mod that was on stays on; a new one starts off.
    A manual entry (is_manual) installs from the player's downloads: file, or files, each matched to a part by
    SHA-256 (ModError 'mods.file_mismatch' when none is a part). Other entries download: every part, or the part
    ids in parts. plans: {mod id: mod_layout.Plan} read before (the window shows them first). matched: what
    match_files gave for those files already (the window matched them to show the plans), so they aren't hashed
    again here; install_part checks its copy of each anyway.
    Returns {'installed': [mod ids], 'unknown': [picked paths that are no part of the entry]}."""
    mid, kind = entry['id'], entry['kind']
    root = mods_dir(dest)
    root.mkdir(parents=True, exist_ok=True)
    if kind != 'romfs':
        install_lua(dest, entry, progress)
        return {'installed': [mid], 'unknown': []}
    unknown = []
    if is_manual(entry):
        picked = list(files or ([file] if file else []))
        if not picked and not matched:
            raise ModError('mods.manual_needs_file', name=entry.get('name', mid))
        if matched is None:
            matched, unknown = match_files(entry, picked)
        if not matched:
            raise mismatch(picked[0], entry)
        todo = [(part, path) for part, path in matched.values()]
    else:
        todo = [(p, None) for p in parts_of(entry) if parts is None or p['id'] in parts]
    done = []
    for part, path in todo:
        install_part(dest, entry, part, path, progress, layout, (plans or {}).get(part['mid']))
        done.append(part['mid'])
    return {'installed': done, 'unknown': unknown}


def install_part(dest, entry, part, path=None, progress=None, layout=None, plan=None):
    """Installs one part of a romfs entry as mods/<part mod id>: from path (the player's download), else downloaded
    from the part's url."""
    root = mods_dir(dest)
    mid = part['mid']
    with tempfile.TemporaryDirectory(dir=root, prefix='.install-') as tmp:
        staged = Path(tmp) / mid
        staged.mkdir()
        copy = Path(tmp) / 'mod.download'
        if path is None:
            download(part['url'], int(part['size']), part['sha256'], copy, progress)
        else:
            # the player's file, copied first and checked again: what is unpacked is what was checked, even if the
            # file in Downloads changes meanwhile; its size first, so a file replaced by a big other one isn't copied
            try:
                if os.stat(path).st_size != int(part['size']):
                    raise ModError('mods.file_changed', picked=Path(path).name)
                got = copy_hashed(path, copy, int(part['size']))   # hashed as it is copied: no second read
            except OSError as e:
                raise ModError('mods.failed', detail=str(e)) from e
            if got != (int(part['size']), part['sha256']):
                raise ModError('mods.file_changed', picked=Path(path).name)
        path = copy
        unpack_romfs(path, staged, dest, plan, layout)
        name = part_name(entry, part)
        source = entry.get('homepage') or 'library'
        write_info(staged, 'romfs', name, entry.get('author', ''), entry.get('version', ''), source,
                   entry['id'] if part.get('id') else '', part.get('id', ''), part.get('includes', ()),
                   entry.get('name') or entry['id'])
        place(root, mid, staged)
    record = load_record(root)
    record[mid] = {'kind': 'romfs', 'version': entry.get('version', ''), 'name': name, 'source': source}
    if part.get('id'):
        record[mid].update(group=entry['id'], part=part['id'])
    save_record(root, record)


def install_lua(dest, entry, progress=None):
    """Installs or updates a Lua library entry: its files, each checked against the index."""
    mid, kind = entry['id'], entry['kind']
    root = mods_dir(dest)
    with tempfile.TemporaryDirectory(dir=root, prefix='.install-') as tmp:
        staged = Path(tmp) / mid
        staged.mkdir()
        files = entry.get('files') or []
        if not files or any(f.get('name') not in LUA_FILES for f in files):
            raise ModError('mods.index_format')
        for i, f in enumerate(files):
            download(LIBRARY + entry['path'] + '/' + f['name'], int(f['size']), f['sha256'], staged / f['name'])
            if progress:
                progress(i + 1, len(files))
        write_info(staged, kind, entry.get('name', mid), entry.get('author', ''), entry.get('version', ''),
                   entry.get('homepage') or 'library')
        place(root, mid, staged)
    record = load_record(root)
    record[mid] = {'kind': kind, 'version': entry.get('version', ''), 'name': entry.get('name', mid),
                   'source': entry.get('homepage') or 'library'}
    save_record(root, record)
    catalog = read_list(root / 'catalog.txt')
    if mid not in catalog:
        write_list(root / 'catalog.txt', catalog + [mid])


def archive_name(path):
    """The mod's name from an archive's file name. A Nexus Mods download is named <file>-<mod id>-<version parts>-
    <upload time>; that tail is dropped ('all tweaks mod-10-1-0-1745514276.rar' -> 'all tweaks mod')."""
    stem = Path(path).stem
    parts = stem.split('-')
    if len(parts) >= 4 and len(parts[-1]) >= 9 and all(p.isdigit() for p in parts[-3:]):
        head = parts[:-1]
        while len(head) > 1 and head[-1].isdigit():
            head = head[:-1]
        stem = '-'.join(head)
    return stem


def add_archive(dest, path, mid=None, name=None, plan=None, layout=None):
    """Installs a game-file mod archive (zip or RAR) the player downloaded, by plan (default: preview's); the id
    and name come from the file name when not given."""
    path = Path(path)
    shown = name or archive_name(path)
    if not mid:
        mid = slug(shown)
    if not valid_id(mid):
        raise ModError('mods.bad_id', id=mid)
    root = mods_dir(dest)
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=root, prefix='.install-') as tmp:
        staged = Path(tmp) / mid
        staged.mkdir()
        unpack_romfs(path, staged, dest, plan, layout)
        source = archive.kind_of(path) or 'archive'   # 'zip' or 'rar'
        write_info(staged, 'romfs', shown, source=source)
        place(root, mid, staged)
    record = load_record(root)
    record[mid] = {'kind': 'romfs', 'version': '', 'name': shown, 'source': source}
    save_record(root, record)
    return mid




def set_enabled(dest, mid, on):
    root = mods_dir(dest)
    kind = kind_of(root, mid)
    if not kind or not (root / mid).is_dir():
        raise ModError('mods.not_installed', id=mid)
    path = root / LISTS[kind]
    # read once: the game's Mods tab writes the same list, and the later of two writers wins (each writes it whole)
    listed = read_list(path)
    ids = [i for i in listed if i != mid]
    if not on:
        write_list(path, ids)
    elif mid not in listed:   # a game-file mod goes last, a Lua mod first; one already on keeps its place
        write_list(path, ids + [mid] if kind == 'romfs' else [mid] + ids)


def remove(dest, mid):
    root = mods_dir(dest)
    if not valid_id(mid) or not (root / mid).is_dir():
        raise ModError('mods.not_installed', id=mid)
    kind = kind_of(root, mid)
    if kind:
        set_enabled(dest, mid, False)
    catalog = read_list(root / 'catalog.txt')
    if mid in catalog:
        write_list(root / 'catalog.txt', [i for i in catalog if i != mid])
    record = load_record(root)
    if record.pop(mid, None) is not None:
        save_record(root, record)
    shutil.rmtree(root / mid)


def status(dest, index, notes=None):
    """The library entries joined with what is installed: [{...entry, installed, enabled, update}] plus mods that are
    installed but not in the library. notes, a list, gets the lines of normalise."""
    have = {m['id']: m for m in installed(dest, notes)}
    out = []
    for e in index:
        rows = []
        for p in parts_of(e):
            m = have.pop(p['mid'], None)
            rows.append(dict(p, installed=bool(m), enabled=bool(m and m['enabled']),
                             update=bool(m and m['version'] != e.get('version', ''))))
        row = dict(e, installed=any(r['installed'] for r in rows), enabled=any(r['enabled'] for r in rows),
                   update=any(r['update'] for r in rows))
        if e.get('parts'):
            row['parts'] = rows
        out.append(row)
    for m in have.values():
        out.append(dict(m, installed=True, update=False, description='', author='', homepage='', notes=''))
    return out


def overlaps(entry, mid):
    """The names of the entry's parts that are on and overlap the part mid: one contains the other (includes). Both
    on change the same game data twice; the window asks first."""
    parts = {p['mid']: p for p in entry.get('parts') or []}
    me = parts.get(mid)
    if not me:
        return []
    out = []
    for p in parts.values():
        if p['mid'] != mid and p.get('enabled') and (p['id'] in me.get('includes', ())
                                                     or me['id'] in p.get('includes', ())):
            out.append(p['name'])
    return out


def english_catalog():
    return json.loads((Path(__file__).resolve().parent / 'text' / 'en.json').read_text(encoding='utf-8'))


def main(argv=None):
    ap = argparse.ArgumentParser(prog='python -m installer.mods', description=__doc__.splitlines()[0])
    ap.add_argument('--dest', type=Path, required=True, help='install folder (the one with xcx.exe)')
    ap.add_argument('--index', help='library index (format 2 or 3), an https URL or a file '
                                    '(default: the library on GitHub)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('list')
    for c in ('install', 'remove', 'enable', 'disable'):
        sub.add_parser(c).add_argument('id')
    sub.choices['install'].add_argument('--file', type=Path, action='append',
                                        help='a download of a manual library entry (once per file)')
    sub.choices['install'].add_argument('--part', action='append', help='a part to download (default: all)')
    for c in ('add', 'add-zip'):
        z = sub.add_parser(c)
        z.add_argument('zip', type=Path)
        z.add_argument('--id')
        z.add_argument('--top', help='the folder of the archive that holds the game folders (after layout.several)')
        z.add_argument('--yes', action='store_true', help='install even when the layout needs a look')
    args = ap.parse_args(argv)
    try:
        if args.cmd == 'list':
            notes = []
            print(json.dumps(status(args.dest, fetch_index(args.index), notes), indent=1))
            for note, params in notes:
                print(f'{note} {json.dumps(params)}', file=sys.stderr)
        elif args.cmd == 'install':
            entry = next((e for e in fetch_index(args.index) if e['id'] == args.id), None)
            if not entry:
                raise ModError('mods.not_in_library', id=args.id)
            result = install(args.dest, entry, files=args.file, parts=args.part)
            print('installed ' + ', '.join(result['installed']))
            for path in result['unknown']:
                print(f'not a file of {args.id}: {path}', file=sys.stderr)
        elif args.cmd in ('add', 'add-zip'):
            p = preview(args.dest, args.zip, args.top)
            english = english_catalog()
            for line in mod_layout.describe(p, lambda i, **k: english.get(i, i).format(**k)):
                print(line)
            if mod_layout.needs_confirm(p) and not args.yes:
                print('not installed: check the lines above, then run again with --yes', file=sys.stderr)
                return 1
            print(f'installed {add_archive(args.dest, args.zip, args.id, plan=p)}')
        elif args.cmd == 'remove':
            remove(args.dest, args.id)
            print(f'removed {args.id}')
        else:
            set_enabled(args.dest, args.id, args.cmd == 'enable')
            print(f'{args.cmd}d {args.id}')
    except ModError as e:
        print(f'{e.id} {json.dumps(e.params)}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
