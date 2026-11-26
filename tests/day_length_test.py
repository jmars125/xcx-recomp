"""runtime/day_length.h through tests/day_length_check.c: the Day length setting's parsing and its faster and slower
clock on a simulated game clock (60 and 30 frames a second, the game's menu, a time change in it, a load, real midnight). No
game files.

    python tests/day_length_test.py        (needs clang: MSYS2 UCRT64, or XCX_CLANG=<path to clang>; it skips without
                                            one, unless XCX_REQUIRE_CLANG=1, as on CI, makes that a failure)
"""
import os
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def clang():
    # XCX_CLANG may be a path or a name on PATH (CI's Linux job gives just 'clang')
    found = shutil.which(os.environ.get('XCX_CLANG', '')) or os.environ.get('XCX_CLANG') or shutil.which('clang') or \
        ('C:/msys64/ucrt64/bin/clang.exe' if os.name == 'nt' else None)
    return found if found and Path(found).is_file() else None


class DayLength(unittest.TestCase):
    def test_check(self):
        cc = clang()
        if not cc:
            if os.environ.get('XCX_REQUIRE_CLANG'):   # CI: a skip would be a pass that tested nothing
                self.fail('no clang (XCX_CLANG)')
            self.skipTest('no clang')
        work = ROOT / 'build' / 'test' / 'day_length'
        work.mkdir(parents=True, exist_ok=True)
        exe = work / ('day_length_check.exe' if os.name == 'nt' else 'day_length_check')
        env = dict(os.environ)
        if os.name == 'nt':
            env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
        for opt in ('-O0', '-O2'):
            subprocess.run([cc, opt, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                            str(ROOT / 'tests' / 'day_length_check.c'), '-o', str(exe), '-lm'], check=True, env=env)
            r = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
            print(opt, r.stdout.strip().splitlines()[-1] if r.stdout.strip() else '')
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
