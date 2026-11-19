# Save slot pages

The game's Save and Load screens show the auto save and three manual slots. Pages give the player 30 manual slots (10 pages of 3) on those same screens, without changing their layout.

Status: checked on Linux with scripted input and screenshots ([ui-testing.md](ui-testing.md)). Not run on Windows.

## What the player sees

- L and R change the page while the screen is idle (rows loaded, no message open). The page wraps around.
- The screen title ends with the page between the game's own L and R button icons, for example `<title>   [L] Page 2/10 [R]`. The icons are drawn by the game's inline icon text. They follow the Button Icon Type setting (Nintendo, PlayStation, Xbox, Steam, keyboard), also with no controller connected.
- A page change plays the sound the game's own screens play when L or R switches a tab.
- Each manual row shows its slot number (`No. 04`) in the corner where the auto save row has its label. An empty row shows the number after its usual "no save" text.
- The auto save is one slot for every page. Continue on the title screen always loads it.
- The open part of the title bar, past the small slash, shows the page's name. It is `Page 2` until the player gives the page another name.
- Y opens the host's text entry, the one the game's character naming uses. Y is a controller's Y, or the left mouse button, which is Y by default. The keyboard works with a controller. A name has up to 24 characters. Empty text puts `Page N` back.
- The screens open on the page the player last used, also after a relaunch. A new profile starts on page 1, which is the game's own slots 01 to 03.

## Where the names and the last page are kept

Both are in `save-pages.txt` in the profile directory, beside `save/`.

- A name is one `page<TAB>name` line.
- The last page is one `last<TAB>page` line (1-based). It is written on every page change and absent while the page is 1.
- The file is read the first time the screen opens, before the rows load.
- It is not in the save folder. The game's save mount is journaled, and a file the game did not write there is dropped at the next commit.

## How it works

The game keeps asking for `gamedata01` to `gamedata03` and their `.tmb` thumbnails. `runtime/save_pages.c` holds the current page. `host_path_kind` in `runtime/fs_host.c` rewrites those three names for pages after the first. Page 2 is `gamedata04` to `gamedata06`, and page 10 is `gamedata28` to `gamedata30`.

Loading, saving and creating a slot all go through that function, so the game needs no knowledge of pages. Page 1 uses the game's own file names. Existing saves keep working, and a build without the hooks reads the same files.

The screen reads its rows when it opens. On a page change the screen's loading state is started again (`save_pages_reload`). The rows are marked requested and the loading flag is set. The screen's own code then reloads them from the files of the new page and redraws.

The generator emits the hooks for the 2.0.0 `main` only. It checks the build ID, the hash and the instruction words at every site (`recomp/save_pages.py`). Another `main` is lifted without them.

| Site (main offset) | Hook | Use |
| --- | --- | --- |
| `0x6661c0` | `rt_save_pages_pad` | Reads the buttons pressed this frame. L and R change the page, Y renames it. |
| `0x6665c0` (entry) | `rt_save_pages_rows` | Writes the slot numbers on the rows and the page in the title. |
| `0x6660a0` (entry) | `rt_save_pages_update` | Plays the page change sound the frame after the pad hook asked for it. |
| `0x6654a0` (entry) | `rt_save_pages_open` | A screen was built. Looks for its title next. |
| `0x916d44` (entry) | `rt_save_pages_text` | Takes the title widget and the empty-row text when the game sets them. |
| `0x928620` (entry) | `rt_save_pages_request` | Remembers whether the screen asked for is Save (mode 1) or Load (mode 0). The Load screen's title then reads "Load", as the Save screen's reads "Save". |
| `0x55f548` (entry) | `rt_save_pages_label_entry` | Keeps the frame layout's handle from the label setter the title goes through. |

The title and the labels are set with the game's own text functions (`rt_call`), so they use the game's font. They are English in every language. The sound is the one the game's L/R tab screens play. [ui-widgets.md](ui-widgets.md) lists the functions involved.

## The title bar and the page name

The page name is drawn in `rect_TextTitle03`, a text pane of the screen frame that these screens leave empty. `rect_TextTitle02` holds the blank spaces that centre the name in the box right of the slash. `title_label` and `name_spacer` in `runtime/save_pages.c` set both.

The pane has the layout's smaller sub-title style, so `title_label` gives it the title widget's style selector before it sets the name (`match_title_style`). The name is then drawn in the title's typeface and size. Its colour is set with a colour tag, lightblue3, the nearest of the game's colours to the title's. [ui-widgets.md](ui-widgets.md) describes the panes, the selector and the colour tag. The title text is the game's own, apart from the page label and the Load screen, whose title reads "Load" (the game's "Load Game" is longer).

## Limits

- There are 30 manual slots (`SAVE_PAGE_COUNT` in `runtime/save_pages.h`). The file names allow more. The screen shows three at a time, so a higher count only adds pages.
- Each slot is about 0.9 MiB (the save file and its thumbnail), so 30 slots add about 27 MiB.
- A save commit links unchanged files. A commit with 19 files took 51 ms on Linux.
- The screen's own arrays hold four rows. The page window is what makes more slots possible without editing the game's layout.

## Checks

- `tests/save_pages_check.py` runs the page range, the name mapping, the label, the page names (cleaning, limit, file format) and the last page. It needs no game files.
- `python3 recomp/gen.py` stops with a message when an instruction word at a site differs.
- By hand, with a profile that has saves:
  1. Open Load Game on the title screen.
  2. Change pages with L and R, and rename a page with Y.
  3. Save from the in-game menu on a later page, then load a slot from it.
  4. Close the game and start it again. The screen should open on the last page.
- [ui-testing.md](ui-testing.md) shows how the Linux checks were done with scripted presses and screenshots. They covered page changes, rename and the controller icon styles.

The buttons the screen reports (this feature uses Y, L and R), the sound and the text entry are described in [ui-widgets.md](ui-widgets.md). A Y press is reported again on the frame the text entry closes, so `rt_save_pages_pad` ignores Y for 30 hook calls after one.
