// The save slot pages of runtime/save_pages.c: the file name each page maps the game's three manual slots to, the
// page range and the labels. Built and run by tests/save_pages_check.py; prints each failure and exits 1 on any.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "host.h"
#include "swkbd.h"

// save_pages.c calls the guest through these; the checks below never reach them.
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) { (void)c, (void)fn, (void)n; return 0; }
void rt_log(const char *fmt, ...) { (void)fmt; }
// The name entry and the profile file are not reached by the checks below, except the file's text through these.
static char file_text[2048];
int fs_host_profile_file(const char *name, char *out, size_t cap) { return snprintf(out, cap, "/profile/%s", name) < (int)cap; }
uint8_t *plat_file_read(const char *path, uint64_t *size) { (void)path, (void)size; return NULL; }
int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    (void)path;
    snprintf(file_text, sizeof file_text, "%.*s", (int)n, (const char *)data);
    return 1;
}
int text_entry_run(const SwkbdRequest *req, uint16_t *out, size_t cap, size_t *n) { (void)req, (void)out, (void)cap; *n = 0; return 0; }
size_t swkbd_utf8_to_utf16(const char *s, uint16_t *out, size_t cap) { size_t n = 0; while (s[n] && n < cap) out[n] = (uint8_t)s[n], n++; return n; }
size_t swkbd_utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t cap) { size_t i = 0; for (; i < n && i + 1 < cap; i++) out[i] = (char)s[i]; out[i] = 0; return i; }
void input_block(int on) { (void)on; }
int kern_core_away(void) { return 0; }
void kern_core_back(int *away) { (void)away; }

#include "../runtime/save_pages.c"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void expect_map(const char *in, const char *want) {
    char out[96] = "";
    int r = save_pages_remap_path(in, out, sizeof out);
    if (want) CHECK(r && !strcmp(out, want), "%s -> '%s' (want %s)", in, out, want);
    else CHECK(!r, "%s was mapped to '%s'", in, out);
}

int main(void) {
    CHECK(save_pages_current() == 0, "starts on the first page");
    expect_map("/sts/game/gamedata01", NULL);   // page 1 is the game's own naming
    expect_map("/sts/game/gamedata03.tmb", NULL);

    CHECK(save_pages_set(1), "page 2 is a change");
    CHECK(!save_pages_set(1), "page 2 again is not");
    expect_map("/sts/game/gamedata01", "/sts/game/gamedata04");
    expect_map("/sts/game/gamedata02.tmb", "/sts/game/gamedata05.tmb");
    expect_map("/sts/game/gamedata03", "/sts/game/gamedata06");
    expect_map("/sts/game/gamedataa", NULL);        // the auto save never moves
    expect_map("/sts/game/gamedataa.tmb", NULL);
    expect_map("/sts/game/gamedata04", NULL);       // only the game's own three names map
    expect_map("/sts/game/gamedata00", NULL);
    expect_map("/sts/game/gamedata01.bak", NULL);
    expect_map("/sts/game/gamedata1", NULL);
    expect_map("/sts/game/systemdata", NULL);
    expect_map("/sts/game/socialdata", NULL);
    expect_map("/sts/other/gamedata01", NULL);

    CHECK(save_pages_set(SAVE_PAGE_COUNT - 1), "last page");
    expect_map("/sts/game/gamedata03.tmb", "/sts/game/gamedata30.tmb");   // 30 manual slots in all
    CHECK(save_pages_set(99) == 0 && save_pages_current() == SAVE_PAGE_COUNT - 1, "clamped high");
    CHECK(save_pages_set(-5) && save_pages_current() == 0, "clamped low");

    char out[8];
    CHECK(save_pages_set(1) && !save_pages_remap_path("/sts/game/gamedata01", out, 4), "a name that does not fit is refused");

    save_pages_set(SAVE_PAGE_COUNT - 1);
    CHECK(save_pages_step(1) && save_pages_current() == 0, "next wraps to the first page");
    CHECK(save_pages_step(-1) && save_pages_current() == SAVE_PAGE_COUNT - 1, "previous wraps to the last page");
    CHECK(save_pages_step(-3) && save_pages_current() == SAVE_PAGE_COUNT - 4, "steps by delta");

    char label[32];
    save_pages_set(1);
    save_pages_label(label, sizeof label);
    CHECK(!strcmp(label, "Page 2/10"), "label '%s'", label);

    // page names
    char nm[SAVE_PAGE_NAME_MAX * 4 + 1];
    save_pages_name(2, nm, sizeof nm);
    CHECK(!strcmp(nm, "Page 3") && !save_pages_has_name(2), "an unnamed page is 'Page N': '%s'", nm);
    CHECK(save_pages_set_name(2, "Mods run"), "naming is a change");
    CHECK(!save_pages_set_name(2, "Mods run"), "the same name is not");
    save_pages_name(2, nm, sizeof nm);
    CHECK(!strcmp(nm, "Mods run") && save_pages_has_name(2), "named '%s'", nm);
    save_pages_set_name(3, "  Lin [ST:icon p1=l] \t\x01 ");
    save_pages_name(3, nm, sizeof nm);
    CHECK(!strcmp(nm, "Lin ST:icon p1=l"), "brackets, controls and the ends are dropped: '%s'", nm);
    save_pages_set_name(4, "abcdefghijklmnopqrstuvwxyz0123456789");
    save_pages_name(4, nm, sizeof nm);
    CHECK(strlen(nm) == SAVE_PAGE_NAME_MAX, "cut at %d characters: '%s'", SAVE_PAGE_NAME_MAX, nm);
    save_pages_set_name(5, "\xc3\xa9\xc3\xa9\xc3");   // two whole characters, then a cut one
    save_pages_name(5, nm, sizeof nm);
    CHECK(!strcmp(nm, "\xc3\xa9\xc3\xa9"), "a cut character is dropped: '%s'", nm);
    save_pages_set_name(6, "\xe3\x83\x9e\xe3\x83\x83");   // multi-byte characters count as characters
    save_pages_name(6, nm, sizeof nm);
    CHECK(!strcmp(nm, "\xe3\x83\x9e\xe3\x83\x83"), "UTF-8 kept: '%s'", nm);
    CHECK(save_pages_set_name(2, "") && !save_pages_has_name(2), "empty text takes the name away");
    CHECK(!save_pages_set_name(-1, "x") && !save_pages_set_name(SAVE_PAGE_COUNT, "x"), "no such page");

    char text[1024];
    size_t len = save_pages_names_format(text, sizeof text);
    CHECK(len && !strstr(text, "\n3\t") && !strncmp(text, "4\tLin ST:icon p1=l\n5\t", 21), "format: '%s'", text);
    for (int i = 0; i < SAVE_PAGE_COUNT; i++) save_pages_set_name(i, "");
    save_pages_names_parse("2\tMods run\r\n11\tout of range\n0\tnone\nbad line\n10\tChar: Elma\n");
    save_pages_name(1, nm, sizeof nm);
    CHECK(!strcmp(nm, "Mods run"), "parsed page 2: '%s'", nm);
    save_pages_name(9, nm, sizeof nm);
    CHECK(!strcmp(nm, "Char: Elma"), "parsed page 10: '%s'", nm);
    CHECK(!save_pages_has_name(0) && !save_pages_has_name(2), "lines out of range or malformed set nothing");
    save_pages_set_name(1, "Round trip");
    save_pages_names_format(text, sizeof text);
    for (int i = 0; i < SAVE_PAGE_COUNT; i++) save_pages_set_name(i, "");
    save_pages_names_parse(text);
    save_pages_name(1, nm, sizeof nm);
    CHECK(!strcmp(nm, "Round trip"), "a saved name reads back: '%s'", nm);

    // the page the screen was left on is kept with the names
    save_pages_set(4);
    save_pages_names_format(text, sizeof text);
    save_pages_set(0);
    save_pages_names_parse(text);
    CHECK(save_pages_current() == 4, "the last page reads back: %d", save_pages_current());
    save_pages_set(0);
    save_pages_names_format(text, sizeof text);
    CHECK(!strstr(text, "last"), "the first page writes no line");
    save_pages_set(3);
    save_pages_names_parse("last\t99\n");
    CHECK(save_pages_current() == 3, "a last page out of range is ignored");
    save_pages_set(0);

    // centring: a wider name needs fewer blanks before it, and none goes out of range
    char sp1[MAX_SPACES + 1], sp2[MAX_SPACES + 1], sp3[MAX_SPACES + 1];
    name_spacer("Te", sp1, sizeof sp1);
    name_spacer("Page 4", sp2, sizeof sp2);
    name_spacer("WWWWWWWWWWWWWWWWWWWWWWWW", sp3, sizeof sp3);
    CHECK(strlen(sp1) > strlen(sp2) && strlen(sp2) > strlen(sp3), "blanks %zu %zu %zu", strlen(sp1), strlen(sp2), strlen(sp3));
    name_spacer("", sp1, sizeof sp1);
    CHECK(strlen(sp1) <= MAX_SPACES && strspn(sp1, " ") == strlen(sp1), "an empty name stays in range");
    name_spacer("\xe4\xb8\xad\xe3\x81\x82\xe4\xb8\xad\xe3\x81\x82", sp2, sizeof sp2);
    CHECK(strlen(sp2) > 0 && strlen(sp2) < strlen(sp1), "full-width characters count as wide");

    puts(fails ? "save_pages_check: FAILED" : "save_pages_check: ok");
    return fails != 0;
}
