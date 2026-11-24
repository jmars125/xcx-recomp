# Testing UI changes by script and screenshot

A change to a game screen can be checked without touching the controller. Replay presses with `XCX_INPUT_SCRIPT`, save pictures of the window with `XCX_SHOTS`, and look at them.

The save slot pages ([save-slots.md](save-slots.md)) were checked this way on Linux: page changes, the page name, the text entry and the controller icon styles. Nothing was run on Windows. [ui-widgets.md](ui-widgets.md) is the reference for what a hook can change on a screen.

Use a disposable profile (a copy of saves in a folder of its own, passed with `--profile`), never the real one, and run one instance at a time: a second game on the machine slows the first and moves the scripted timing.

## Pressing buttons

`XCX_INPUT_SCRIPT=<file>` replays presses; the format and the timing rules are in [controls.md](controls.md#scripted-input). The points that matter for a UI check:

- A line is `<tick> <BUTTON> [hold]`, ticks at 60 Hz of game time. The title menu needs `RIGHT` and `A` to reach Load Game, for example `600 RIGHT`, `660 A`, then the presses for the screen under test a few hundred ticks later. Leave generous gaps: a slow machine loads late, and a press before the screen is ready is lost.
- Pressing a button the screen does not use is harmless. This is how to find the bits a screen reports:
  1. Temporarily log the pressed-button mask in the hook.
  2. Script `Y`, `X`, `ZL`, `ZR`, `PLUS` and `MINUS` one after another.
  3. Read the value each one gives.

  The bits do not follow the Switch's own order. On the Save and Load screen Y is `0x01`.
- The scripted input is ORed with real input, so leave the controller alone during a run.

## Taking pictures

`XCX_SHOTS=<lo>-<hi>:<step>` saves every `<step>`th presented frame in `[lo, hi]` as `f<n>.png` (480x270 by default; `XCX_SHOTS_FULL=1` keeps the window size) in `XCX_SHOTS_DIR` (default: the current directory). `n` counts presented frames, not ticks. At `fps_limit = 120`, frame `n` is about `2 * tick - 520` for a script started from the title screen. A press at tick 1100 lands near frame 1700, so take the pictures a few hundred frames after it. `w<lo>-<hi>:<step>` counts world-clock seconds instead.

A picture of a small part of the screen (the title bar, a row) is easier to judge when cropped to it; the file is a normal PNG. The host's text entry has its own test hook: `XCX_SWKBD_TEST=type:<text>` types the text 20 frames after it opens and presses OK at frame 40 (`ok`, `cancel` press only), and writes the entry's own picture as `swkbd-test-<frame>.png` in the game folder.

## Other settings that help

- `XCX_FPS_LIMIT`, `window_size` and the controller icon settings are read from `xcx.ini` next to the executable and from `XCX_*` variables. To check the icon styles, set `glyph_style` to `playstation`, `xbox`, `steam` or `original` and restart; with no controller connected the forced styles still apply.
- Settings read through `XCX_*` names must be read with `cfg_get` in code (`runtime/cfg.h`), not `getenv`: the second does not see them in this build.
- `XCX_TRACE=fs` logs the game's file accesses, which shows which `gamedataNN` files a screen opens.
- A rebuild of the runtime alone (no new hook) is quick: `tools/build_game.py` with the same arguments, no `--full-build`, reuses the generated C. A new or changed hook in `recomp/` needs `recomp/gen.py` first, which only rewrites the chunks that changed.

## What to look for

- The thing itself, then the neighbours: page 1 and a later page, an empty row, a long text, a language with another text width.
- That the game still works around it: back out of the screen, open it again, load a slot.
- The log for lines of the feature (`save pages: ...`, `swkbd: ...`) and for a crash report (`crash.txt` beside the executable).
