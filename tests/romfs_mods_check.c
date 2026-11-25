// runtime/romfs_mods.c's "what changed since start-up" on its own, for tests/romfs_mods_test.py, which makes a mods
// folder (XCX_MOD_DIR) with mods a and b, each with a copy of bdat/x.bdat, both on. The game reading the table is
// romfs_mod_file; the Mods tab looking at the files is romfs_mods_check_files. Checks that mod files are looked at
// only after a list change and when the Mods tab asks (never on the game's own timer), that one look serves every
// row, that bdat_merge_auto waits until the files have stayed put, and that a merge made ahead isn't made again and
// again. Prints "ok <checks>" or a FAIL line per check that fails.
#include <stdarg.h>
#include "../runtime/romfs_mods.c"

void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_mods_shutdown(void) {}   // the platform file's exit path

// No game archive: data tables two mods have are served from the top mod, as when they can't be merged.
int bdat_merge_serve(const char *mount, const char *key, const BdatModFiles *mods, int n, char *out, size_t cap) {
    (void)mount; (void)key; (void)mods; (void)n; (void)out; (void)cap;
    return 0;
}
static int prepared;   // merges made ahead (bdat_merge_auto, or Merge now)
int bdat_merge_prepare(const char *mount, const char *key, const BdatModFiles *mods, int n) {
    (void)mount; (void)key; (void)mods; (void)n;
    __atomic_add_fetch(&prepared, 1, __ATOMIC_RELAXED);
    return 1;
}
void bdat_merge_next_clear(void) {}

static int auto_on;
int cfg_bool(const char *name) { return !strcmp(name, "XCX_BDAT_MERGE_AUTO") && auto_on; }
void cfg_set(const char *name, const char *value) { (void)name; (void)value; }
int cfg_save(void) { return 1; }
const char *romfs_mount_name(void) { return "rom"; }

static int checks, failures;
static void verify(int yes, const char *what) {
    ++checks;
    if (!yes) {
        ++failures;
        printf("FAIL %s\n", what);
    }
}

static void put(const char *rel, const char *text) {   // a file under the mods folder, written whole
    char path[4200];
    snprintf(path, sizeof path, "%s/%s", getenv("XCX_MOD_DIR"), rel);
    plat_file_write_atomic(path, text, strlen(text));
}

static void game_reads(void) {   // the game opening a file now and then: maybe_reload's 0.5 s check runs at once
    char out[ROMFS_MOD_PATH_CAP];
    __atomic_store_n(&next_check_ms, 0, __ATOMIC_RELAXED);
    romfs_mod_file("rom:/bdat/x.bdat", out, sizeof out);
}

static void wait_merge(void) {
    for (int i = 0; i < 500 && __atomic_load_n(&ahead_busy, __ATOMIC_ACQUIRE); i++) plat_sleep_ms(10);
}

int main(void) {
    game_reads();   // start-up: the game reads bdat/x.bdat, which a and b both have
    verify(nstarted == 1, "the table the game read is followed");
    verify(!romfs_mods_tables_changed() && !romfs_mod_changed("a"), "nothing changed at start-up");

    // a list change (by hand, Setup or the library, the menu closed) is looked at by the game's own check at once, and
    // so is putting it back
    put("romfs-enabled.txt", "b\na\n");
    game_reads();
    verify(romfs_mods_tables_changed() == 1, "another order is a change, seen without the Mods tab");
    put("romfs-enabled.txt", "a\nb\n");
    game_reads();
    verify(romfs_mods_tables_changed() == 0, "and the order put back is none");

    // a's copy updated on disk: the game's own checks don't look at mod files, only the Mods tab does
    put("a/romfs/bdat/x.bdat", "a's copy, updated and longer");
    game_reads();
    game_reads();
    verify(!romfs_mods_tables_changed(), "with the menu closed, no mod file is looked at");
    romfs_mods_check_files();
    verify(romfs_mods_tables_changed() == 1, "the Mods tab's look sees the update");
    verify(romfs_mod_changed("a") == 1 && romfs_mod_changed("b") == 0, "and which mod it was, from that one look");

    // bdat_merge_auto: not while the file may still be changing, then once, and not again
    auto_on = 1;
    game_reads();
    verify(prepared == 0, "no merge ahead right after a change");
    // a list change seen by the game's own check: its look stores a changed_at newer than that check's start, which
    // must not count as a wait long passed (a few Up/Down clicks are one change)
    put("romfs-enabled.txt", "b\na\n");
    game_reads();
    wait_merge();
    put("romfs-enabled.txt", "a\nb\n");
    game_reads();
    wait_merge();
    verify(prepared == 0, "no merge ahead right after a list change");
    plat_sleep_ms(AUTO_MS + 100);
    game_reads();
    wait_merge();
    verify(prepared == 1 && romfs_mods_premerge_state() == 2, "merged ahead once the files stayed put");
    for (int i = 0; i < 3; i++) {
        plat_sleep_ms(CHECK_MS);
        game_reads();
        romfs_mods_check_files();
        wait_merge();
    }
    verify(prepared == 1, "a merge made ahead isn't made again while nothing changes");

    // Merge now after a file changed that nothing looked at since: the merge's own look counts, so the automatic
    // merge doesn't take the older look for a change and merge again at every check
    put("b/romfs/bdat/x.bdat", "b's copy, updated");
    romfs_mods_premerge();
    wait_merge();
    for (int i = 0; i < 4; i++) {
        plat_sleep_ms(CHECK_MS);
        game_reads();
        wait_merge();
    }
    verify(prepared == 2, "a merge made ahead from files newer than the last look isn't made again and again");
    verify(romfs_mod_changed("b") == 1, "and the merge's look shows that b was updated");

    // a file still being written: each look that sees it changed restarts the wait
    put("a/romfs/bdat/x.bdat", "a's copy, being written");
    romfs_mods_check_files();
    verify(romfs_mods_premerge_state() == 0, "the merge made ahead no longer matches");
    // Past AUTO_MS since the first change (a sleep never ends early), so only the second change's look can hold the
    // merge back; the game reads right after it, which leaves a loaded machine all of AUTO_MS before that check fails.
    plat_sleep_ms(AUTO_MS + 200);
    put("a/romfs/bdat/x.bdat", "a's copy, being written, more of it");
    romfs_mods_check_files();
    game_reads();
    wait_merge();
    verify(prepared == 2, "no merge while the file was still changing a moment ago");
    plat_sleep_ms(AUTO_MS);
    game_reads();
    wait_merge();
    verify(prepared == 3, "merged once it stayed put");

    if (!failures) printf("ok %d\n", checks);
    return failures;
}
