"""runtime/romfs_mods.c's "what changed since start-up" (tests/romfs_mods_check.c): mod files are looked at only after
a list change and when the Mods tab asks, one look serves every row, bdat_merge_auto waits until the files stay put,
and a merge made ahead isn't made again and again.

    python tests/romfs_mods_test.py        (needs clang: MSYS2 UCRT64, or XCX_CLANG=<path to clang>)
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def clang():
    # XCX_CLANG may be a path or a name on PATH (CI's Linux job gives just 'clang')
    found = shutil.which(os.environ.get('XCX_CLANG', '')) or os.environ.get('XCX_CLANG') or shutil.which('clang') or \
        ('C:/msys64/ucrt64/bin/clang.exe' if os.name == 'nt' else None)
    return found if found and Path(found).is_file() else None


class RomfsModsTest(unittest.TestCase):
    @unittest.skipUnless(clang(), 'needs clang (MSYS2 UCRT64, or XCX_CLANG)')
    def test_changes_since_start_up(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            exe = tmp / 'romfs_mods_check.exe'
            plat = 'win32' if os.name == 'nt' else 'linux'
            libs = ['-lwinmm', '-lbcrypt'] if os.name == 'nt' else ['-lpthread', '-lm', '-ldl']
            rt = ROOT / 'runtime'
            stub = []
            if os.name != 'nt':   # linux.c's one SDL call, without an SDL3 package (as the workflow's compile step)
                sdl = tmp / 'sdl'
                (sdl / 'SDL3').mkdir(parents=True)
                (sdl / 'SDL3' / 'SDL_clipboard.h').write_text(
                    '#include <stdbool.h>\nbool SDL_SetClipboardText(const char *text);\n')
                (sdl / 'sdl_stub.c').write_text(
                    '#include <stdbool.h>\nbool SDL_SetClipboardText(const char *text) { (void)text; return 0; }\n')
                stub = [f'-I{sdl}', str(sdl / 'sdl_stub.c')]
            subprocess.run([clang(), '-std=gnu11', '-O1', '-Wall', f'-I{rt}', f'-I{rt / "platform"}', *stub,
                            str(ROOT / 'tests/romfs_mods_check.c'), str(rt / f'platform/{plat}.c'),
                            str(rt / 'platform/common.c'), *libs, '-o', str(exe)], check=True)
            mods = tmp / 'mods'
            for mid in ('a', 'b'):
                (mods / mid / 'romfs' / 'bdat').mkdir(parents=True)
                (mods / mid / 'romfs' / 'bdat' / 'x.bdat').write_text(f"{mid}'s copy")
            (mods / 'romfs-enabled.txt').write_text('a\nb\n')
            run = subprocess.run([str(exe)], env=dict(os.environ, XCX_MOD_DIR=str(mods)), capture_output=True,
                                 text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertTrue(run.stdout.startswith('ok '), run.stdout)


if __name__ == '__main__':
    unittest.main()
