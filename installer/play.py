"""Play from Setup: find a complete build of this release and start the game.

A folder is playable when its xcx-install.json says a run of this very release finished (`complete`, which
runner.run_steps records at the end of a successful run and drops before a step that changes the folder) and the
game exe is there. An install by another release is not: Setup offers to update it instead.

Where Setup looks, in order: the folder given (--dest), else the install folder the window last set up (installer/
recent.py, which the caller reads, bounded), Setup's folder itself and the folders right inside it. For the AppImage,
which remembers no folder by default, Setup's folder is the one that holds the .AppImage file.

The game gets the arguments Setup did not know (gui.py, cli.py --play), so `XCX-Setup --play --fullscreen` starts
`xcx --fullscreen`, and Setup waits for it: a launcher that tracks Setup's process sees the game's lifetime and exit code.
"""
import os
import subprocess
import sys
from pathlib import Path

from . import state as statefile
from .events import GAME_EXE

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


def candidates(layout, dest=None, last=None):
    """The folders to look in, in order, without repeats; last: the remembered install folder."""
    if dest:
        return [Path(dest)]
    found = [Path(last)] if last else []
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


def find(layout, dest=None, last=None):
    """(playable, known): the first folder holding a finished build of this release, and the first holding any
    install (for the form to update). Either is None."""
    play = known = None
    for p in candidates(layout, dest, last):
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
