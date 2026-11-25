# Data row with Save and Load in the in-game menu

The in-game main menu has no way to load a save: Load Game lives only on the title screen. This feature turns the menu's
Save row into a Data row. It opens a list of two rows: Save, which opens the game's Save screen exactly as before, and
Load, which opens the game's own Load screen. Confirming a slot there loads that save into the running game. The sub-list
has nine slots, so seven are left for more Data options.

Status: built and checked on Linux (RADV) with scripted input and screenshots, against the 2.0.0 `main`, on a copy of a
test profile. Nothing was run on Windows. What was seen, all with the game's own widgets, cursor, sounds and button
icons:

- the menu keeps its eight rows in their order; System keeps its red new-item dot; Data opens a two-row list;
- Save opens the Save screen, "Save to this slot?" appears for a slot, backing out returns to the list;
- Load opens the Load screen; Confirm on a slot of another place (the Chapter 3 slot in the BLADE Barracks, from a
  field in the first area) fades to the loading screen and then plays that save's place and party; the clock runs on;
- backing out of the slot list, or choosing Cancel on "Load this save data?", returns to the list and starts nothing.

Also checked on Linux, the same way:

- a build with `--mod-menu-hook`: the menu, Data and the Save screen behave the same; Online keeps its five rows
  because no page is registered, so the mod menu's own row was not seen;
- the save slot pages of PR #204 merged into a scratch copy: Load opens that branch's paged Load screen and a slot
  loads the same way. Both branches add a line to the same places in `recomp/gen.py`, so that merge needs a hand: keep both.

Not checked, and worth checking on Windows: the same menu walk with a keyboard and with a controller; loading from the
title screen still working (the code does not touch it); the three button icon styles in the Load screen footer (the
footer is the game's own screen, not drawn here); loading a save from a chapter far from the one being played, with a long
play time; the mod menu's own row together with the Data row.

## What the hooks do

Generated code gets three entry hooks, only for the 2.0.0 `main` and only after the build ID, the hash and the first
instruction of each function match (`recomp/load_menu.py`). Any other build is lifted without them and keeps the game's
menu. The runtime part is `runtime/load_menu.c`; row table, handlers and the list builder are described in
[menu-rows.md](menu-rows.md).

| Site (`main` offset) | Hook | What it does |
| --- | --- | --- |
| `0x839910`, the menu's per-frame update | `rt_load_menu_update` | Before the first list is built it changes the Save row of the row table (see below). A table that is not the game's is left alone. |
| `0x838fd8`, the Save row's handler | `rt_load_menu_handler` | Answers for the Load row (see below); calls with the real menu object pass through. |
| `0x66bee8`, the menu's label setter | `rt_load_menu_label` | Writes the text of the Data and Load rows (see Labels); every other row is left to the game's code. |

The row table is the game's own data, so the change is made in memory, never in a file:

1. The Save row becomes a parent row. Its handler is the plain parent handler the Intel row also uses, its flag byte 0, its
   child list two rows.
2. Child 0 is a byte-for-byte copy of the original Save row, so Save behaves as before, with the game's own label.
3. Child 1 is the same row with a `this` adjustment of 1. A real menu object is always aligned, so the hook sees the
   marker in the object address and knows the call is for the Load row.

For the Load row the handler hook answers op 6 (choose) by asking the game for the Save and Load screen in load
mode: the same request the title's Load Game makes (`0x928620`, mode byte 0), made with the scene manager the Save row's
own handler uses. It answers op 4 (has the request ended?) by asking the original code, and when the request has ended with a
positive result it starts the field script the title's Continue starts (`fld_continue`, through
`0x12ebe4`). A cancelled request ends with -1 and starts nothing. Every other op goes to the original code with the real
object, so the row greys out and shows "cannot do that now" messages exactly like the Save row.

## Labels

The Save child keeps the game's own "Save" label. The game has no message that reads only "Data" or only "Load" (its
nearest are a combined save and load title and the title's Load Game), so these two labels are our own text, chosen with the owner.
The two rows keep message ids that no other row of the menu uses (the game's combined title and "Load Game", which only
mark the rows), and the label hook shows the row and sets the text through the game's own text pane (`0x560464`, `0x91720c`),
so the font, size and colour are the game's. The two texts are in English only, like every label here
([ui-widgets.md](ui-widgets.md) where that page exists). Nothing is drawn by the host.

## Limits

- The main list has exactly eight rows (layout panes, view slots, per-row arrays and the builder's capacity); this
  feature keeps eight and adds a second level to a row that had none. Every sub-list has nine slots: Data uses two.
- The loaded save replaces the running game through `fld_continue`, as on the title. Anything the title does before its
  Continue (for example its own fade) is not repeated here; the game's loading screen shows instead.
- A load ended with a positive result in every test (4 for the fourth slot in the list, 1 for the first, which fits
  "the position of the chosen slot"); the code treats every positive result as a load and a negative one as cancelled.

## Tests

- `python tests/menu/load_menu_check.py`: the generator's site and word checks, and the runtime on synthetic memory (the
  row table, the marker, the request, `fld_continue` only after a positive result, another game version left alone).
  Linux only: it maps memory at the game's addresses.
- Scripted run on a copy of a test profile, with screenshots: [menu-rows.md](menu-rows.md#testing-a-menu-change).

Note: the mod menu and this feature change the same row table, and both hook the label setter (`0x66bee8`). The mod menu copies
the eight rows into a block of its own; this code changes the table the menu points at when the first frame runs, whichever
of the two it is, and its label hook answers first and only for its two rows, so the order does not matter. The Online
sub-list has the free slots the mod menu's row needs; whether a row for it belongs in Online or System is a mod menu matter.
