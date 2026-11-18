"""Play from Setup: find a complete build of this release and start the game.

A folder is playable when its xcx-install.json says a run of this very release finished (`complete`, which
runner.run_steps records at the end of a successful run and drops before a step that changes the folder) and the
game exe is there. An install by another release is not: Setup offers to update it instead.

Where Setup looks, in order: the folder given (--dest), else the install folder it last finished (remembered in
xcx-setup.json in Setup's own folder), Setup's folder itself and the folders right inside it. The AppImage keeps no
memory (its folder is a read-only mount, and nothing is written outside the install folder on Linux); it looks in the
folder that holds the .AppImage file and the folders right inside that.

The game gets the arguments Setup did not know (gui.py, cli.py --play), so `XCX-Setup --play --fullscreen` starts
`xcx --fullscreen`, and Setup waits for it: a launcher that tracks Setup's process sees the game's lifetime and exit code.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

from . import state as statefile
from .events import GAME_EXE

MEMORY = 'xcx-setup.json'
# Set by Setup's own start (AppRun, the AppImage runtime, Python): not the game's business
SETUP_ENV = ('TCL_LIBRARY', 'TK_LIBRARY', 'PYTHONDONTWRITEBYTECODE', 'PYTHONIOENCODING', 'PYTHONUTF8',
             'PYTHONUNBUFFERED', 'PYTHONHOME', 'PYTHONPATH', 'APPDIR', 'APPIMAGE', 'ARGV0', 'OWD')


def playable(dest, release):
    """True when dest holds a finished build of release. Never raises."""
    try:
        dest = Path(dest)
        state = statefile.load(dest)
    except (statefile.StateError, OSError, ValueError):
        return False
    return bool(state) and state.get('complete') == release and (dest / GAME_EXE).is_file()


def installed(dest):
    """True when dest holds an install of any release (xcx-install.json). Never raises."""
    try:
        return (Path(dest) / statefile.NAME).is_file()
    except (OSError, ValueError):
        return False


def appimage():
    """The .AppImage file Setup runs from, or None."""
    path = os.environ.get('APPIMAGE')
    return Path(path) if path else None


def setup_folder(layout):
    """The folder the player sees Setup in: the AppImage's folder, else Setup's own."""
    image = appimage()
    return image.parent if image else layout.root


def memory_path(layout):
    """Where Setup remembers the last install folder; None where it keeps nothing (the AppImage, a repository)."""
    if appimage() or layout.dev:
        return None
    return layout.root / MEMORY


def remembered(layout):
    path = memory_path(layout)
    try:
        dest = json.loads(path.read_text(encoding='utf-8')).get('dest') if path and path.is_file() else None
    except (OSError, ValueError, AttributeError):
        return None
    return Path(dest) if isinstance(dest, str) and dest else None


def remember(layout, dest):
    """Record dest as the last finished install folder. Best effort: a read-only Setup folder keeps nothing."""
    path = memory_path(layout)
    if not path:
        return
    try:
        tmp = path.with_name(MEMORY + '.tmp')
        tmp.write_text(json.dumps({'dest': str(Path(dest).resolve())}) + '\n', encoding='utf-8')
        os.replace(tmp, path)
    except OSError:
        pass


def candidates(layout, dest=None):
    """The folders to look in, in order, without repeats."""
    if dest:
        return [Path(dest)]
    found = []
    last = remembered(layout)
    if last:
        found.append(last)
    home = setup_folder(layout)
    found.append(home)
    try:
        found += sorted(p for p in home.iterdir() if p.is_dir() and (p / statefile.NAME).is_file())
    except OSError:
        pass
    seen, out = set(), []
    for p in found:
        key = os.path.normcase(str(p.resolve()))
        if key not in seen:
            seen.add(key)
            out.append(p)
    return out


def find(layout, dest=None):
    """(playable, known): the first folder holding a finished build of this release, and the first holding any
    install (for the form to update). Either is None."""
    play = known = None
    for p in candidates(layout, dest):
        if play is None and playable(p, layout.release):
            play = p
        if known is None and installed(p):
            known = p
    return play, known


def game_command(dest, args=()):
    return [str(Path(dest) / GAME_EXE), *args]


def game_env():
    return {k: v for k, v in os.environ.items() if k not in SETUP_ENV}


def start(dest, args=()):
    """Start the game in dest with args; returns the Popen. Its working folder is the install folder."""
    return subprocess.Popen(game_command(dest, args), cwd=str(dest), env=game_env(), stdin=subprocess.DEVNULL,
                            close_fds=True)


def run(dest, args=()):
    """Start the game and wait for it; its exit code (127 when it could not start)."""
    try:
        proc = start(dest, args)
    except OSError as e:
        print(f'cannot start {Path(dest) / GAME_EXE}: {e}', file=sys.stderr)
        return 127
    try:
        return proc.wait()
    except KeyboardInterrupt:
        return proc.wait()
