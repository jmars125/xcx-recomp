"""xcx.ini.example and the section table in runtime/cfg.c agree on where each setting goes.

When the menu saves a setting the player's xcx.ini lacks, cfg_save_to puts it after the key's commented example line,
or else under the section that cfg.c's sections[] table names for the key. A new installation starts from
xcx.ini.example, so a key that the example keeps in one section and the table names another would end up in two
places in different files. The example's sections follow the F1 menu's tabs. No game files.

    python tests/ini_sections_test.py
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KEY_LINE = re.compile(r'^#?\s*([A-Za-z][A-Za-z0-9_]*)\s*=')   # "key = value" or its commented example "# key = value"
SECTION_LINE = re.compile(r'^\[(.+)\]\s*$')


def cfg_table(name):
    src = (ROOT / 'runtime' / 'cfg.c').read_text(encoding='utf-8')
    start = src.index(name)
    return src[start:src.index('\n};', start)]


def table_sections():
    """cfg.c sections[]: [(KEY, section), ...] in table order."""
    table = cfg_table('static const struct { const char *key, *section; } sections[]')
    return re.findall(r'\{"([A-Z0-9_]+)", "([a-z ]+)"\}', table)


def menu_keys():
    """cfg.c metas[]: the settings the F1 menu shows (and so may save)."""
    return set(re.findall(r'\{"([A-Z0-9_]+)", "', cfg_table('static const CfgMeta metas[] = {')))


def example_sections():
    """xcx.ini.example: {KEY: [section, ...]} for every key line, commented or not, and the list of sections."""
    keys, sections, section = {}, [], None
    for line in (ROOT / 'xcx.ini.example').read_text(encoding='utf-8').splitlines():
        m = SECTION_LINE.match(line)
        if m:
            section = m.group(1).strip().lower()
            sections.append(section)
            continue
        m = KEY_LINE.match(line)
        if m and section:
            keys.setdefault(m.group(1).upper().removeprefix('XCX_'), []).append(section)
    return keys, sections


class IniSectionsTest(unittest.TestCase):
    def test_table_has_each_key_once(self):
        seen = [k for k, _ in table_sections()]
        twice = sorted({k for k in seen if seen.count(k) > 1})
        self.assertEqual(twice, [], f'keys listed twice in cfg.c sections[]: {twice}')

    def test_table_sections_exist_in_the_example(self):
        _, sections = example_sections()
        missing = sorted({s for _, s in table_sections() if s not in sections})
        self.assertEqual(missing, [], f'cfg.c sections[] names sections xcx.ini.example lacks: {missing}')

    def test_example_sections_are_unique(self):
        _, sections = example_sections()
        twice = sorted({s for s in sections if sections.count(s) > 1})
        self.assertEqual(twice, [], f'sections that xcx.ini.example opens twice: {twice}')

    def test_example_and_table_agree(self):
        keys, _ = example_sections()
        wrong = [f'{k}: example [{"], [".join(keys[k])}], cfg.c [{s}]'
                 for k, s in table_sections() if k in keys and keys[k] != [s]]
        self.assertEqual(wrong, [], 'keys whose section differs between xcx.ini.example and cfg.c sections[]:\n  '
                         + '\n  '.join(wrong))

    def test_table_keys_are_in_the_example(self):
        # the example documents every key a save can place: one the table lists but the example lacks has no line
        # telling a player what it does, and lands at the end of its section instead of beside its description
        # (the bind_ keys are one family: the example describes them on one pattern line and shows four)
        keys, _ = example_sections()
        missing = sorted(k for k, _ in table_sections() if k not in keys and not k.startswith('BIND_'))
        self.assertEqual(missing, [], f'cfg.c sections[] keys xcx.ini.example has no line for: {missing}')

    def test_menu_settings_in_the_example_have_a_table_section(self):
        # a menu row's key the table lacks lands at the end of the file, outside any section, in a file that has no
        # example line for it
        keys, _ = example_sections()
        table = {k for k, _ in table_sections()}
        missing = sorted(k for k in menu_keys() & set(keys) if k not in table)
        self.assertEqual(missing, [], f'menu settings in xcx.ini.example without a cfg.c sections[] entry: {missing}')


if __name__ == '__main__':
    unittest.main()
