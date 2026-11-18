# Command line

For launchers, scripts and shortcuts: what `xcx.exe` (`xcx` on Linux) and XCX-Setup take on their command lines.

## The game

```
xcx.exe [--set KEY=VALUE]... [--fullscreen | --windowed] [--exefs DIR] [--romfs DIR] [--profile DIR] [mode]
```

| Option | What it does |
| --- | --- |
| `--set KEY=VALUE` | One setting for this run. `KEY` is a key of `xcx.ini` ([xcx.ini.example](../xcx.ini.example) lists them), any case, with or without `XCX_`; `--set=KEY=VALUE` works too. Repeatable. |
| `--fullscreen` | `--set FULLSCREEN=1`: borderless fullscreen on the window's monitor. |
| `--windowed` | `--set FULLSCREEN=0`. |
| `--exefs DIR`, `--romfs DIR` | The game's ExeFS and RomFS (default: `exefs/` and `romfs/` beside the exe, else the paths the build baked in). |
| `--profile DIR` | The save folder (default: `profile/` beside the exe). |
| `--help` | The list of options and modes, exit code 0. |

With no mode the game starts. The modes (`--seconds S [--shot PNG]`, `--boot`, `--selftest`, the `--probe-*` checks,
`--prepare-shaders`, `--warm-shaders`) are for tests and the setup; `--help` lists them.

### Which value wins

For every setting, first match wins:

1. `--set` (and `--fullscreen` / `--windowed`) or the environment variable `XCX_<KEY>`. They are the same thing:
   `--set` sets that environment variable before anything reads a setting (`runtime/cfg.c` `cfg_args`), so a setting
   given both ways takes the later one, which is `--set`.
2. `xcx.ini` beside the exe.
3. The device profile (the Steam Deck, [tester-guide.md](tester-guide.md)).
4. The setting's own default.

Like an environment variable, a setting given on the command line is greyed out in the in-game menu for that run and
is not written to `xcx.ini`. The log names each one (`cfg: XCX_FPS_LIMIT=30 from the command line`).

`--set` stops the game with exit code 2 and one line on stderr for a key the game does not know, a missing `=` or an
empty value. It checks the key only; a value the setting cannot use is handled as when `xcx.ini` holds it.

Examples:

```
xcx.exe --fullscreen --set fps_limit=40 --set vk_present=fifo
xcx --windowed --set window_size=1280x800 --profile "/run/media/deck/sd/xcx saves"
```

## XCX-Setup

`XCX-Setup.exe` (Windows), `XCX-Setup-<version>-linux.AppImage` or `xcx-setup` in the unpacked Linux tarball:

```
XCX-Setup [--dest DIR] [--play] [--lang en|ru] [GAME ARGUMENTS...] [-- GAME ARGUMENTS...]
```

| Option | What it does |
| --- | --- |
| `--dest DIR` | The install folder to show or play. Without it Setup looks for one (below). |
| `--play` | Start the game, without a window, when the folder holds a finished build of this Setup's release; otherwise open the window on that folder and start the game when the setup has finished. |
| `--lang en\|ru` | The window's language. |
| anything else | Goes to the game when Setup starts it, as do all arguments after a lone `--`. |

When Setup starts the game (Play, `--play`) it waits for it and ends with its exit code, so a launcher that tracks
Setup's process sees the game's lifetime. On Windows `XCX-Setup.exe` waits for the window in turn.

**Play screen.** Setup opens on a Play screen instead of the form when it finds a finished build of its own release:
**Play** starts the game and hides the window, **Update / reinstall** opens the form for that folder. A folder built by
another release opens the form on that folder, ready to update. Setup looks in the `--dest` folder, else in the
install folder the window last set up (`setup.json`, [installer.md](installer.md), "Updating the port": per user on
Windows, in the tarball's own folder on Linux, none for the AppImage unless `XCX_SETUP_RECENT` names a file), in
Setup's own folder (for the AppImage, the folder that holds the `.AppImage` file) and in
the folders right inside it. A build counts as finished when `xcx-install.json` records that a run of this release
ended without error (`complete`, `installer/runner.py`) and the game exe is there (`installer/play.py`).

Examples:

```
XCX-Setup.exe --play --fullscreen
./XCX-Setup-<version>-linux.AppImage --play --dest ~/Games/XCX -- --set fps_limit=40
./xcx-setup -m installer --dest ~/Games/XCX --lang auto --play --fullscreen
```

The last one is the installer's command line ([installer.md](installer.md), "Command line") with `--play`: it runs the
setup steps when the folder needs them, prints their events, and then starts the game. A folder that already holds a
finished build, given no game files, starts at once.
