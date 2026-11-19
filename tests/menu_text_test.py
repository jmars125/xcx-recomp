"""Every settings description in runtime/ui_text.c, in every language, fits the menu's description pane.

The pane (runtime/ui/option_rows.cpp desc_set) copies a description into 512 bytes and then adds a note under it: "set
by environment variable XCX_..." when the environment fixes the row, or "after restart". A description that leaves
no room for the note loses it, and one over 512 bytes shows nothing at all. Cyrillic letters take 2 bytes in UTF-8,
so a Russian text of the same length as its English one is about twice as long here. No game files.

    python tests/menu_text_test.py
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PANE = 512          # option_rows.cpp struct Desc


def key_room():
    """Bytes of the longest XCX_ name the environment note can show: a key of cfg.c's settings table (metas[])."""
    src = (ROOT / 'runtime' / 'cfg.c').read_text(encoding='utf-8')
    table = src[src.index('static const CfgMeta metas[] = {'):]
    table = table[:table.index('\n};')]
    return 4 + max(len(k) for k in re.findall(r'\{"([A-Z0-9_]+)", "', table))

ESC = {'n': b'\n', 't': b'\t', '"': b'"', '\\': b'\\', "'": b"'", '0': b'\0'}


def c_string(body):
    """The bytes of one C string literal's body (UTF-8 source, C escapes)."""
    out, i = bytearray(), 0
    while i < len(body):
        ch = body[i]
        if ch != '\\':
            out += ch.encode('utf-8')
            i += 1
            continue
        e = body[i + 1]
        if e == 'x':
            m = re.match(r'[0-9a-fA-F]{1,2}', body[i + 2:])
            out.append(int(m.group(0), 16))
            i += 2 + len(m.group(0))
        elif e in ESC:
            out += ESC[e]
            i += 2
        else:
            raise ValueError(f'escape \\{e} not handled')
    return bytes(out)


def tables():
    """{language table name: {UI_ id: bytes}} for the text_<lang>[] tables of ui_text.c."""
    src = (ROOT / 'runtime' / 'ui_text.c').read_text(encoding='utf-8')
    out = {}
    for m in re.finditer(r'static const char \*const (text_\w+)\[UI_TEXT_COUNT\] = \{(.*?)\n\};', src, re.S):
        entries = {}
        # every "[UI_X] =" entry must be read to its end (string literals up to the comma): a comment or macro between
        # the pieces would otherwise cut a description short and the check would pass on too few bytes
        starts = len(re.findall(r'\[UI_\w+\]\s*=', m.group(2)))
        for e in re.finditer(r'\[(UI_\w+)\]\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+)', m.group(2)):
            entries[e.group(1)] = b''.join(c_string(s) for s in re.findall(r'"((?:[^"\\]|\\.)*)"', e.group(2)))
            rest = m.group(2)[e.end():].lstrip()
            if not (rest.startswith(',') or rest.startswith('[') or not rest):
                raise ValueError(f'{m.group(1)} {e.group(1)}: the value does not end after its string literals')
        if len(entries) != starts:
            raise ValueError(f'{m.group(1)}: {starts} entries, {len(entries)} read')
        out[m.group(1)] = entries
    return out


class MenuText(unittest.TestCase):
    def test_descriptions_fit_the_pane(self):
        t = tables()
        self.assertIn('text_en', t)
        self.assertIn('text_ru', t)
        en = t['text_en']
        too_long = []
        KEY_ROOM = key_room()
        self.assertGreater(KEY_ROOM, 10)
        for name, entries in t.items():
            env = entries.get('UI_MENU_SET_BY_ENV', en['UI_MENU_SET_BY_ENV'])
            restart = entries.get('UI_MENU_AFTER_RESTART', en['UI_MENU_AFTER_RESTART'])
            note = 2 + max(len(env) + 1 + KEY_ROOM, len(restart))   # "\n\n" and the longer note
            for key, text in entries.items():
                if key.endswith('_DESC') and len(text) + note >= PANE:
                    too_long.append(f'{name} {key}: {len(text)} bytes, the pane leaves {PANE - 1 - note}')
        self.assertFalse(too_long, '\n'.join(too_long))

    def test_day_length_texts(self):
        t = tables()
        for name in ('text_en', 'text_ru'):
            for key in ('UI_MENU_DAY_NIGHT_GROUP', 'UI_MENU_DAY_LENGTH', 'UI_MENU_DAY_LENGTH_DESC', 'UI_MENU_DAY_ORIGINAL', 'UI_MENU_DAY_STOPPED',
                        'UI_MENU_DAY_STEP', 'UI_MENU_DAY_UNSUPPORTED', 'UI_MENU_DAY_NO_CLOCK'):
                self.assertIn(key, t[name], f'{name} lacks {key}')
            self.assertEqual(t[name]['UI_MENU_DAY_STEP'].count(b'%s'), 2, f'{name} UI_MENU_DAY_STEP takes two %s')
            self.assertEqual(t[name]['UI_MENU_DAY_STEP'].count(b'%'), 2, f'{name} UI_MENU_DAY_STEP has no other %')

    def test_dawn_dusk_texts(self):
        t = tables()
        for name in ('text_en', 'text_ru'):
            for key in ('UI_MENU_DAWN_DUSK', 'UI_MENU_DAWN_DUSK_DESC', 'UI_MENU_DAWN_DUSK_SMOOTHED', 'UI_MENU_DAWN_DUSK_EXTENDED',
                        'UI_MENU_DARK_NIGHTS', 'UI_MENU_DARK_NIGHTS_DESC', 'UI_MENU_DARK_NIGHTS_DARKER',
                        'UI_MENU_DARK_NIGHTS_DARKEST'):
                self.assertIn(key, t[name], f'{name} lacks {key}')


if __name__ == '__main__':
    unittest.main()
