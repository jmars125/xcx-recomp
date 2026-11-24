# Changing a game screen's text and menus from a hook

Reference for finding and changing text on a game screen from the hooks in `recomp/*.py` and the runtime code they call. It also covers menu sounds, the buttons a screen reports and the host's text entry. All offsets are `main` offsets (from `main+0x0`) in the 2.0.0 build. They were found while adding [save slot pages](save-slots.md). [ui-testing.md](ui-testing.md) shows how to drive the game and look at the result.

Status: everything here was checked on Linux with scripted input and screenshots. Nothing was run on Windows. A topic this page does not cover gets its own section here, or its own doc if it is a separate feature.

## Hook rules

- The generator emits a hook only when the build ID, the hash and the instruction words at every site match. Another `main` is lifted without the hooks.
- An entry hook sits on the first instruction of a function. It may call guest functions with `rt_call(c, fn, n, ...)`.
- A hook in the middle of a function may only read and write memory or values. The function's registers live in C locals there, so a call would use stale registers.
- `rt_call` passes integer registers only. Float arguments go in `c->v[i]` (`__uint128_t v[32]`) before the call.
- A hook that calls the guest saves `c->x[0..18]` first and restores them after. The interrupted function then keeps its own registers.
- Guest addresses are host pointers, so `LD8/16/32/64` read them. A module offset is added to `MAIN = 0x7000000000`.

## Finding a widget

A layout handle is a pair: the layout object and a part index. The Save and Load screen keeps one per row at `scene+0xd8+0x10*row`.

| Function | Use |
| --- | --- |
| `main+0x55f16c` | Finds a child of a layout handle by name. |
| `main+0x915d00` | Called by find-child. The name is a `/` separated path into the layout's pane tree. |
| `main+0x915110` | Casts a pane to a text pane. Returns 0 if it is not one. |
| `main+0x91720c` | Sets a text pane's text from UTF-8. |
| virtual at `+0x20` of a pane | Shows or hides the pane. |
| `main+0x55f548` | Sets a label by message id: `(handle, child name, message)`. Screen titles are set through it. |

The entry of `main+0x55f548` receives the layout handle as its first argument. Hooking it gives the handle of a screen's frame without reading the screen's own fields.

To list the panes a layout has, call find-child in a loop over the `rect_` strings in the main module's rodata. The names that answer exist in that layout.

## Setting text

- Text set through `main+0x91720c` or by message id goes through the game's own text layouter. It uses the game's font and understands the tags below.
- `[ST:icon p1=l]` is a controller button icon. It becomes `[ML:icon ...]` before drawing and follows the Button Icon Type setting, also with no controller connected. The route is described in [glyph-inline-text-route.md](glyph-inline-text-route.md) and [glyph-inline-trial.md](glyph-inline-trial.md). Use the tag in place of a button name.
- Other tags in the game's text: `ST:col` (becomes `System:Color`), `ST:space`, `ST:wide`, `ST:wait`, `ST:page`, `ST:undisp`, `ST:ruby`, `ST:playername`, `ST:gender`.
- Text typed by a player has `[` and `]` removed before it is set. A name then cannot start a tag.
- A text pane squeezes or clips its text to its own width. The layout fixes its size, weight and colour (see "What did not work").
- The game's own labels are English in every language here.

### The title bar of the Save and Load screens

The frame layout has a title pane, `rect_TextTitle`. It squeezes its text to its width, which ends at the small slash. The layout also has two panes these screens leave empty:

- `rect_TextTitle02` sits lower than the title line.
- `rect_TextTitle03` starts where the text of `02` ends.

Text in `03` appears in the empty box right of the slash, at the title's height. Blank spaces in `02` move it to the right. The panes were found by looking up about 1000 pane names from the rodata in the frame's layout.

- With no spaces, the text in `03` starts at pixel 355 (at 1920 wide).
- One space is about 9.1 px. This was measured from screenshots with 20 and 40 spaces.
- The centre of the box is about pixel 1018.
- The font is proportional, so a text width is an estimate. `name_spacer` in `runtime/save_pages.c` adds a width per kind of character (narrow, normal, wide, full-width) and picks the blanks that put the text's middle at the box's middle. It is good to within a few characters.
- The panes use the layout's sub-title text style, which is smaller and grayer than the title.

### Changing a text pane's typeface and size

A text pane's widget holds a style selector that the text setter (`main+0x91720c`, then `main+0x916d44`) reads: a flag byte at `+0x58`, a kind byte at `+0x59` and a style id at `+0x5a` (`+0x5c` is the size the style gave it, a record only). The style id picks an entry of the game's font style table, which sets typeface and size. The title pane has id `0x14` and the name pane `0x1c`. Writing the title widget's three fields into the name pane's before setting its text draws the name in the title's typeface and size. `match_title_style` in `runtime/save_pages.c` does this. The game's own routine that applies a style descriptor to a pane (`main+0x920134`) writes the same fields.

The widths of the larger text were measured with 20 repeated letters in both styles (3440x1440 window): m 548 and 762 px, W 594 and 766, A 419 and 631, n 393 and 590, 0 437 and 657, ten n separated by spaces 293 and 441.

### Colour in text

The colour comes from a tag in the text, not from the selector. `[ST:col p1=<name>]` becomes `[System:Color p1=<name>]` before drawing, and the game looks `<name>` up in a table of colour names. The parameter name does not matter. A number does not work, which is why the values 0 to 9 tried before changed nothing. The table, read from the game's memory, has black, white, red, green, blue, yellow, gray, lightblue, orange, lightblue2 and lightblue3. A colour replaces the pane's own tint: white gives a plain grey, not the title's blue-grey.

Measured on screen with the title at RGB (181, 209, 214) and the name pane's default at about three quarters of that: lightblue3 gave (171, 194, 217), the nearest. `runtime/save_pages.c` puts `[ST:col p1=lightblue3]` in front of the name. A name typed by a player has `[` and `]` removed, so it cannot carry a tag.

These were also tried for the colour and changed nothing: the text object's words at `+0x3e8` and `+0x3ec` (equal on both panes), the float at `+0x3fc`, the widget's `+0xd8` and the second style pair at `+0x118`, and each of the floats from `+0x1c0` to `+0x264` that differ between the two text objects. The layout function that scales a pane's value (`main+0x55fd30`) refuses text panes.

## Playing a menu sound

`main+0x208554` returns the sound manager. `main+0x40bc48(manager, id, 0)` plays an effect. Effect 5 is the sound the game's own screens play when L or R switches a tab or a page.

Call it from an entry hook. `save_pages.c` notes the request in the pad hook and plays the sound from the screen's update entry on the next frame.

## Buttons a screen reports

A screen's update receives a mask of the buttons pressed this frame. On the Save and Load screen the bits are Y `0x01`, L `0x10`, R `0x20`, ZL `0x40`, ZR `0x80` and Minus `0x200`. They do not follow the Switch's own order.

The bits were found by scripting one press per button and logging the mask with a temporary line in the hook ([ui-testing.md](ui-testing.md)). Another screen may report different bits, so check each one the same way.

A press that opens a text entry is reported again on the frame the entry closes. `rename_cooldown` in `save_pages.c` ignores Y for 30 hook calls after an entry.

## Asking the player for text

`text_entry_run` (`runtime/swkbd.h`) shows the host's text entry, the one the game's character naming uses. Its keyboard works with a controller ([naming-and-controller-keyboard.md](naming-and-controller-keyboard.md)).

A guest thread gives up the virtual core and blocks the game's input while it waits: `kern_core_away`, `input_block(1)`, then `input_block(0)` and `kern_core_back`. `rename_page` in `save_pages.c` does this. `swkbd_utf8_to_utf16` and `swkbd_utf16_to_utf8` convert the text.

For a test without a window, `XCX_SWKBD_TEST=type:<text>` types the text.

Settings the test tools take from the environment (`XCX_*`) are read with `cfg_get` from `runtime/cfg.h`. `getenv` does not see them.

## What did not work

All of this was tried on the Save and Load title, to make the page name look like the title.

- **Writing into the title widget's fields.** The fields near `+0x38` are part of a pointer, and the write crashed the game. The size-like values at `+0xf8`, `+0x1b8` and `+0x278` are measured text widths. No field is a settable pane width.
- **Resizing the title pane.** The widget behind `rect_TextTitle` is an animation and state wrapper (first words of its table at `main+0x1c8e4b8`). A move or resize needs the layout's own calls.
- **Colour tags with numbers.** `[ST:col ...]` with the parameter names `p1`, `ColorID`, `id` and `name` and values 0 to 9 changed nothing, because the value has to be a colour name. See "Colour in text".
- **The scale function.** `main+0x917448(widget, s0, s1)` with the vector registers set to 1.0 and 1.35 changed nothing.
- **Putting the name in the title's own text.** The title's text object (`[widget+0x38]`) has an overflow mode at `+0x3f0`, set with `main+0xa1d204(widget+0x38, mode)`. The game uses mode 3, which shrinks the whole text to the pane's width. Mode 2 also shrinks it. Modes 0, 4 and 5 clip the text at the pane's right edge. Mode 1 draws it small and high. The text object's width field at `+0x3da` reads 172, the pane's width. Writing 900 to it, before or after setting the text, changed nothing, because the clip comes from the layout done when the text is set.
- **Copying style fields of the text object.** Copying the colour-like floats, the effect values, the style ids and the glyph block from the title's text object to the name pane's changed nothing on screen. Copying the structural fields (`+0xd4`, `+0xf0`, `+0x1e4`) crashed the game. The style selector is on the pane's widget, not on its text object: see "Changing a text pane's typeface and size".
- **A font name.** No font family name, Futura or any other, could be read from the game's data. Only `MNU_Font*`, `MNU_ResFontStyle` and `MNU_FONTICON` strings turned up. The title and the name pane both use the game's menu font. They differ in size, weight and colour, which the layout sets per pane; the style selector sets typeface and size, and a colour tag sets the colour.
- **Mouse clicks on the title.** The cursor is captured and hidden on this screen, and the left click is already Y by default. Y covers the click.
