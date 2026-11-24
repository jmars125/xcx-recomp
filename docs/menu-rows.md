# The in-game main menu: rows, handlers, lists and text

Reference for changing what the in-game main menu (the list that opens with Plus) shows or does from the hooks in
`recomp/*.py` and the runtime code they call. All offsets are `main` offsets in the 2.0.0 build, found with a
disassembler and checked by running the game. The first use is [menu-load-game.md](menu-load-game.md). Hook rules and
widget lookups are in [ui-widgets.md](ui-widgets.md) where that page exists in your branch.

Status: every statement was checked on Linux with scripted input and screenshots unless it says "read from the code".
Nothing was run on Windows.

## The menu object

`rt_*` functions get the menu object as `controller`. Fields used here (offsets from the object):

| Offset | Meaning |
| --- | --- |
| `+0x98` | the menu's layout view (the object passed to the label code) |
| `+0x10b0` | state of the update state machine (1 build, 2 and 3 browse, 4 open a sub-list, 5 and 6 browse it, 7 and 8 choose and wait, 9 and 0xb leave) |
| `+0x10c0`, `+0x10c8` | the row table (pointer) and its row count, 8 |
| `+0x1108` | the selected main row |
| `+0x1160` | the sub-list's visible rows: indices from `+0x1160`, their count at `+0x1188`, the selected one at `+0x1190` |
| `+0x1198` | the entry of the selected row |
| `+0xda8` | which list is active: 0 main, 1 sub-list |
| `+0x1560`, `+0x1564` | a message id a handler sets to say why a row cannot be chosen; the default label id |

## Row tables

Rows are 0x30-byte entries in function-local statics built by `0x836368` on first use. The main table is at
`0x2763140`, eight rows.

| Offset | Meaning |
| --- | --- |
| `+0x00`, `+0x08` | handler: a member function pointer, function and `this` adjustment (the adjustment is stored shifted left by one; bit 0 set means a virtual call) |
| `+0x10` | message id of the row's label (16 bits) |
| `+0x18`, `+0x20` | the child rows: pointer and count; zero for a leaf row |
| `+0x28` | flag byte: 1 for the child rows seen, 0 for rows with children; read when a row is chosen |

The eight main rows, with the number of children:

| Row | Children | Notes |
| --- | --- | --- |
| Party | 9 | 7 shown on a new save; the two Skell rows are hidden until the game's Skell flag (see below) is set |
| Missions, Map, Active Members | none | open their screen |
| Intel | 6 | |
| Online | 5 | the mod menu appends a sixth row when it is built in and a page is registered |
| Save | none (see menu-load-game.md: it becomes the Data row with two children) | opens the Save screen |
| System | 6 | has the red new-item dot |

Every sub-list shares one layout and has nine slots. The layout has exactly eight main rows and nine sub rows
(`mc_MenuTitle01..08`, `mc_SubMenuTitle01..09`; probing 09 or higher on the main list finds nothing). There is no way to
add a ninth main row from a hook: the panes are layout data, and no call that creates or moves a pane was found. The
table code builds only two levels: the list builder takes level 0 or 1 and does nothing for another value, so a child
row's own child list is never opened. A third level would mean refilling the sub-list from a hook when a folder-like row is
chosen and handling Back; that was not tried.

## Handlers

A handler is `bool handler(menu, op)`. The list builder and the state machine ask it questions by op:

| Op | Seen as |
| --- | --- |
| 0 | is the row shown at all (false: the row is skipped when the list is built) |
| 1 | can the row be chosen (false: the handler stores a message id at `+0x1560` and the menu shows it) |
| 2, 7 | nothing for the rows seen |
| 3 | returns false for the Save row, true for the plain parent handler's rows except itself; not needed for a new row |
| 4 | has the screen the row opened ended (`!busy` of the scene manager's request) |
| 5 | use the row's own message id (true) or the default id (false) |
| 6 | choose: do what the row does |

Plain parent handlers (`0x838fc0`, `0x838fcc`) answer every op true except 3. Most rows' handlers are jump tables over the
op; a hook on them has to be an entry hook, and `recomp/gen.py` raises if the function takes the all-labels path.

The two hidden Party rows share one condition: both answer op 0 with the same game flag (`0x42e`, read through
`0xc89f0`). They are the Skell gear page (between Fashion Gear and Arts) and a Return to Skell action (after Status).
Forcing the answer true showed them with the greyed look. What sets the flag was not traced.

## Texts

Labels are message ids looked up in message table `0x100`: `table = 0x34bbb0(0x100)`, `text = 0x3de0e4(table, id)`
(a UTF-8 pointer). Scanning ids 0 to 0x3000 this way lists the game's own wording. Findings that matter for the menu:
the game has a "Save", a "Load Game" (the title's and the Load screen's title), and a combined "Save and Load" title, but no
plain "Data" or "Load" (menu-load-game.md says how the two labels are written). `0x917160(widget, {table, id})` sets a label from a table and an id; `0x91720c` sets one from a
raw UTF-8 string (the mod menu uses it).

## Save and Load requests

The screens for Save and Load are one state. A row asks for it with `0x928620(manager, mode, a, b)`: mode 1 is Save (the
Save row's handler passes `1, 1, 0` through `0x40dc7c`), mode 0 is Load (the title passes `0, 1, 0`). The manager is the
global behind the GOT slot at `0x1d54e40`. While the screen is up the request is busy (`0x9286b8`); afterwards the last
result is the word at `0x271fFa0` (`0x9286e8`): positive for done (it was the position of the chosen slot in the list), -1 for cancelled. The title starts the
loaded game by running the field script `fld_continue` (`0x12ebe4`); the menu can do the same from the field and the
game restarts from the slot.

## Testing a menu change

Run the game on a copy of a test profile, never the real one. `XCX_INPUT_SCRIPT=<file>` replays buttons (lines of
`tick BUTTON frames`, 60 Hz ticks; start-up is slow, so the first presses need generous gaps); `XCX_SHOTS=<from>-<to>:<step>`
with `XCX_SHOTS_DIR` writes screenshots (480x270); `XCX_FPS_LIMIT=60` keeps the ticks steady. A throwaway entry hook that
logs `x0`, `x1` and `x30` shows which function asks what, and an entry hook on `0x839910` can call the text lookup above
for a list of ids. Keep such hooks out of the tree.
