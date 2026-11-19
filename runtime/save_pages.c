// Save slot pages, see save_pages.h.
#include "save_pages.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "fs.h"
#include "host.h"
#include "input.h"
#include "swkbd.h"
#include "platform/platform.h"

static volatile int page;   // read by guest threads (fs_host.c), written by the Save/Load screen's thread

int save_pages_current(void) { return __atomic_load_n(&page, __ATOMIC_RELAXED); }

int save_pages_set(int p) {
    if (p < 0) p = 0;
    if (p >= SAVE_PAGE_COUNT) p = SAVE_PAGE_COUNT - 1;
    return __atomic_exchange_n(&page, p, __ATOMIC_RELAXED) != p;
}

int save_pages_step(int delta) {
    int p = ((save_pages_current() + delta) % SAVE_PAGE_COUNT + SAVE_PAGE_COUNT) % SAVE_PAGE_COUNT;
    return save_pages_set(p);
}

void save_pages_label(char *out, size_t cap) {
    snprintf(out, cap, "Page %d/%d", save_pages_current() + 1, SAVE_PAGE_COUNT);
}

// ---- page names
static char names[SAVE_PAGE_COUNT][SAVE_PAGE_NAME_MAX * 4 + 1];   // UTF-8; empty = not named
static int names_loaded;

int save_pages_has_name(int page) { return page >= 0 && page < SAVE_PAGE_COUNT && names[page][0]; }

void save_pages_name(int page, char *out, size_t cap) {
    if (save_pages_has_name(page)) snprintf(out, cap, "%s", names[page]);
    else snprintf(out, cap, "Page %d", page + 1);
}

int save_pages_set_name(int page, const char *utf8) {
    if (page < 0 || page >= SAVE_PAGE_COUNT) return 0;
    char clean[sizeof names[0]];
    size_t o = 0;
    int chars = 0;
    for (const unsigned char *p = (const unsigned char *)(utf8 ? utf8 : ""); *p && chars < SAVE_PAGE_NAME_MAX;) {
        size_t k = *p < 0x80 ? 1 : *p >= 0xf0 ? 4 : *p >= 0xe0 ? 3 : *p >= 0xc0 ? 2 : 1;
        int whole = *p < 0x80 || (*p >= 0xc0 && k > 1);   // a stray continuation byte is not a character
        for (size_t i = 1; i < k && whole; i++) if ((p[i] & 0xc0) != 0x80) { whole = 0, k = i; }
        if (!whole || *p < 0x20 || *p == 0x7f || *p == '[' || *p == ']') { p += k; continue; }
        if (o + k >= sizeof clean) break;
        memcpy(clean + o, p, k);
        o += k, p += k, chars++;
    }
    while (o && clean[o - 1] == ' ') o--;   // no spaces at the ends
    clean[o] = 0;
    size_t lead = strspn(clean, " ");
    memmove(clean, clean + lead, o - lead + 1);
    if (!strcmp(clean, names[page])) return 0;
    snprintf(names[page], sizeof names[page], "%s", clean);
    return 1;
}

void save_pages_names_parse(const char *text) {
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char buf[SAVE_PAGE_NAME_MAX * 4 + 16];
        if (len < sizeof buf) {
            memcpy(buf, line, len), buf[len] = 0;
            if (len && buf[len - 1] == '\r') buf[len - 1] = 0;
            char *tab = strchr(buf, '\t');
            int page = atoi(buf);
            if (!strncmp(buf, "last\t", 5)) {   // the page the screen was last left on
                if (page = atoi(buf + 5), page >= 1 && page <= SAVE_PAGE_COUNT) save_pages_set(page - 1);
            } else if (tab && page >= 1 && page <= SAVE_PAGE_COUNT) {
                save_pages_set_name(page - 1, tab + 1);
            }
        }
        line = end ? end + 1 : NULL;
    }
}

size_t save_pages_names_format(char *out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < SAVE_PAGE_COUNT; i++) {
        if (!names[i][0]) continue;
        int w = snprintf(out + o, cap - o, "%d\t%s\n", i + 1, names[i]);
        if (w < 0 || (size_t)w >= cap - o) break;
        o += (size_t)w;
    }
    if (save_pages_current() && cap - o > 16) o += (size_t)snprintf(out + o, cap - o, "last\t%d\n", save_pages_current() + 1);
    return o;
}

#define NAMES_FILE "save-pages.txt"

static void names_load(void) {   // once, when the screen first needs a name
    if (names_loaded) return;
    char path[1200];
    if (!fs_host_profile_file(NAMES_FILE, path, sizeof path)) return;   // storage not up yet: try again later
    names_loaded = 1;
    uint64_t size;
    uint8_t *data = plat_file_read(path, &size);
    if (!data) return;
    save_pages_names_parse((const char *)data);
    free(data);
}

static void names_save(void) {
    char path[1200], text[SAVE_PAGE_COUNT * (SAVE_PAGE_NAME_MAX * 4 + 8) + 32];
    if (!fs_host_profile_file(NAMES_FILE, path, sizeof path)) return;
    size_t n = save_pages_names_format(text, sizeof text);
    if (!plat_file_write_atomic(path, text, n)) rt_log("save pages: cannot write %s", path);
}

int save_pages_remap_path(const char *rest, char *out, size_t cap) {
    static const char prefix[] = "/sts/game/gamedata";
    int p = save_pages_current();
    if (!p || strncmp(rest, prefix, sizeof prefix - 1)) return 0;
    const char *d = rest + sizeof prefix - 1;
    if (d[0] < '0' || d[0] > '9' || d[1] < '0' || d[1] > '9') return 0;
    int slot = (d[0] - '0') * 10 + (d[1] - '0');
    if (slot < 1 || slot > SAVE_PAGE_SLOTS) return 0;
    const char *tail = d + 2;
    if (*tail && strcmp(tail, ".tmb")) return 0;
    return snprintf(out, cap, "%s%02d%s", prefix, slot + p * SAVE_PAGE_SLOTS, tail) < (int)cap;
}

// Fields of the screen object, offsets from its base (found from the screen's update code and a state trace).
enum {
    SCENE_ROWS = 0x438,        // 4 rows of ROW_SIZE bytes: u32 state at +0 (0 idle, 1 requested, 4 loaded), u16 slot at +4, valid byte at +9
    ROW_SIZE = 0xc5b0,
    SCENE_COUNT = 0xf1af8,     // u32 number of rows
    SCENE_LOADING = 0x121b08,  // u32, 1 while the rows load
    SCENE_READY = 0x121b0c,    // u32, bit 0 once the screen is ready for input
};
#define W32(a, v) (*(volatile uint32_t *)(uintptr_t)(a) = (v))
#define W8(a, v) (*(volatile uint8_t *)(uintptr_t)(a) = (v))

void save_pages_reload(uint64_t scene) {
    for (int i = 1; i < 4; i++) {   // row 0 is the auto save, which no page changes
        uint64_t row = scene + SCENE_ROWS + (uint64_t)i * ROW_SIZE;
        W8(row + 9, 0);
        W32(row, 1);
    }
    W32(scene + SCENE_READY, 0);
    W32(scene + SCENE_LOADING, 1);
}

// The game's button bits in the mask of buttons pressed this frame.
enum { PAD_Y = 0x01, PAD_L = 0x10, PAD_R = 0x20 };
static int sound_pending;   // a page change waits for rt_save_pages_update to play its sound
static int title_dirty;     // the page's name changed: rt_save_pages_update draws the title again
// The game's sound effect queue: the manager's accessor and the request (manager, effect, 0). Effect 5 is the one the
// game's own screens play when L or R switches a tab or page.
#define MAIN 0x7000000000ull
enum { F_SOUND_MANAGER = 0x208554, F_PLAY_SOUND = 0x40bc48, SOUND_TAB_SWITCH = 5 };

// The name entry: the host's text entry the game's character naming uses, shown over the game while this guest thread
// waits in it (the virtual core given up, the game's input blocked), as swkbd.c does for the game's own request.
static void rename_page(void) {
    static SwkbdRequest req;
    int page = save_pages_current();
    char current[SAVE_PAGE_NAME_MAX * 4 + 1] = "";
    if (save_pages_has_name(page)) save_pages_name(page, current, sizeof current);
    memset(&req, 0, sizeof req);
    swkbd_utf8_to_utf16("Enter a name for this page.", req.prompt, sizeof req.prompt / sizeof *req.prompt - 1);
    swkbd_utf8_to_utf16(current, req.initial, KB_LIMIT);
    req.max_len = SAVE_PAGE_NAME_MAX;   // no minimum: an empty name puts "Page N" back
    uint16_t answer[KB_LIMIT * 2 + 1];
    size_t n = 0;
    int away = kern_core_away();
    input_block(1);
    int confirmed = text_entry_run(&req, answer, KB_LIMIT * 2, &n);
    input_block(0);
    kern_core_back(&away);
    if (!confirmed) return;
    char utf8[KB_LIMIT * 6 + 1];
    swkbd_utf16_to_utf8(answer, n, utf8, sizeof utf8);
    if (save_pages_set_name(page, utf8)) {
        names_save();
        title_dirty = 1;
    }
    rt_log("save pages: page %d is now \"%s\"", page + 1, utf8);
}

static int rename_cooldown;   // hook calls (one per frame) that ignore Y after a name entry: the press that opened it is
                              // still the one the screen reports on the frame the entry returns

void rt_save_pages_pad(uint64_t scene, uint32_t pad) {
    names_load();
    if (rename_cooldown > 0) {
        rename_cooldown--;
        pad &= ~(uint32_t)PAD_Y;
    }
    int delta = pad & PAD_R ? 1 : pad & PAD_L ? -1 : 0;
    if (pad & PAD_L && pad & PAD_R) delta = 0;
    if (delta && save_pages_step(delta)) {
        save_pages_reload(scene);
        sound_pending = 1;
        names_save();   // the page is kept for the next launch
    } else if (pad & PAD_Y && !(pad & (PAD_L | PAD_R))) {
        rename_page();
        rename_cooldown = 30;
    }
}

static void title_label(Cpu *c);

void rt_save_pages_update(Cpu *c) {
    if (!sound_pending && !title_dirty) return;
    uint64_t saved[19];
    memcpy(saved, c->x, sizeof saved);
    if (sound_pending) {
        sound_pending = 0;
        uint64_t sound = rt_call(c, MAIN + F_SOUND_MANAGER, 0);
        if (sound) rt_call(c, MAIN + F_PLAY_SOUND, 3, sound, SOUND_TAB_SWITCH, 0);
    }
    if (title_dirty) {
        title_dirty = 0;
        title_label(c);
    }
    memcpy(c->x, saved, sizeof saved);
}

// Game functions that find a child widget by name, take it as a text widget and set its string (the row code at
// main+0x6665c0 does the same for the date), and the name of the label that marks the auto save row. Every row has
// one; the manual rows keep theirs hidden, and this code shows it with the slot number.
enum { F_FIND_CHILD = 0x55f16c, F_AS_TEXT = 0x915110, F_SET_TEXT = 0x91720c, S_TYPE_LABEL = 0x17ce2ee };
enum { ROW_GROUPS = 0xd8, ROW_GROUP_SIZE = 0x10 };   // per row: a widget handle that holds that row's labels

static void row_label(Cpu *c, uint64_t scene, int row, const char *text) {
    uint64_t label = rt_call(c, MAIN + F_FIND_CHILD, 2, scene + ROW_GROUPS + (uint64_t)row * ROW_GROUP_SIZE, MAIN + S_TYPE_LABEL);
    uint64_t t = label ? rt_call(c, MAIN + F_AS_TEXT, 1, label) : 0;
    if (!t) return;
    rt_call(c, MAIN + F_SET_TEXT, 2, t, (uint64_t)(uintptr_t)text);
    rt_call(c, LD64(LD64(t) + 0x20), 2, t, 1);   // virtual +0x20: show or hide
}

enum { TITLE_NONE, TITLE_ARMED, TITLE_HAVE };
static struct { int state, busy; uint64_t widget; char text[64], empty[48]; } title;   // touched only by the screen's guest thread
enum { F_TEXT_CALLER_END = 0x55f5c0 };   // return address inside the by-message-id label setter the frame uses
enum { F_EMPTY_CALLER_END = 0x665bb8, S_EMPTY_LABEL = 0x17c6589, S_EMPTY_LABEL2 = 0x17a936d, EMPTY_GROUPS = 0x120 };
// The text of a row without a save is set by the row code at main+0x665bb4, in rect_TextNon and rect_TextNon02 of the
// row's second widget group (one per row from EMPTY_GROUPS). The first row's text is kept, so each empty row can show
// it with the slot number after it in whatever language the game runs in.

// The screen frame sets its title through main+0x55f548 (layout handle, child name, message). Its layout also holds
// spare text panes beside the title, rect_TextTitle02 and rect_TextTitle03, that the Save and Load screens leave empty:
// the page's name goes into the second. The layout places that pane right after the first by the width of the first's
// text, and the first sits lower, so it gets blanks that carry the name across the slash into the empty part of the title
// bar, as many as put the name in the middle of it. The layout handle is kept when the title is found, as {layout object,
// part index}, the form the game's find-child call takes.
static struct { uint64_t layout; uint32_t idx; } last_label;   // the handle of the label being set
static uint64_t frame_handle[2];                                // the title's handle, once found
static const char SPACER_PANE[] = "rect_TextTitle02", NAME_PANE[] = "rect_TextTitle03";

// Where the name goes, in the layout's own pixels at 1920 wide as measured on screenshots: the second pane starts
// NAME_BASE after the layout's left edge when the first holds no text, and every blank space in the first moves it by
// SPACE_WIDTH. BOX_CENTER is the middle of the empty part of the title bar. name_width gives a name's width in the same
// units, as an estimate by kind of character (the font is proportional). The name is drawn in the title's typeface (see
// match_title_style): the widths are those first measured for the smaller sub-title style times the ratio measured for
// twenty repeated letters in both styles (m 1.39, W 1.29, A, n, 0 and a space about 1.5).
enum { NAME_BASE = 355, BOX_CENTER = 1018, MAX_SPACES = 90 };
static const float SPACE_WIDTH = 9.1f;

static float name_width(const char *s) {
    float w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned cp = *p, k = 1;
        if (cp >= 0xf0) cp = (cp & 7), k = 4;
        else if (cp >= 0xe0) cp = (cp & 15), k = 3;
        else if (cp >= 0xc0) cp = (cp & 31), k = 2;
        for (unsigned i = 1; i < k && p[i]; i++) cp = cp << 6 | (p[i] & 63);
        p += k;
        if (cp == ' ') w += 13.6f;
        else if (strchr("iljtfr.,;:!|'`", (int)cp) && cp < 128) w += 11.3f;
        else if (strchr("mwMW@%", (int)cp) && cp < 128) w += 31.0f;
        else if (cp >= '0' && cp <= '9') w += 31.5f;
        else if (cp >= 'A' && cp <= 'Z') w += 25.5f;
        else if (cp >= 'a' && cp <= 'z') w += 18.8f;
        else if (cp < 0x2e80) w += cp < 128 ? 16.5f : 21.0f;
        else w += 51.0f;   // full-width characters
    }
    return w;
}

// The blanks that carry a name of this text across to the middle of the box, so it reads as centred.
static void name_spacer(const char *name, char *out, size_t cap) {
    int n = (int)((BOX_CENTER - NAME_BASE - name_width(name) / 2) / SPACE_WIDTH + 0.5f);
    if (n < 0) n = 0;
    if (n > MAX_SPACES) n = MAX_SPACES;
    if ((size_t)n >= cap) n = (int)cap - 1;
    memset(out, ' ', (size_t)n);
    out[n] = 0;
}

void rt_save_pages_label_entry(Cpu *c) {
    if (title.state != TITLE_ARMED) return;
    uint64_t h = c->x[0];
    last_label.layout = h ? LD64(h) : 0;
    last_label.idx = h ? LD32(h + 8) : 0;
}

static int load_screen;   // the screen asked for last is the Load screen

void rt_save_pages_request(uint32_t mode) {
    load_screen = !(mode & 1);
}

void rt_save_pages_open(void) {
    names_load();
    title.state = TITLE_ARMED;
    title.widget = 0;
}

void rt_save_pages_text(Cpu *c) {
    uint64_t str = c->x[1];
    if (c->x[30] == MAIN + F_EMPTY_CALLER_END && str > 0x10000 && !title.busy)   // the first empty-row label's text
        snprintf(title.empty, sizeof title.empty, "%.*s", (int)sizeof title.empty - 1, (const char *)(uintptr_t)str);
    if (title.state != TITLE_ARMED || title.busy || str < 0x10000 || c->x[30] != MAIN + F_TEXT_CALLER_END) return;
    title.widget = c->x[0];
    frame_handle[0] = last_label.layout, frame_handle[1] = last_label.idx;
    snprintf(title.text, sizeof title.text, "%.*s", (int)sizeof title.text - 1, (const char *)(uintptr_t)str);
    title.state = TITLE_HAVE;
}

// The name pane has the layout's sub-title style: smaller, and a different typeface. The game's text setter reads a
// style selector from the text pane's widget (a flag at +0x58, a kind at +0x59 and a style id at +0x5a), so the name
// pane takes the title widget's selector and is then drawn in the title's typeface and size. The colour is not part of
// the selector: it is set by NAME_COLOUR.
enum { STYLE_FLAG = 0x58, STYLE_KIND = 0x59, STYLE_ID = 0x5a };

// The colour is set by a tag in the text: the game's colour tag takes the name of an entry of its colour table (black,
// white, red, green, blue, yellow, gray, lightblue, orange, lightblue2 and lightblue3). The title's pale blue-grey is
// nearest to lightblue3, within about 8 of 255 per channel on screen; the name's default is the same hue, a quarter
// dimmer. A name typed by a player has [ and ] removed, so it cannot carry a tag of its own.
static const char NAME_COLOUR[] = "[ST:col p1=lightblue3]";

static void match_title_style(uint64_t name_widget) {
    if (!title.widget || !LD8(title.widget + STYLE_FLAG)) return;
    ST8(name_widget + STYLE_FLAG, LD8(title.widget + STYLE_FLAG));
    ST8(name_widget + STYLE_KIND, LD8(title.widget + STYLE_KIND));
    ST16(name_widget + STYLE_ID, LD16(title.widget + STYLE_ID));
}

static void title_label(Cpu *c) {
    if (title.state != TITLE_HAVE) return;
    char page[24], name[SAVE_PAGE_NAME_MAX * 4 + 1], label[160];
    save_pages_label(page, sizeof page);
    save_pages_name(save_pages_current(), name, sizeof name);
    // The Load screen's title is the game's "Load Game", longer than the Save screen's "Save", so it shows "Load". Like
    // "Page", that word is English in every language.
    snprintf(label, sizeof label, "%s   [ST:icon p1=l] %s [ST:icon p1=r]", load_screen ? "Load" : title.text, page);
    title.busy = 1;
    rt_call(c, MAIN + F_SET_TEXT, 2, title.widget, (uint64_t)(uintptr_t)label);
    for (int k = 0; k < 2 && frame_handle[0]; k++) {
        char spacer[MAX_SPACES + 1];
        name_spacer(name, spacer, sizeof spacer);
        const char *pane_name = k ? NAME_PANE : SPACER_PANE, *text = k ? name : spacer;
        uint64_t pane = rt_call(c, MAIN + F_FIND_CHILD, 2, (uint64_t)(uintptr_t)frame_handle, (uint64_t)(uintptr_t)pane_name);
        uint64_t t = pane ? rt_call(c, MAIN + F_AS_TEXT, 1, pane) : 0;
        if (t) {
            char coloured[sizeof name + sizeof NAME_COLOUR];
            if (k) {   // the name pane only; the pane of blanks has no visible text
                match_title_style(t);
                snprintf(coloured, sizeof coloured, "%s%s", NAME_COLOUR, text);
                text = coloured;
            }
            rt_call(c, MAIN + F_SET_TEXT, 2, t, (uint64_t)(uintptr_t)text);
            rt_call(c, LD64(LD64(t) + 0x20), 2, t, 1);   // virtual +0x20: show
        }
    }
    title.busy = 0;
}

void rt_save_pages_rows(Cpu *c) {
    uint64_t scene = c->x[0], saved[19];
    memcpy(saved, c->x, sizeof saved);
    for (int row = 1; row <= SAVE_PAGE_SLOTS; row++) {
        char text[24];
        snprintf(text, sizeof text, "No. %02d", save_pages_current() * SAVE_PAGE_SLOTS + row);
        row_label(c, scene, row, text);
    }
    for (int row = 1; row <= SAVE_PAGE_SLOTS && title.empty[0]; row++) {
        char text[96];
        snprintf(text, sizeof text, "%s   No. %02d", title.empty, save_pages_current() * SAVE_PAGE_SLOTS + row);
        for (int k = 0; k < 2; k++) {
            uint64_t w = rt_call(c, MAIN + F_FIND_CHILD, 2, scene + EMPTY_GROUPS + (uint64_t)row * ROW_GROUP_SIZE,
                                 MAIN + (k ? S_EMPTY_LABEL2 : S_EMPTY_LABEL));
            uint64_t t = w ? rt_call(c, MAIN + F_AS_TEXT, 1, w) : 0;
            if (t) {
                title.busy = 1;
                rt_call(c, MAIN + F_SET_TEXT, 2, t, (uint64_t)(uintptr_t)text);
                title.busy = 0;
            }
        }
    }
    title_label(c);
    memcpy(c->x, saved, sizeof saved);
}
