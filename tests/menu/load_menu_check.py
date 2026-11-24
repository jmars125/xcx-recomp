"""Checks of the Load menu hooks without game files: the generator's site table and the runtime on synthetic memory.

Run from the repository root with .venv (Linux or another POSIX host; the runtime test maps memory at the game's
addresses, which AddressSanitizer reserves, so only UBSan is on). It compiles tests/menu/load_menu.c with runtime/load_menu.c twice, once for the game name 2.0.0 and once for
another version, and runs both.
"""
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'recomp'))
import load_menu  # noqa: E402


def mod(build, sha, base=0x7000000000, name='main', words=None):
    table = {base + off: w for off, w in (words or load_menu.WORDS).items()}
    return SimpleNamespace(name=name, base=base, nso=SimpleNamespace(build_id=build, source_sha256=sha), word=lambda a: table.get(a, 0))


def expect_error(fn, text):
    try:
        fn()
    except ValueError as e:
        assert text in str(e), e
        return
    raise AssertionError('no error: ' + text)


def generator_checks():
    good = mod(load_menu.BUILD, load_menu.SHA256)
    assert load_menu.verified(good)
    assert not load_menu.verified(mod('0' * 40, load_menu.SHA256))                      # another build: no hooks
    assert not load_menu.verified(mod(load_menu.BUILD, load_menu.SHA256, name='sdk'))   # another module: no hooks
    expect_error(lambda: load_menu.verified(mod(load_menu.BUILD, '0' * 64)), 'identity')
    words = dict(load_menu.WORDS)
    words[load_menu.SAVE_HANDLER] ^= 1
    expect_error(lambda: load_menu.verified(mod(load_menu.BUILD, load_menu.SHA256, words=words)), 'mismatch at main+0x838fd8')
    assert load_menu.entry(good, good.base + load_menu.UPDATE) == 'rt_load_menu_update(c->x[0]);'
    assert load_menu.entry(good, good.base + load_menu.SAVE_HANDLER) == 'if (rt_load_menu_handler(c)) return;'
    assert load_menu.entry(good, good.base + load_menu.LABEL) == 'if (rt_load_menu_label(c)) return;'
    assert load_menu.entry(good, good.base + 0x1000) == ''
    load_menu.validate_patches(SimpleNamespace(sites={'main': {0x1000: 1}}))
    expect_error(lambda: load_menu.validate_patches(SimpleNamespace(sites={'main': {load_menu.UPDATE: 1}})), 'hooked entry')
    print('load menu generator: build and word checks, hook text, patch overlap')


def runtime_checks():
    out = ROOT / 'build' / 'menu' / 'load_menu'
    out.mkdir(parents=True, exist_ok=True)
    for game in ('2.0.0', '1.2.1'):
        exe = out / ('test-' + game)
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-Wall', '-fsanitize=undefined', f'-DTEST_GAME="{game}"', '-Iruntime',
                        'tests/menu/load_menu.c', 'runtime/load_menu.c', '-o', str(exe)], cwd=ROOT, check=True)
        subprocess.run([str(exe)], cwd=ROOT, check=True, env={'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})


if __name__ == '__main__':
    generator_checks()
    runtime_checks()
