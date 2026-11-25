"""runtime/bdat_merge.c on made-up BDAT files, one case per rule of docs/bdat-merge.md: cells and priority, opaque
changes, rows matched by key and renamed keys, row layouts, removed rows put back or not, references, unique keys,
tables without unique keys, files that can't be merged, and the writer's round trip. No game data: the tables are
written here.

    python tests/bdat_merge_test.py        (needs clang: MSYS2 UCRT64, or XCX_CLANG=<path to clang>)

With XCX_BDAT_FILES=<a.bdat;b.bdat;...> it also checks that the writer rebuilds every table of those files (mods'
copies of bdat/common.bdat, for example) byte for byte. With XCX_BDAT_MERGE_PY=<script> the merge cases run on a
script with tests/bdat_merge_check.c's command line instead (another implementation of the rules, to compare).
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FMT = {1: 'B', 2: 'H', 3: 'I', 4: 'b', 5: 'h', 6: 'i', 7: 'I', 8: 'f', 9: 'I', 10: 'B', 11: 'I', 12: 'B', 13: 'H'}
STR = (7, 11)


def clang():
    # XCX_CLANG may be a path or a name on PATH (CI's Linux job gives just 'clang')
    found = shutil.which(os.environ.get('XCX_CLANG', '')) or os.environ.get('XCX_CLANG') or shutil.which('clang') or \
        ('C:/msys64/ucrt64/bin/clang.exe' if os.name == 'nt' else None)
    return found if found and Path(found).is_file() else None


class Table:
    """name hash, [(column name hash, type)], base id, rows as tuples (strings as bytes), the string table's kept word,
    and debug=True for a block of debug names before the columns, as the game's own tables have."""

    def __init__(self, name, cols, rows, base=1, extra=0, debug=False):
        self.name, self.cols, self.rows, self.base, self.extra, self.debug = name, cols, rows, base, extra, debug

    def copy(self):
        return Table(self.name, list(self.cols), [tuple(r) for r in self.rows], self.base, self.extra, self.debug)


def write_table(t):
    st = bytearray(b'\0') + struct.pack('<II', t.name, t.extra)
    names = []
    for h, _ in t.cols:
        names.append(len(st))
        st += struct.pack('<I', h)
    seen = {b'': 0}
    rows = []
    for row in t.rows:
        row = list(row)
        for i, (_, ty) in enumerate(t.cols):
            if ty in STR:
                if row[i] not in seen:
                    seen[row[i]] = len(st)
                    st += row[i] + b'\0'
                row[i] = seen[row[i]]
        rows.append(row)
    fmt = '<' + ''.join(FMT[ty] for _, ty in t.cols)
    index = {}
    if t.cols[0][1] == 9:
        for r, row in enumerate(t.rows):
            index.setdefault(row[0], r)
    hidx = b''.join(struct.pack('<II', h, r) for h, r in sorted(index.items()))
    dbg = b'debug names\0' * 3 if t.debug else b''
    co = 48 + len(dbg)
    ho = co + 3 * len(t.cols)
    ro = ho + len(hidx)
    so = ro + struct.calcsize(fmt) * len(rows)
    out = bytearray(struct.pack('<4s11I', b'BDAT', 0x3004, len(t.cols), len(rows), t.base, 0, co, ho, ro,
                                struct.calcsize(fmt), so, len(st)))
    out += dbg
    for (h, ty), no in zip(t.cols, names):
        out += struct.pack('<BH', ty, no)
    out += hidx
    for row in rows:
        out += struct.pack(fmt, *row)
    out += st
    out += b'\0' * (-len(out) % 4)
    return bytes(out)


def write_file(tables):
    parts = [write_table(t) for t in tables]
    offs, o = [], 16 + 4 * len(parts)
    for p in parts:
        offs.append(o)
        o += len(p)
    return struct.pack('<4sIII', b'BDAT', 0x01001004, len(parts), o) + struct.pack('<%dI' % len(offs), *offs) + \
        b''.join(parts)


def read_file(b):
    n = struct.unpack_from('<I', b, 8)[0]
    offs = list(struct.unpack_from('<%dI' % n, b, 16)) + [len(b)]
    out = []
    for i in range(n):
        t = offs[i]
        _, _, cc, rc, base, _, co, ho, ro, rl, so, sl = struct.unpack_from('<4s11I', b, t)
        S = t + so
        name, extra = struct.unpack_from('<II', b, S + 1)
        cols = []
        for k in range(cc):
            ty, no = struct.unpack_from('<BH', b, t + co + 3 * k)
            cols.append((struct.unpack_from('<I', b, S + no)[0], ty))
        fmt = '<' + ''.join(FMT[ty] for _, ty in cols)
        rows = []
        for r in range(rc):
            v = list(struct.unpack_from(fmt, b, t + ro + r * rl))
            for k, (_, ty) in enumerate(cols):
                if ty in STR:
                    v[k] = b[S + v[k]:b.index(b'\0', S + v[k])]
            rows.append(tuple(v))
        out.append(Table(name, cols, rows, base, extra))
    return out


COLS = [(0x11111111, 9), (0x22222222, 1), (0x33333333, 5), (0x44444444, 8), (0x55555555, 7), (0x66666666, 13)]
REF_COLS = [(0x77777771, 9), (0x77777772, 9), (0x77777773, 1)]   # column 1 refers to rows of table 0 by key


def make_rows(n, seed):
    return [(0x1000 * (seed + 1) + r, r & 0xff, -r, r / 2, b'name%d' % (r % 3) if r % 4 else b'', r * 7)
            for r in range(n)]


def game():
    """0, 1: keyed; 2: keyed, base id 10; 3: keyed, three columns; 4: duplicate keys (matched by row number);
    5: keyed, column 1 refers to table 0's keys."""
    return [Table(0xA0000001, COLS, make_rows(8, 0), debug=True, extra=0x12345678),
            Table(0xA0000002, COLS, make_rows(6, 1), debug=True, extra=0x9abcdef0),
            Table(0xA0000003, [(0x77777777, 9), (0x88888888, 2)], [(0x5000 + r, r) for r in range(5)], base=10,
                  debug=True),
            Table(0xA0000004, COLS[:3], [(0x6000 + r, r, r) for r in range(4)], debug=True),
            Table(0xA0000005, COLS[:3], [(0x6100 + r // 2, r, r) for r in range(6)], debug=True),
            Table(0xA0000006, REF_COLS, [(0x7000 + r, 0x1000 + r, r) for r in range(3)], debug=True)]


def mod_of(tables):
    """A mod tool's copy: the same tables without the debug block and with the kept word cleared."""
    out = []
    for t in tables:
        c = t.copy()
        c.debug, c.extra = False, 0
        out.append(c)
    return out


def edit(t, r, c, v):
    row = list(t.rows[r])
    row[c] = v
    t.rows[r] = tuple(row)


def keys(t):
    return [r[0] for r in t.rows]


def engine():
    """('c', clang), ('py', a script with the same command line), or None."""
    py = os.environ.get('XCX_BDAT_MERGE_PY')
    if py:
        return 'py', py
    return ('c', clang()) if clang() else None




def mm3(d):
    """murmur3 (32 bits, seed 0): bdat-toolset's hash of plain names."""
    c1, c2, h, n, M = 0xcc9e2d51, 0x1b873593, 0, len(d), 0xffffffff
    rot = lambda x, k: ((x << k) | (x >> (32 - k))) & M
    for i in range(0, n - n % 4, 4):
        k = rot(struct.unpack_from('<I', d, i)[0] * c1 & M, 15) * c2 & M
        h = (rot(h ^ k, 13) * 5 + 0xe6546b64) & M
    k = 0
    for j, b in enumerate(d[n - n % 4:]):
        k |= b << (8 * j)
    if n % 4:
        h ^= rot(k * c1 & M, 15) * c2 & M
    h ^= n
    h ^= h >> 16
    h = h * 0x85ebca6b & M
    h ^= h >> 13
    h = h * 0xc2b2ae35 & M
    return h ^ (h >> 16)


PLAIN = {mm3(s.encode()): s for s in ('Id', 'Count', 'FirstRow', 'T0')}   # made-up names and their hashes


def named(tables):
    """The made-up game with table 0, its first two columns and its first row's key named by PLAIN's hashes."""
    t = tables[0]
    t.name = mm3(b'T0')
    t.cols = [(mm3(b'Id'), t.cols[0][1]), (mm3(b'Count'), t.cols[1][1])] + t.cols[2:]
    t.rows[0] = (mm3(b'FirstRow'),) + tuple(t.rows[0][1:])
    tables[3].name = mm3(b'DEADBEEF')   # a plain name that is 8 hex digits: not a table's hash, so its murmur3
    return tables


# bdat-toolset names a table known only by its hash with the 8 hex digits, no brackets (toolset/src/convert/schema.rs,
# AsFileName for Label); the reader also takes <8 hex digits>.
HASH_STEM = '%08X'


def to_json(t, plain=False):
    """Table t as bdat-toolset writes it with -f json; plain: the names and hash cells PLAIN knows as plain text."""
    import json as _json
    name = lambda h: PLAIN[h] if plain and h in PLAIN else '<%08X>' % h
    cols = [name(h) for h, _ in t.cols]
    rows = []
    for i, r in enumerate(t.rows):
        row = {'$id': t.base + i}
        for n, (_, ty), v in zip(cols, t.cols, r):
            row[n] = name(v) if ty == 9 else v.decode('utf-8') if ty in (7, 11) else v
        rows.append(row)
    return _json.dumps({'schema': [{'name': n, 'type': ty} for n, (_, ty) in zip(cols, t.cols)], 'rows': rows},
                       indent=2)


@unittest.skipUnless(engine(), 'needs clang (MSYS2 UCRT64, or XCX_CLANG)')
class BdatMerge(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        kind, path = engine()
        if kind == 'py':
            cls.cmd = [sys.executable, path]
            return
        exe = cls.dir / ('bdat_merge_check' + ('.exe' if os.name == 'nt' else ''))
        rt = ROOT / 'runtime'
        subprocess.run([path, '-O1', '-Wall', '-Werror', f'-I{rt}', str(ROOT / 'tests/bdat_merge_check.c'),
                        str(rt / 'bdat_merge.c'), '-o', str(exe)], check=True)
        cls.cmd = [str(exe)]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def put(self, name, data):
        p = self.dir / name
        p.write_bytes(data)
        return str(p)

    def merge(self, base, *mods, expect=0):
        """mods: (name, tables or bytes) highest priority first. Returns (tables, notes, summary)."""
        args = self.cmd + ['merge', self.put('game.bdat', write_file(base)), str(self.dir / 'out.bdat')]
        for name, tables in mods:
            data = tables if isinstance(tables, bytes) else write_file(tables)
            args.append('%s=%s' % (name, self.put(name + '.bdat', data)))
        r = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(r.returncode, expect, r.stdout + r.stderr)
        lines = r.stdout.splitlines()
        notes = [x[5:] for x in lines if x.startswith('note ')]
        if expect:
            return None, notes, lines
        summary = dict(kv.split('=', 1) for kv in next(x for x in lines if x.startswith('summary ')).split()[1:])
        summary = {k: v if k == 'file' else int(v) for k, v in summary.items()}
        return read_file((self.dir / 'out.bdat').read_bytes()), notes, summary

    def has(self, notes, *parts):
        self.assertTrue(any(all(p in n for p in parts) for n in notes), '%s not in %s' % (parts, notes))

    # ---- the writer

    def test_round_trip(self):
        if self.cmd[0] == sys.executable:
            self.skipTest('the C writer only')
        tables = mod_of(game())
        edit(tables[0], 2, 4, b'')
        path = self.put('rt.bdat', write_file(tables))
        out = subprocess.run(self.cmd + ['rewrite', path], capture_output=True, text=True, check=True).stdout
        self.assertEqual(out.split()[-1], '0')

    def test_real_files_round_trip(self):
        files = [f for f in os.environ.get('XCX_BDAT_FILES', '').split(';') if f]
        if not files or self.cmd[0] == sys.executable:
            self.skipTest('XCX_BDAT_FILES not set (the C writer only)')
        out = subprocess.run(self.cmd + ['rewrite', *files], capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            self.assertTrue(line.endswith(' 0'), line)

    # ---- cells and whole tables

    def test_cells_merge_and_the_top_mod_wins_a_clash(self):
        g = game()
        a, b = mod_of(g), mod_of(g)
        edit(a[0], 1, 2, 100)            # both set table 0 row 1 column 2
        edit(b[0], 1, 2, 200)
        edit(b[0], 2, 3, 9.5)            # only B
        edit(a[0], 3, 4, b'from a')      # only A, a string
        edit(b[0], 4, 4, b'name1')       # B reuses a string that is already there
        edit(b[0], 5, 1, 9)              # both set the same value: no clash
        edit(a[0], 5, 1, 9)
        for order, winner in (((('a', a), ('b', b)), 100), ((('b', b), ('a', a)), 200)):
            out, notes, s = self.merge(g, *order)
            t = out[0]
            self.assertEqual(t.rows[1][2], winner)
            self.assertEqual((t.rows[2][3], t.rows[3][4], t.rows[4][4], t.rows[5][1]), (9.5, b'from a', b'name1', 9))
            self.assertEqual(t.rows[6], g[0].rows[6])
            self.assertEqual((s['changed'], s['merged'], s['priority'], s['leftout'], s['caveat']), (1, 1, 1, 0, 0))
            self.has(notes, 'priority', "column 2: %s's value, not %s's" % (order[0][0], order[1][0]))
            self.assertEqual([x.rows for x in out[1:]], [x.rows for x in g[1:]])

    def test_tables_keep_their_bytes(self):
        g = game()
        a, b = mod_of(g), mod_of(g)
        edit(a[1], 0, 1, 7)
        del b[2].rows[4]                 # one mod only: any change is fine, its own table is used
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        got = (self.dir / 'out.bdat').read_bytes()
        self.assertEqual((s['changed'], s['merged']), (2, 0))
        self.assertEqual((out[1].rows, out[2].rows), (a[1].rows, b[2].rows))
        for t in (g[0], g[3], a[1], b[2]):   # unchanged tables are the game's bytes, debug block included
            self.assertIn(write_table(t), got)
        self.assertNotIn(write_table(g[1]), got)

    def test_opaque_changes(self):
        for top in ('a', 'b'):
            with self.subTest(top=top):
                g = game()
                a, b = mod_of(g), mod_of(g)
                edit(a[1], 2, 2, 77)
                b[1].cols, b[1].rows = b[1].cols[:-1], [r[:-1] for r in b[1].rows]   # B drops a column
                order = (('a', a), ('b', b)) if top == 'a' else (('b', b), ('a', a))
                out, notes, s = self.merge(g, *order)
                if top == 'b':
                    self.assertEqual((out[1].cols, out[1].rows), (b[1].cols, b[1].rows))
                    self.assertEqual(s['whole'], 1)
                    self.has(notes, 'left out: a:', "b's whole table is (other columns)")
                else:
                    self.assertEqual((out[1].cols, out[1].rows), (a[1].cols, a[1].rows))
                    self.has(notes, 'left out: b:', 'other columns')
                self.assertEqual(s['leftout'], 1)

    # ---- rows

    def test_rows_added_by_several_mods(self):
        g = game()
        a, b = mod_of(g), mod_of(g)
        edit(a[0], 0, 1, 50)
        b[0].rows += [(0x9001, 1, 2, 3.0, b'new row', 4), (0x9002, 5, 6, 7.0, b'', 8)]
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        self.assertEqual(out[0].rows, a[0].rows + b[0].rows[-2:])
        self.assertEqual(s['merged'], 1)
        a[0].rows += [b[0].rows[-2]]                          # the same row from both: kept once, silently
        a[0].rows += [(0x9002, 9, 9, 9.0, b'a', 9)]           # the same key, other cells: the higher mod's
        a[0].rows += [(0x9003, 0, 0, 0.0, b'', 0)]            # another key: kept too
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        self.assertEqual(out[0].rows, a[0].rows)
        self.assertEqual((s['priority'], s['leftout']), (1, 0))
        self.has(notes, 'priority', "added row (key 00009002): a's, not b's")

    def test_a_renamed_key_is_the_same_row(self):
        g = game()
        a, b = mod_of(g), mod_of(g)
        edit(a[0], 3, 0, 0xdeadbeef)     # A renames row 3; its other cells stay
        edit(b[0], 3, 2, 99)             # B edits row 3 under its old key
        b[0].rows.insert(0, (0x9000, 0, 0, 0.0, b'', 0))     # and adds a row at the top, so the match is by key
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        t = out[0]
        k = keys(t).index(0xdeadbeef)
        self.assertEqual(t.rows[k][2], 99)
        self.assertNotIn(0x1003, keys(t))
        self.assertEqual(len(t.rows), 9)

    def test_rows_removed_in_the_middle(self):
        g = game()
        a, b = mod_of(g), mod_of(g)
        del b[0].rows[3]                 # B removes row 3: its row layout
        edit(a[0], 5, 1, 55)             # A edits a row after it, by key
        edit(a[0], 3, 1, 33)             # and the removed row
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        t = out[0]
        self.assertEqual(keys(t), keys(b[0]))
        self.assertEqual(t.rows[keys(t).index(0x1005)][1], 55)
        self.has(notes, 'left out: a:', 'putting it back would renumber')
        self.has(notes, 'caveat: a:', 'other row numbers than in its copy')
        out, notes, s = self.merge(g, ('b', b), ('a', a))      # B on top: A's edit to the removed row is lost
        self.assertEqual(keys(out[0]), keys(b[0]))
        self.has(notes, 'left out: a:', 'that b removes')

    def test_rows_removed_at_the_end(self):
        g = game()
        a, b = mod_of(g), mod_of(g)
        del b[0].rows[6:]                # B removes rows 6 and 7
        edit(a[0], 6, 1, 66)             # A (higher) edits row 6: it comes back at its own number
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        self.assertEqual(keys(out[0]), keys(g[0])[:7])
        self.assertEqual(out[0].rows[6][1], 66)
        self.has(notes, 'caveat: b:', 'row 7 (key 00001006) is kept, although it removes it, for a')
        a2 = mod_of(g)
        edit(a2[0], 7, 1, 77)            # row 7 alone can't come back at its number
        out, notes, s = self.merge(g, ('a', a2), ('b', b))
        self.assertEqual(keys(out[0]), keys(b[0]))
        self.has(notes, 'left out: a:', 'putting it back would renumber')
        out, notes, s = self.merge(g, ('b', b), ('a', a))      # A lower: the removal stands
        self.assertEqual(keys(out[0]), keys(b[0]))
        self.has(notes, 'left out: a:', 'that b removes')

    def test_a_row_another_mod_refers_to_is_kept(self):
        g = game()
        b, c = mod_of(g), mod_of(g)
        del b[0].rows[7]                 # B removes the last row of table 0
        edit(b[0], 0, 1, 1)
        c[5].rows.append((0x7003, 0x1007, 3))     # C adds a row to table 5 that refers to it
        edit(c[0], 1, 1, 2)
        out, notes, s = self.merge(g, ('b', b), ('c', c))
        self.assertIn(0x1007, keys(out[0]))
        self.has(notes, 'caveat: b:', 'kept, although it removes it, for c')
        b2 = mod_of(g)
        del b2[0].rows[2]                # in the middle it can't come back: reported
        c[5].rows[-1] = (0x7003, 0x1002, 3)
        out, notes, s = self.merge(g, ('b', b2), ('c', c))
        self.assertNotIn(0x1002, keys(out[0]))
        self.has(notes, 'caveat: c:', 'it refers to row 3 (key 00001002), which b removes')

    def test_two_row_layouts(self):
        g = game()
        a, b = mod_of(g), mod_of(g)
        del a[0].rows[2]
        del b[0].rows[4]
        b[0].rows.append((0x9009, 1, 1, 1.0, b'b', 1))
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        self.assertEqual(keys(out[0]), keys(a[0]) + [0x9009])
        self.has(notes, 'left out: b:', "a's row layout is")

    def test_the_same_row_layout_twice_loses_nothing(self):
        g = game()
        a, b = mod_of(g), mod_of(g)   # a whole copy and one of its pieces both remove the same row
        del a[0].rows[2]
        del b[0].rows[2]
        edit(a[1], 0, 1, 7)
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        self.assertEqual(keys(out[0]), keys(a[0]))
        self.assertFalse([n for n in notes if n.startswith('left out')], notes)
        self.assertEqual(s['leftout'], 0)

    def test_a_mod_a_higher_one_already_contains_adds_nothing(self):
        g = game()
        whole, piece, other = mod_of(g), mod_of(g), mod_of(g)
        edit(whole[0], 1, 1, 7)
        edit(whole[1], 2, 1, 8)
        edit(piece[1], 2, 1, 8)      # the piece: one of the whole version's tables, alike
        edit(other[1], 2, 1, 8)
        edit(other[3], 0, 1, 9)      # alike in one table, its own change in another
        out, notes, s = self.merge(g, ('whole', whole), ('piece', piece), ('other', other))
        self.has(notes, 'adds nothing: piece:', 'every table it changes is the same in whole')
        self.has(notes, 'partly alike: other:', '1 of the 2 tables it changes is the same in whole')
        self.assertFalse([n for n in notes if n.startswith('adds nothing: other')], notes)
        self.assertFalse([n for n in notes if ': whole:' in n], notes)
        self.assertEqual((s['nothing'], s['alike'], s['leftout'], s['priority']), (1, 1, 0, 0))

    def test_debug_names_are_not_changes(self):
        def with_labels():
            return game() + [Table(0xA0000007, [(0x11111111, 9), (0x22222222, 1), (0x99999999, 11)],
                                   [(0x8000 + r, r, b'label%d' % r) for r in range(4)], debug=True)]
        def blanked(m):
            m[6].rows = [r[:2] + (b'',) for r in m[6].rows]
            return m
        g = with_labels()
        a, b = blanked(mod_of(g)), mod_of(g)       # a mod tool's empty labels, and nothing else, in a's table 6
        edit(b[0], 1, 1, 50)
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        self.assertEqual([r[2] for r in out[6].rows], [r[2] for r in g[6].rows])
        self.assertEqual((s['changed'], s['merged']), (1, 0))
        a, b = blanked(mod_of(g)), blanked(mod_of(g))   # both change table 6 for real: the game's labels stay
        edit(a[6], 1, 1, 50)
        edit(b[6], 2, 1, 60)
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        self.assertEqual([r[1:] for r in out[6].rows], [(0, b'label0'), (50, b'label1'), (60, b'label2'),
                                                        (3, b'label3')])
        self.assertEqual((s['changed'], s['merged'], s['priority']), (1, 1, 0))

    def test_keys_stay_unique(self):
        g = game()
        a, b = mod_of(g), mod_of(g)
        edit(a[0], 3, 0, 0x9999)                          # A renames row 3 to 9999
        b[0].rows.append((0x9999, 1, 2, 3.0, b'b', 4))     # B adds a row 9999
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        self.assertEqual(keys(out[0]).count(0x9999), 1)
        self.assertEqual(out[0].rows[3][0], 0x9999)
        self.has(notes, 'left out: b:', 'its added row (key 00009999): another row has that key')
        out, notes, s = self.merge(g, ('b', b), ('a', a))
        self.assertEqual(keys(out[0]).count(0x9999), 1)
        self.assertEqual(out[0].rows[3][0], 0x1003)
        self.has(notes, 'left out: a:', 'its new key 00009999 for row 4')

    def test_tables_without_unique_keys(self):
        g = game()
        a, b = mod_of(g), mod_of(g)
        a[4].rows.append((0x6200, 1, 1))
        b[4].rows.append((0x6201, 2, 2))
        edit(b[4], 1, 2, 5)
        out, notes, s = self.merge(g, ('a', a), ('b', b))
        self.assertEqual(out[4].rows[6:], [a[4].rows[-1], b[4].rows[-1]])
        self.assertEqual(out[4].rows[1][2], 5)
        self.has(notes, 'caveat: b:', '1 of its rows have other row numbers')
        b2 = mod_of(g)
        edit(b2[4], 0, 0, 0x6102)        # a row given another row's key: rows moved, B's table can't be matched
        out, notes, s = self.merge(g, ('a', a), ('b', b2))
        self.assertEqual(out[4].rows, a[4].rows)
        self.has(notes, 'left out: b:', 'rows moved')

    # ---- files

    def test_files_that_cant_be_merged(self):
        g = game()
        a, b = mod_of(g), mod_of(g)
        edit(a[0], 0, 1, 1)
        edit(b[0], 0, 2, 2)
        out, notes, s = self.merge(g, ('a', a), ('b', b[:5]))      # B lacks a table: left out
        self.assertEqual(s['file'], 'copied:a')
        self.assertEqual(out[0].rows, a[0].rows)
        self.has(notes, 'left out: b:', 'another list of tables')
        out, notes, s = self.merge(g, ('b', b[:5]), ('a', a))      # on top: its whole file
        self.assertEqual(s['file'], 'whole:b')
        self.assertEqual(len(out), 5)
        self.has(notes, 'left out: a:', "b's copy has another list of tables and is used whole")
        good = write_file(b)
        t0 = struct.unpack_from('<I', good, 16)[0]
        ro = struct.unpack_from('<I', good, t0 + 32)[0]
        bad_string = bytearray(good)     # row 0's string cell (column 4, byte 11 of the row) points past the strings
        struct.pack_into('<I', bad_string, t0 + ro + 11, 0xffffff00)
        for name, data in (('short', good[:100]), ('magic', b'XDAT' + good[4:]),
                           ('offset', good[:16] + struct.pack('<I', len(good) + 8) + good[20:]),
                           ('string', bytes(bad_string))):
            with self.subTest(name):
                out, notes, s = self.merge(g, ('a', a), ('b', data))
                self.assertEqual(s['file'], 'copied:a')
                self.has(notes, 'left out: b:', "can't be read")
        _, notes, lines = self.merge(g, ('a', good[:100]), ('b', good[:50]), expect=2)
        self.assertTrue(any(x.startswith('unmergeable') for x in lines), lines)

    # ---- bdat-toolset JSON tables

    def merge_json(self, base, *mods, expect=0):
        """mods: (name, tables or None, {table index: json text}) highest priority first."""
        args = self.cmd + ['merge', self.put('game.bdat', write_file(base)), str(self.dir / 'out.bdat')]
        for name, tables, jsons in mods:
            if tables is not None:
                args.append('%s=%s' % (name, self.put(name + '.bdat', write_file(tables))))
            for stem, text in jsons:
                d = self.dir / name
                d.mkdir(exist_ok=True)
                args.append('%s=%s' % (name, self.put('%s/%s.json' % (name, stem), text.encode('utf-8')
                                                      if isinstance(text, str) else text)))
        r = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(r.returncode, expect, r.stdout + r.stderr)
        lines = r.stdout.splitlines()
        notes = [x[5:] for x in lines if x.startswith('note ')]
        summary = dict(kv.split('=', 1) for kv in next(x for x in lines if x.startswith('summary ')).split()[1:])
        return read_file((self.dir / 'out.bdat').read_bytes()), notes, summary

    def test_json_tables_merge_like_tables(self):
        g = named(game())
        part1, part2, full = g[0].copy(), g[0].copy(), mod_of(g)
        edit(part1, 1, 1, 11)                      # two parts of one mod, other cells of the same table
        edit(part2, 2, 2, 22)
        edit(full[0], 1, 1, 11)                    # a full copy that has part 1's change too, and one of its own
        edit(full[0], 5, 3, 0.5)
        part3 = g[3].copy()
        edit(part3, 0, 1, 33)
        out, notes, s = self.merge_json(g, ('p1', None, [('T0', to_json(part1, plain=True))]),
                                        ('p2', None, [(HASH_STEM % g[0].name, to_json(part2)),
                                                      ('DEADBEEF', to_json(part3))]),
                                        ('all', full, []))
        t = out[0]
        self.assertEqual((t.rows[1][1], t.rows[2][2], t.rows[5][3]), (11, 22, 0.5))
        self.assertEqual(out[3].rows[0][1], 33)
        out[3] = g[3]
        self.assertEqual((s['priority'], s['leftout'], s['caveat'], s['file']), ('0', '0', '0', 'merged'))
        self.assertEqual([x.rows for x in out[1:]], [x.rows for x in g[1:]])
        out, notes, s = self.merge_json(g, ('p1', None, [('T0', to_json(part1, plain=True))]))
        self.assertEqual(s['file'], 'copied:p1')             # one JSON mod alone: the game's file with its table
        self.assertEqual(out[0].rows, part1.rows)
        self.assertIn(write_table(g[1]), (self.dir / 'out.bdat').read_bytes())

    def test_json_table_with_no_change_is_no_change(self):
        g = named(game())
        a = mod_of(g)
        edit(a[0], 1, 1, 9)
        out, notes, s = self.merge_json(g, ('p', None, [('T0', to_json(g[0], plain=True))]), ('a', a, []))
        self.assertEqual((s['changed'], s['merged'], notes), ('1', '0', []))

    def test_json_tables_that_cant_be_used(self):
        g = named(game())
        a = mod_of(g)
        edit(a[0], 1, 1, 9)
        t = g[0].copy()
        edit(t, 3, 1, 7)
        good = to_json(t, plain=True)
        cases = {
            'not valid JSON': good[:-3],
            'no schema or rows': '{"schema": [], "row": []}',
            'not numbered one after another': good.replace('"$id": 3', '"$id": 30'),
            'not a value of its type': good.replace('"Count": 7', '"Count": 700'),
            "has no such table": None,
        }
        for reason, text in cases.items():
            with self.subTest(reason):
                stem = 'T0' if text is not None else 'NoSuchTable'
                out, notes, s = self.merge_json(g, ('p', None, [(stem, text if text is not None else good)]),
                                                ('a', a, []))
                self.has(notes, 'left out: p:', 'is not used', reason)
                self.assertEqual(out[0].rows, a[0].rows)


if __name__ == '__main__':
    unittest.main()
