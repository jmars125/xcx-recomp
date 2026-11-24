"""Build and run tests/save_pages_check.c: the save slot pages of runtime/save_pages.c.

Usage: save_pages_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():   # as tests/diff/common.py, which needs Capstone; this check does not
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    work = ROOT / 'build' / 'test' / 'save_pages'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('save_pages_check.exe' if os.name == 'nt' else 'save_pages_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-fno-strict-aliasing', '-Wall', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'save_pages_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
