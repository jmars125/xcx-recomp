// The F1 Mods tab's Mod priority section on its own (runtime/ui/mods_page.cpp, mod_priority), drawn headless by ImGui
// from a made-up mod list and merge status, for tests/mods_priority_test.py: the mods listed by their names, Up moving
// one, the merge lines in their looks (left out, caveat, grey for mods that add nothing or are partly alike) and the
// restart line once the list differs from what the game merged at start-up. Prints "ok <n>" or one "FAIL <check>" per
// failure; the exit code is the failure count. Also each row's tag of what the merges said about it, Turn off, and
// Merge now: merging, then the next start's lines and tags in place of this run's. And a row's note opening that mod's
// lines, Revert to the start-up list, Restart now (asking first in the world), and the priority number in the
// library's picked mod.
#define XCX_MOD_UI_TESTING
#include "../runtime/ui/mods_page.cpp"
#include <cstdlib>

// What mods_page.cpp calls besides ImGui and the menu text. Weak, so the file links whichever of these the page calls.
static ModLibView test_view;
static char test_ids[8][65];
static int test_n, test_changed;
static const char *test_status[8];
static int test_kind[8], test_nstatus;
static char moved[65];
static int moved_up = -1;
extern "C" {
__attribute__((weak)) void modlib_snapshot(ModLibView *v) { *v = test_view; }
__attribute__((weak)) void modlib_refresh(void) {}
__attribute__((weak)) void modlib_install(const char *) {}
__attribute__((weak)) void modlib_remove(const char *) {}
static char lib_off[65];   // modlib_set_enabled(id, 0): the library's own switch
__attribute__((weak)) void modlib_set_enabled(const char *id, int on) {
    if (!on) snprintf(lib_off, sizeof lib_off, "%s", id);
}
__attribute__((weak)) void mod_ui_snapshot(ModSnapshot *out) { memset(out, 0, sizeof *out); }
__attribute__((weak)) void mod_ui_interaction(int) {}
__attribute__((weak)) int rt_mods_ui_submit(const ModAction *) { return 0; }
__attribute__((weak)) int rt_mods_ui_intent(const struct ModIntent *) { return 0; }
__attribute__((weak)) int romfs_mods_priority(char (*ids)[65], int cap) {
    for (int i = 0; i < test_n && i < cap; i++) memcpy(ids[i], test_ids[i], 65);
    return test_n;
}
__attribute__((weak)) int romfs_mods_move(const char *id, int up) {
    snprintf(moved, sizeof moved, "%s", id);
    moved_up = up;
    return 1;
}
__attribute__((weak)) int romfs_mods_tables_changed(void) { return test_changed; }
static int test_file_checks;   // romfs_mods_check_files calls: one per refresh, never one per row
__attribute__((weak)) void romfs_mods_check_files(void) { test_file_checks++; }
static int test_ahead, premerges;   // romfs_mods_premerge_state; calls of romfs_mods_premerge
static const char *test_next[4];
static int test_nnext;
__attribute__((weak)) int romfs_mods_premerge(void) { premerges++; return 1; }
__attribute__((weak)) int romfs_mods_premerge_state(void) { return test_ahead; }
static int test_auto, test_listchg, restores, rereads, restarts, test_restarted;
static int test_world;
__attribute__((weak)) int romfs_mods_list_changed(void) { return test_listchg; }
static int test_updated;   // "other" updated since start
__attribute__((weak)) int romfs_mod_changed(const char *id) { return test_updated && !strcmp(id, "other") ? 1 : 0; }
__attribute__((weak)) int romfs_mods_restore_list(void) { restores++; return 1; }
__attribute__((weak)) void modlib_reread_enabled(void) { rereads++; }
__attribute__((weak)) int rt_restart_game(void) { restarts++; return 1; }
__attribute__((weak)) int rt_restart_started(void) { return test_restarted; }
__attribute__((weak)) int romfs_game_in_world(void) { return test_world; }
// "other" has one line of its own in the reports, the others none
__attribute__((weak)) int bdat_merge_mod_lines(const char *id, int next, char (*out)[320], int *kinds, int cap) {
    (void)next;
    if (strcmp(id, "other") || cap < 1) return 0;
    snprintf(out[0], 320, "%s", "bdat/common.bdat: left out: Other mod: a change");
    if (kinds) kinds[0] = 1;
    return 1;
}
__attribute__((weak)) int romfs_mods_auto_merge(void) { return test_auto; }
__attribute__((weak)) void romfs_mods_set_auto_merge(int on) { test_auto = on; }
__attribute__((weak)) int bdat_merge_next_status(int i, char *out, size_t cap, int *kind) {
    if (i < 0 || i >= test_nnext) return 0;
    snprintf(out, cap, "%s", test_next[i]);
    if (kind) *kind = 0;
    return 1;
}
// the next start: "other" adds nothing there
__attribute__((weak)) int bdat_merge_next_mod_tag(const char *id, char *out, size_t cap, int *flags) {
    int on = !strcmp(id, "other");
    snprintf(out, cap, "%s", on ? "adds nothing" : "");
    if (flags) *flags = on ? BDAT_TAG_ADDS_NOTHING : 0;
    return on;
}
__attribute__((weak)) int bdat_merge_status(int i, char *out, size_t cap, int *kind) {
    if (i < 0 || i >= test_nstatus) return 0;
    snprintf(out, cap, "%s", test_status[i]);
    if (kind) *kind = test_kind[i];
    return 1;
}
// tags: "whole.piece" and "whole" add nothing, "other" has a left-out change
__attribute__((weak)) int bdat_merge_mod_tag(const char *id, char *out, size_t cap, int *flags) {
    const char *t = !strcmp(id, "whole.piece") || !strcmp(id, "whole") ? "adds nothing" : !strcmp(id, "other") ?
                    "2 left out" : "";
    snprintf(out, cap, "%s", t);
    if (flags) *flags = !strcmp(t, "adds nothing") ? BDAT_TAG_ADDS_NOTHING : t[0] ? BDAT_TAG_LEFT_OUT : 0;
    return t[0] != 0;
}
static char turned_off[65];
__attribute__((weak)) int romfs_mods_set(const char *id, int on) {
    if (!on) snprintf(turned_off, sizeof turned_off, "%s", id);
    return 1;
}
const char *cfg_value(const char *) { return "en"; }
unsigned cfg_version(void) { return 1; }
const char *ov_language(void) { return "en-US"; }
const char *plat_ui_language(void) { return "en"; }
}

static int checks, failures;
static void verify(bool yes, const char *what) {
    ++checks;
    if (!yes) { ++failures; printf("FAIL %s\n", what); }
}

static void frame() {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Mods", nullptr, ImGuiWindowFlags_NoDecoration);
    mods_page(1.0f, 0, false);
    ImGui::End();
    ImGui::Render();
}

static void refresh() {   // the page re-reads the list and the status when the library view changes
    test_view.generation++;
    frame();
    frame();
}

static void row(const char *id, const char *name) {
    ModLibRow &r = test_view.rows[test_view.count++];
    memset(&r, 0, sizeof r);
    snprintf(r.id, sizeof r.id, "%s", id);
    snprintf(r.name, sizeof r.name, "%s", name);
    strcpy(r.kind, "romfs");
    r.installed = r.enabled = 1;
}

static void click(ImVec2 lo, ImVec2 hi) {   // a press and release in the middle of a widget, a frame each
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent((lo.x + hi.x) / 2, (lo.y + hi.y) / 2);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
}

static int find(char tag, const char *text) {   // the index of a drawn line, -1 when there is none
    for (int i = 0; i < prio_count; i++)
        if (prio_tag[i] == tag && (!text || strstr(prio_lines[i], text))) return i;
    return -1;
}

int main() {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 2000);   // tall enough that the whole section is on the page
    io.DeltaTime = 1.0f / 60;
    io.IniFilename = nullptr;
    io.Fonts->Build();
    row("whole", "Whole mod");
    row("whole.piece", "Whole mod: Piece");
    row("other", "Other mod");
    test_view.state = MODLIB_IDLE;

    // no mod on and nothing merged: no section
    test_n = 0;
    refresh();
    verify(prio_count == 0, "no mod and no merge: no Mod priority section");

    // one mod on (one without data tables, say) and nothing merged: still listed, with Turn off
    strcpy(test_ids[0], "whole");
    test_n = 1;
    refresh();
    verify(find('P', "Whole mod") >= 0 && prio_off_shown[0], "one mod on is listed, with Turn off");

    // three mods, merged with one of each kind of line
    strcpy(test_ids[0], "whole.piece");
    strcpy(test_ids[1], "whole");
    strcpy(test_ids[2], "other");
    test_n = 3;
    static const char *const lines[] = {"bdat/common.bdat: 3 mods merged", "bdat/common.bdat: left out: other: x",
                                        "bdat/common.bdat: caveat: other: y",
                                        "bdat/common.bdat: adds nothing: Whole mod: Piece: z",
                                        "bdat/common.bdat: partly alike: Whole mod: w"};
    for (int i = 0; i < 5; i++) {
        test_status[i] = lines[i];
        test_kind[i] = i;
    }
    test_nstatus = 5;
    refresh();
    verify(find('P', nullptr) == find('P', "Whole mod: Piece") && find('P', "Whole mod: Piece") < find('P', "Other mod"),
           "the mods are listed by name, in order");
    verify(find('H', nullptr) >= 0, "the merge lines have their heading");
    verify(find('N', "3 mods merged") >= 0, "the summary line is plain");
    verify(find('O', "left out") >= 0 && find('Y', "caveat") >= 0, "left out and caveat lines have their colours");
    verify(find('G', "adds nothing") >= 0 && find('G', "partly alike") >= 0, "adds-nothing and partly-alike are grey");
    verify(find('R', nullptr) < 0, "no restart line while the list is what was merged");
    verify(find('T', "adds nothing") == find('P', "Whole mod: Piece") + 1 && find('T', "2 left out") >= 0,
           "each mod's row carries what the merges said about it");
    verify(prio_off_shown[0] && prio_off_shown[1] && prio_off_shown[2], "every mod has Turn off");
    io.AddMousePosEvent((prio_off_low[0].x + prio_off_high[0].x) / 2, (prio_off_low[0].y + prio_off_high[0].y) / 2);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    verify(!strcmp(lib_off, "whole.piece") && !turned_off[0],
           "Turn off goes through the library, so its row shows the change");

    // Up on the third mod asks romfs_mods.c to move it
    io.AddMousePosEvent((prio_up_low[2].x + prio_up_high[2].x) / 2, (prio_up_low[2].y + prio_up_high[2].y) / 2);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    verify(!strcmp(moved, "other") && moved_up == 1, "Up moves that mod up");

    // the list differs from what was merged at start-up: the restart line, Merge now, and this run's lines out of
    // the way under their own (shut) heading
    test_changed = 1;
    memset(prio_off_shown, 0, sizeof prio_off_shown);
    refresh();
    int r = find('R', nullptr);
    verify(r >= 0 && r < find('M', nullptr) && find('M', nullptr) < find('S', nullptr),
           "the restart line comes first, then Merge now, then the start-up lines' heading");
    verify(r >= 0 && strstr(prio_lines[r], "restart"), "the restart line says to restart");
    verify(find('N', nullptr) < 0, "this run's lines are shut away once the list changed");
    verify(find('T', nullptr) < 0, "a changed list hides the tags, which are of the old one");
    verify(prio_off_shown[0], "Turn off stays while the list is changed");

    // Merge now asks romfs_mods.c to merge ahead; while it runs, the page says so
    io.AddMousePosEvent((prio_merge_low.x + prio_merge_high.x) / 2, (prio_merge_low.y + prio_merge_high.y) / 2);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    verify(premerges == 1, "Merge now starts the merge ahead");
    test_ahead = 1;
    refresh();
    verify(find('W', nullptr) >= 0 && find('M', nullptr) < 0, "merging: no button, a line saying so");

    // merged ahead: the next start's lines and tags
    static const char *const next_lines[] = {"bdat/common.bdat: 2 mods merged (next)"};
    test_next[0] = next_lines[0];
    test_nnext = 1;
    test_ahead = 2;
    refresh();
    int x = find('X', nullptr);
    verify(x > r && find('N', "(next)") == x + 1, "the next start's heading, then its lines");
    verify(find('Q', nullptr) > r && find('Q', nullptr) < x, "the buttons sit right under the restart line");
    verify(find('T', "adds nothing") == find('P', "Other mod") + 1 && find('T', "2 left out") < 0,
           "the rows carry the next start's tags");
    test_nnext = 0;
    refresh();
    verify(find('Z', nullptr) >= 0, "nothing to merge at the next start says so");
    test_ahead = 0;

    // the merge-automatically box: first in the section, and a click turns the setting on
    verify(find('A', nullptr) >= 0 && find('A', nullptr) < find('P', nullptr), "the merge-automatically box leads");
    io.AddMousePosEvent(prio_auto_low.x + 6, (prio_auto_low.y + prio_auto_high.y) / 2);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
    verify(test_auto == 1, "ticking the box turns automatic merging on");
    refresh();
    verify(find('M', nullptr) < 0 && find('W', nullptr) >= 0,
           "merging automatically: no Merge now, the changed list shows as merging");

    // nothing merged and no mod left, but the list changed since start-up: the section still says so
    test_n = 0;
    test_nstatus = 0;
    refresh();
    verify(find('R', nullptr) >= 0 && find('P', nullptr) < 0, "a changed list shows the restart line on its own");
    test_n = 1;
    refresh();
    verify(find('P', "Whole mod") >= 0, "the last mod left stays listed");

    // the picked mod in the library (the first row, "whole") says where it is in the priority list
    strcpy(test_ids[0], "whole.piece");
    strcpy(test_ids[1], "whole");
    strcpy(test_ids[2], "other");
    test_n = 3;
    test_changed = 0;
    test_auto = 0;
    refresh();
    verify(find('K', "Priority 2 of 3") >= 0, "the library's picked mod shows its priority number");

    // a row's note opens that mod's own lines, and closes them again; a mod with none says so
    click(prio_tag_low[2], prio_tag_high[2]);
    refresh();
    verify(find('D', "Other mod: a change") == find('T', "2 left out") + 1, "a note opens its mod's lines below it");
    click(prio_tag_low[2], prio_tag_high[2]);
    refresh();
    verify(find('D', nullptr) < 0, "a second press closes them");
    click(prio_tag_low[0], prio_tag_high[0]);
    refresh();
    verify(find('E', nullptr) >= 0, "a mod with no lines of its own says so");
    click(prio_tag_low[0], prio_tag_high[0]);
    refresh();

    // a changed list: Revert to the start-up list, which reads the lists again for the library, and Restart now
    test_changed = 1;
    test_listchg = 1;
    refresh();
    verify(find('U', nullptr) >= 0 && find('Q', nullptr) > find('U', nullptr),
           "Revert to the start-up list, then Restart now");
    click(prio_undo_low, prio_undo_high);
    verify(restores == 1 && rereads == 1, "Revert to the start-up list writes it back and updates the library");
    test_listchg = 0;
    refresh();
    verify(find('U', nullptr) < 0 && find('Q', nullptr) >= 0, "an updated mod alone: Restart now, no way back");
    test_updated = 1;
    refresh();
    verify(find('V', "updated after") == find('P', "Other mod") + 1, "the updated mod's row says so");
    test_updated = 0;
    refresh();
    verify(find('V', nullptr) < 0, "no mark once nothing changed");

    // at the title screen (no save loaded) Restart now restarts at once; in the world it asks first
    test_world = 0;
    click(prio_restart_low, prio_restart_high);
    verify(restarts == 1, "Restart now at the title screen restarts at once");
    test_world = 1;
    click(prio_restart_low, prio_restart_high);
    frame();
    verify(restarts == 1 && find('C', nullptr) >= 0, "Restart now in the world asks first");
    click(prio_yes_low, prio_yes_high);
    verify(restarts == 2, "Restart in the question restarts");

    // once a restart has started a copy, neither Restart nor the question's Restart starts another while the window closes
    test_restarted = 1;
    test_world = 0;
    click(prio_restart_low, prio_restart_high);
    verify(restarts == 2, "Restart now does nothing once a restart is under way");
    test_world = 1;
    test_restarted = 0;
    click(prio_restart_low, prio_restart_high);
    frame();
    test_restarted = 1;
    click(prio_yes_low, prio_yes_high);
    verify(restarts == 2, "the question's Restart does nothing once a restart is under way");

    // the mod files are looked at once per refresh, not once per row; and a download, which publishes a new library
    // view with every chunk, doesn't make the page look at them every frame
    test_changed = 0;   // nothing stale: the page's own timer waits a full second
    test_file_checks = 0;
    refresh();
    verify(test_file_checks == 1, "one look at the mod files per refresh, for all three rows");
    test_view.state = MODLIB_WORKING;
    test_file_checks = 0;
    for (int i = 0; i < 30; i++) {   // half a second of frames, each with a new view
        test_view.generation++;
        frame();
    }
    verify(test_file_checks == 0, "a download's new views don't make the page look at the mod files every frame");
    test_view.state = MODLIB_IDLE;
    refresh();
    verify(test_file_checks == 1, "once the library is idle again, its new view is looked at at once");

    ImGui::DestroyContext();
    if (!failures) printf("ok %d\n", checks);
    return failures;
}
