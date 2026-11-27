"""The F1 Mods tab's library list, headless: builds tests/mods_library_check.cpp with ImGui and runs it.

    python tests/mods_library_test.py        (needs clang: MSYS2 UCRT64, or XCX_CLANG=<path to clang>)

No game, window or GPU: ImGui draws into nothing and the check reads what the page recorded.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def clangxx():
    """clang++ beside the clang of XCX_CLANG, on PATH or in MSYS2 UCRT64; None when there is none."""
    cc = os.environ.get('XCX_CLANG') or shutil.which('clang') or \
        ('C:/msys64/ucrt64/bin/clang.exe' if os.name == 'nt' else None)
    if not cc:
        return None
    p = Path(cc)
    xx = p.with_name(p.name.replace('clang', 'clang++', 1))
    found = xx if xx.is_file() else shutil.which(str(xx))
    return str(found) if found else None


@unittest.skipUnless(clangxx(), 'needs clang (MSYS2 UCRT64, or XCX_CLANG)')
class LibraryListTest(unittest.TestCase):
    def test_parts_sit_under_one_row_and_the_list_scrolls_in_its_box(self):
        rt = ROOT / 'runtime'
        im = rt / 'third_party' / 'imgui'
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / ('mods_library_check' + ('.exe' if os.name == 'nt' else ''))
            text = Path(tmp) / 'ui_text.o'
            subprocess.run([clangxx(), '-x', 'c', '-std=gnu11', '-O0', '-w', f'-I{rt}', '-c', str(rt / 'ui_text.c'),
                            '-o', str(text)], check=True)
            subprocess.run([clangxx(), '-std=gnu++17', '-O0', '-w', f'-I{rt}', f'-I{im}',
                            str(ROOT / 'tests/mods_library_check.cpp'), str(im / 'imgui.cpp'),
                            str(im / 'imgui_draw.cpp'), str(im / 'imgui_tables.cpp'), str(im / 'imgui_widgets.cpp'),
                            str(text), '-o', str(exe)] + (['-static'] if os.name == 'nt' else []), check=True)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue(r.stdout.startswith("ok "), r.stdout)


if __name__ == '__main__':
    unittest.main()
