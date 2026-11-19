// Save slot pages. The game's Save/Load screen shows the auto save and three manual slots. Each page keeps those three
// rows and stands for the next three slot files: page 1 is gamedata01..03, page 2 is gamedata04..06, and so on. The
// game keeps asking for gamedata01..03; fs_host.c maps those names (and their .tmb thumbnails) to the current page's
// files, so the screen, loading and saving all work on the page without knowing about it.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SAVE_PAGE_SLOTS 3    // manual slots the screen shows
#define SAVE_PAGE_COUNT 10   // pages, 30 manual slots in all

int save_pages_current(void);         // 0-based
int save_pages_set(int page);         // clamped to the range; returns 1 when the page changed
int save_pages_step(int delta);       // moves by delta pages with wrap-around; returns 1 when the page changed
void save_pages_label(char *out, size_t cap);   // "Page 2/10"
// A save path below the mount ("/sts/game/gamedata02" or "...gamedata02.tmb") on a page after the first becomes the
// same name for that page's slot; returns 1 and fills out when it did. Other paths are left to the caller.
int save_pages_remap_path(const char *rest, char *out, size_t cap);

// Page names. A page can carry a name the player gave it ("Mods run", a character's name); a page without one is called
// "Page N". Names are kept in the profile directory (save-pages.txt, one "page<TAB>name" line each; not in the game's
// journaled save, which only keeps what the game itself writes) and are UTF-8, at most SAVE_PAGE_NAME_MAX characters.
// The same file keeps a "last<TAB>page" line, the page the screen was left on, which the next launch opens on.
#define SAVE_PAGE_NAME_MAX 24
void save_pages_name(int page, char *out, size_t cap);        // the page's name, else "Page N" (0-based page)
// Sets the page's name from UTF-8 text: control characters and the brackets of text tags are dropped, the ends trimmed,
// the length cut at SAVE_PAGE_NAME_MAX characters. Empty text takes the name away. Returns 1 when the name changed.
int save_pages_set_name(int page, const char *utf8);
int save_pages_has_name(int page);
void save_pages_names_parse(const char *text);                // the file's content
size_t save_pages_names_format(char *out, size_t cap);        // the file's content for the names that are set

// The Save/Load screen's object (main+0x6654a0 builds it) loads its rows at start: it sets a loading flag, marks every
// row "requested" and walks them one at a time until each is loaded, then raises a ready bit. Calling this on the
// screen's own update thread starts that sequence again, so the rows show the files of the page that is now current.
void save_pages_reload(uint64_t scene);

// Called from the Save/Load screen's update (recomp/save_pages.py) while its rows are loaded and no message is open;
// scene is the screen object and pad the buttons that went down this frame, in the game's bit layout.
void rt_save_pages_pad(uint64_t scene, uint32_t pad);
// Y (also the left mouse button by default) on the screen asks for a new name for the current page: the host's text
// entry (swkbd.h text_entry_run, the one the game's character naming uses) opens over the game, and the title shows the
// name once it is confirmed. Controllers use that entry's own keyboard.

// Called at the start of the function that fills the rows' widgets, before it runs (c->x[0] is the screen object): puts
// the slot number on each manual row. Leaves every guest register as it found it.
struct Cpu;
void rt_save_pages_rows(struct Cpu *c);

// The screen's title is set by the screen frame, not by the screen's own code. rt_save_pages_open runs
// when a screen is built and arms the capture; rt_save_pages_text runs at every text widget string set and takes the
// first label the frame sets after that as the title widget, remembering its text. rt_save_pages_rows then appends
// the page ("<title>   Page 2/10") each time the rows are filled. The labels this adds ("Page", "No.") are English in
// every language.
void rt_save_pages_open(void);
void rt_save_pages_text(struct Cpu *c);
void rt_save_pages_request(uint32_t mode);   // main+0x928620: the screen is asked for, mode 1 is Save and 0 is Load
void rt_save_pages_label_entry(struct Cpu *c);   // main+0x55f548: remembers which layout handle and child name it was given

// Called at the start of the screen's update, once per frame (c->x[0] is the screen object): plays the page change
// sound the frame after the pad hook asked for it. The pad hook runs inside a function whose registers live in
// locals, where a call into the game is not safe; here every register is still in the Cpu. Leaves them as found.
void rt_save_pages_update(struct Cpu *c);
