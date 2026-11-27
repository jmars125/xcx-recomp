"""Works out which files of a mod archive are game files and where they go, and says how sure it is.

A game-file mod should hold romfs/<game path> (or romfs/mod/<game path>, the XCXDE mod loader's layout), maybe inside
atmosphere/contents/<title id>/. Many do not: the files sit in a folder named after the mod ("tweaks mod/bdat/..."),
or loose at the top ("bdat/common.bdat"). plan() reads such an archive the way a person would:

  1. A romfs folder: its contents are the game files, as before (installer/mods.py, romfs_members). Sure.
  2. No romfs folder, and the install's game files are known (installer/game_paths.py): try every folder of the
     archive as the top of the game's folders and keep the one under which the most files are files of the game. A
     bdat table as JSON, bdat/<file>/<Table>.json, counts when bdat/<file>.bdat is a game file: the game merges such
     tables into that file at start-up (runtime bdat merge), so a mod of only tables is placed as surely.
     Sure when at least one file matched. Two folders side by side that match as well as each other are two
     versions of the mod in one archive: the player picks one (LayoutError 'layout.several' with the choices, then
     plan(..., top=choice)).
  3. Otherwise: the top is the first folder that holds a folder the game is known to have (KNOWN_TOP), below any
     folders that hold nothing else. Not sure: the player sees the result and confirms it.

Files that are not game files are left out and named: exefs/ (Switch code, which XCX Recomp does not run), read-me
files, pictures, archives inside the archive. Files under the chosen top that the game does not have are installed
(a mod may add files) but named, since a misplaced file is the usual reason a mod does nothing.
"""
from dataclasses import dataclass, field

TITLE_ID = '0100453019aa8000'   # Xenoblade Chronicles X: Definitive Edition (installer/mods.py uses it too)
# Top folders of the game's files that mods are known to replace; enough to recognise a layout when the install's
# game files cannot be read, not a list of the game's folders
KNOWN_TOP = {'bdat', 'chr', 'sound', 'movie'}
DOC_SUFFIXES = {'.txt', '.md', '.pdf', '.rtf', '.doc', '.docx', '.htm', '.html', '.url', '.nfo', '.ini', '.json',
                '.png', '.jpg', '.jpeg', '.gif', '.webp', '.bmp', '.mp4', '.zip', '.rar', '.7z'}
MAP_HEADER = 'map/ma9990.wismhd'   # the game's map header, which a mod's map pieces change
SHOWN = 12   # names a note lists before "and N more"


class LayoutError(Exception):
    def __init__(self, id, **params):
        super().__init__(id)
        self.id, self.params = id, params


@dataclass
class Plan:
    files: dict                 # archive member name -> game path
    how: str                    # 'romfs', 'loader', 'matched' or 'guessed'
    top: str = ''               # the folder of the archive read as the top of the game's folders ('' the archive's)
    matched: int = 0            # files that are files of the game (when the game's files are known)
    added: list = field(default_factory=list)      # game paths the game does not have
    left_out: list = field(default_factory=list)   # (member name, reason): 'exefs', 'doc', 'outside', 'dir_only'

    @property
    def sure(self):
        return self.how in ('romfs', 'loader', 'matched')

    @property
    def tables(self):
        """bdat tables as JSON (bdat-toolset's format, as some mods ship their changes): installed as they come, used
        once the game's bdat merge reads them, never game files of their own."""
        return sorted(g for g in self.files.values() if is_table_json(g))

    @property
    def map_pieces(self):
        """Map pieces (map/ma9990.pieces/<file>, as tools/map_pieces.py writes them): installed as they come, put
        together with the game's map when the game starts, never game files of their own."""
        return sorted(g for g in self.files.values() if is_map_piece(g))

    @property
    def new_files(self):
        """The added files that are not JSON tables or map pieces."""
        return [g for g in self.added if not is_table_json(g) and not is_map_piece(g)]


def is_table_json(game_path):
    """A bdat table as bdat-toolset JSON: bdat/<file>/<Table>.json, which the game merges into bdat/<file>.bdat."""
    low = game_path.lower()
    return low.startswith('bdat/') and low.endswith('.json') and low.count('/') >= 2


def is_map_piece(game_path):
    """A file of a mod's map pieces, map/ma9990.pieces/<file> (pieces.txt and the chunks it names), which the game puts
    together with its own map/ma9990.* files. The folder's name is matched in any case, as the game finds it."""
    parts = game_path.lower().split('/')
    return len(parts) == 3 and parts[0] == 'map' and parts[1] == 'ma9990.pieces' and parts[2] != ''


def table_target(game_path):
    """The bdat file a JSON table belongs to: 'bdat/us/common_ms/X.json' -> 'bdat/us/common_ms.bdat'."""
    return game_path.rsplit('/', 1)[0] + '.bdat'


def in_game(game_path, game):
    """Whether a path is a game file, a JSON table of one, or a map piece of the game's map."""
    if not game:
        return False
    return (game_path in game or is_table_json(game_path) and table_target(game_path) in game
            or is_map_piece(game_path) and MAP_HEADER in game)


def is_title_id(name):
    return len(name) == 16 and all(ch in '0123456789abcdefABCDEF' for ch in name)


def doc_like(name):
    base = name.rsplit('/', 1)[-1]
    dot = base.rfind('.')
    return dot > 0 and base[dot:].lower() in DOC_SUFFIXES   # PurePosixPath(name).suffix, without building a path


def check_members(members):
    """The member names that are files; raises LayoutError on an unsafe, linked or encrypted entry."""
    files, seen = [], set()
    for m in members:
        if m.name in seen:   # the same name twice, or a\b beside a/b: one would be written over the other
            raise LayoutError('mods.zip_duplicate', name=m.name)
        seen.add(m.name)
        if not m.safe:
            raise LayoutError('mods.zip_unsafe', name=m.name)
        if m.type == 'dir':
            continue
        if m.type != 'file':
            raise LayoutError('mods.zip_unsafe', name=m.name)
        if m.encrypted:
            raise LayoutError('archive.encrypted', detail=m.name)
        files.append(m.name)
    return files


def from_romfs(files):
    """Plan for an archive with a romfs folder, or None when it has none. The game path follows the last 'romfs'
    part; a leading 'mod/' (the XCXDE mod loader's layout) is dropped."""
    picks, left, how = {}, [], 'romfs'
    for name in files:
        parts = name.split('/')
        low = [p.lower() for p in parts]
        if 'romfs' not in low:
            left.append((name, 'exefs' if 'exefs' in low else 'doc' if doc_like(name) else 'outside'))
            continue
        cut = len(low) - low[::-1].index('romfs')
        title = next((p for p in parts[:cut] if is_title_id(p) and p.lower() != TITLE_ID), None)
        if title:
            raise LayoutError('mods.zip_other_game', title=title)
        rest = parts[cut:]
        if len(rest) > 1 and rest[0].lower() == 'mod':
            rest, how = rest[1:], 'loader'
        if rest:
            picks[name] = '/'.join(rest)
    return Plan(picks, how, left_out=left) if picks else None


def under(files, top):
    """{member: game path} for the files below the folder top ('' for all), read-me files at its first level left
    out, and the files that are left out."""
    picks, left = {}, []
    prefix = top + '/' if top else ''
    for name in files:
        if not name.startswith(prefix):
            left.append((name, 'exefs' if 'exefs' in name.lower().split('/') else 'doc' if doc_like(name) else 'outside'))
            continue
        rest = name[len(prefix):]
        if '/' not in rest and doc_like(rest):
            left.append((name, 'doc'))
            continue
        if rest.lower().split('/')[0] == 'exefs':
            left.append((name, 'exefs'))
            continue
        picks[name] = rest
    return picks, left


def top_scores(files, game):
    """{folder: how many files under() would pick below it that are the game's}, for every folder that has any, in
    one pass over the files and their folders (under() for every folder would take files x folders)."""
    score = {}
    for name in files:
        parts = name.split('/')
        for i in range(len(parts)):   # parts[:i] as the top, the rest as the game path, as under() reads it
            rest = parts[i:]
            if len(rest) == 1 and doc_like(rest[0]) or rest[0].lower() == 'exefs':
                continue
            if in_game('/'.join(rest), game):
                top = '/'.join(parts[:i])
                score[top] = score.get(top, 0) + 1
    return score


def known_tops(files):
    """The folders under which under() would pick a file inside a folder the game is known to have (KNOWN_TOP)."""
    out = set()
    for name in files:
        parts = name.split('/')
        for i in range(len(parts) - 1):
            if parts[i].lower() in KNOWN_TOP:
                out.add('/'.join(parts[:i]))
    return out


def plan(members, game=None, top=None):
    """The Plan for an archive's members (archive.Member). game: a GamePaths, or None when unknown. top: the folder
    the player picked as the top of the game's folders (after 'layout.several'), else None."""
    files = check_members(members)
    if not files:
        raise LayoutError('archive.empty')
    if top is not None:
        picks, left = under(files, top)
        if not picks:
            raise LayoutError('mods.zip_no_romfs')
        score = sum(1 for g in picks.values() if in_game(g, game))
        added = sorted(g for g in picks.values() if game and not in_game(g, game))
        return check_duplicates(Plan(picks, 'matched' if score else 'guessed', top, score, added, left))
    p = from_romfs(files)
    if p:
        p.matched = sum(1 for g in p.files.values() if in_game(g, game))
        p.added = sorted(g for g in p.files.values() if game and not in_game(g, game))
        return check_duplicates(p)
    if game:
        scored = top_scores(files, game)
        if scored:
            high = max(scored.values())
            best = sorted((t for t, n in scored.items() if n == high), key=lambda t: (t.count('/'), t))
            rivals = [t for t in best if not any(t.startswith(o + '/') or not o for o in best if o != t)]
            if len(rivals) > 1:
                raise LayoutError('layout.several', tops=rivals, choices=', '.join(rivals))
            top = best[0]
            picks, left = under(files, top)
            added = sorted(g for g in picks.values() if not in_game(g, game))
            return check_duplicates(Plan(picks, 'matched', top, high, added, left))
    # Unknown game files: the first folder (from the top) that holds a known game folder, else the deepest folder
    # reached through folders that hold nothing but one folder and read-me files
    known = sorted(known_tops(files), key=lambda t: (t.count('/'), t))
    if known:
        top = known[0]
        rivals = [t for t in known if t.count('/') == top.count('/')]
        if len(rivals) > 1:
            raise LayoutError('layout.several', tops=rivals, choices=', '.join(rivals))
    else:
        top = ''
        while True:
            picks, _ = under(files, top)
            heads = {g.split('/')[0] for g in picks.values() if '/' in g}
            loose = [g for g in picks.values() if '/' not in g]
            if len(heads) != 1 or loose:
                break
            top = (top + '/' if top else '') + heads.pop()
    picks, left = under(files, top)
    if not picks:
        raise LayoutError('mods.zip_no_romfs')
    return check_duplicates(Plan(picks, 'guessed', top, left_out=left,
                                 added=sorted(picks.values()) if game else []))


def check_duplicates(p):
    seen = {}
    for name, g in p.files.items():
        key = g.casefold()   # closer to how NTFS folds case than lower()
        if key in seen:
            raise LayoutError('mods.zip_duplicate', name=g)
        seen[key] = name
    return p


def describe(p, cat):
    """The lines that tell the player what plan p will do, in the catalog cat's words."""
    def names(items):
        items = list(items)
        shown = ', '.join(items[:SHOWN])
        return shown + (cat('layout.more', count=len(items) - SHOWN) if len(items) > SHOWN else '')
    lines = []
    n = len(p.files)
    if p.how == 'matched':
        lines.append(cat('layout.matched', top=p.top or cat('layout.archive_top'), matched=p.matched, total=n))
    elif p.how == 'guessed':
        lines.append(cat('layout.guessed', top=p.top or cat('layout.archive_top'), total=n))
    elif p.how == 'loader':
        lines.append(cat('layout.loader', total=n))
    else:
        lines.append(cat('layout.romfs', total=n))
    lines.append(cat('layout.files', files=names(sorted(p.files.values()))))
    if p.new_files and p.how != 'guessed':
        lines.append(cat('layout.added', files=names(p.new_files)))
    if p.tables:
        lines.append(cat('layout.bdat_json', count=len(p.tables)))
    if p.map_pieces:
        lines.append(cat('layout.map_pieces', count=len(p.map_pieces)))
    for reason in ('exefs', 'doc', 'outside'):
        left = [name for name, why in p.left_out if why == reason]
        if left:
            lines.append(cat('layout.left_' + reason, files=names(left)))
    return lines


def needs_confirm(p):
    """Whether the player should see the plan before anything is written: anything but a plain romfs archive whose
    files are all the game's."""
    return p.how not in ('romfs', 'loader') or bool(p.new_files) or bool(p.tables) or bool(p.map_pieces) or any(why == 'outside' for _, why in p.left_out)
