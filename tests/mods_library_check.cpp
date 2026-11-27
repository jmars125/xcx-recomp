// The F1 Mods tab's library list on its own (runtime/ui/mods_page.cpp, library), drawn headless by ImGui from a made-up
// view of the mods folder, for tests/mods_library_test.py: an entry's parts sit under one row that starts shut and
// opens on a click, they show without the entry's name in front, and the list scrolls in a box that takes at most
// its share of the page. Prints "ok <n>" or one "FAIL <check>" per failure; the exit code is the failure count.
#define XCX_MOD_UI_TESTING
#include "../runtime/ui/mods_page.cpp"
#include <cstdlib>

// What mods_page.cpp calls besides ImGui and the menu text, as the game would answer with no Lua mods and nothing
// merged. Weak, so the file links whichever of these the page calls.
static ModLibView test_view;
extern "C" {
__attribute__((weak)) void modlib_snapshot(ModLibView *v) { *v = test_view; }
__attribute__((weak)) void modlib_refresh(void) {}
__attribute__((weak)) void modlib_install(const char *) {}
__attribute__((weak)) void modlib_remove(const char *) {}
static char switched[65];   // the last modlib_set_enabled, as "<id> <on>"
__attribute__((weak)) void modlib_set_enabled(const char *id, int on) { snprintf(switched, sizeof switched, "%s %d", id, on); }
__attribute__((weak)) void mod_ui_snapshot(ModSnapshot *out) { memset(out, 0, sizeof *out); }
__attribute__((weak)) void mod_ui_interaction(int) {}
__attribute__((weak)) int rt_mods_ui_submit(const ModAction *) { return 0; }
__attribute__((weak)) int rt_mods_ui_intent(const struct ModIntent *) { return 0; }
__attribute__((weak)) int romfs_mods_priority(char (*)[65], int) { return 0; }
__attribute__((weak)) int romfs_mods_move(const char *, int) { return 0; }
__attribute__((weak)) int romfs_mods_tables_changed(void) { return 0; }
__attribute__((weak)) int bdat_merge_status(int, char *, size_t, int *) { return 0; }
__attribute__((weak)) int bdat_merge_mod_tag(const char *, char *out, size_t, int *flags) {
    out[0] = 0;
    if (flags) *flags = 0;
    return 0;
}
__attribute__((weak)) int romfs_mod_has_other_files(const char *) { return 0; }
__attribute__((weak)) int romfs_mods_set(const char *, int) { return 0; }
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

// The menu's window as ui/menu.cpp draws it around the page: a tab above, the page, and Continue under it.
static bool focus_tab, other_tab;   // other_tab: another tab shows, the page isn't drawn
static void frame(bool focus_first = false) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Mods", nullptr, ImGuiWindowFlags_NoDecoration);
    ImGui::Button("Mods tab");
    if (focus_tab) {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        focus_tab = false;
    }
    float bottom = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2;
    if (other_tab) ImGui::Button("a row of another tab");
    else mods_page(1.0f, bottom, focus_first);
    ImGui::Button("Continue");
    ImGui::End();
    ImGui::Render();
}

static void click(ImVec2 low, ImVec2 high) {
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent((low.x + high.x) / 2, (low.y + high.y) / 2);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    frame();
}

static void row(const char *id, const char *name, int on, const char *group = "", const char *group_name = "") {
    ModLibRow &r = test_view.rows[test_view.count++];
    memset(&r, 0, sizeof r);
    snprintf(r.id, sizeof r.id, "%s", id);
    snprintf(r.name, sizeof r.name, "%s", name);
    strcpy(r.kind, "romfs");
    r.installed = 1;
    r.enabled = on;
    snprintf(r.group, sizeof r.group, "%s", group);
    snprintf(r.group_name, sizeof r.group_name, "%s", group_name);
}

static void press(ImGuiKey key) {   // a key down for a frame and up for one, as the menu's controller mapping sends
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(key, true);
    frame();
    io.AddKeyEvent(key, false);
    frame();
}

static bool has_row(const char *label) {
    for (int i = 0; i < lib_row_count; i++)
        if (!strcmp(lib_rows[i], label)) return true;
    return false;
}

int main() {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60;
    io.IniFilename = nullptr;
    io.Fonts->Build();

    // three mods of their own and an entry in six parts, two of them on; one part's library.ini predates group_name
    row("alpha", "Alpha", 1);
    static const char *const parts[] = {"Probe tweak", "Treasure sensor up", "Earlier missions", "More multiguns",
                                        "Target appendages more", "Time attack and support mission tweaks"};
    for (int i = 0; i < 6; i++) {
        char id[65], name[96];
        snprintf(id, sizeof id, "tweaks.p%d", i);
        snprintf(name, sizeof name, "Tweaks mod: %s", parts[i]);
        row(id, name, i < 2, "tweaks", i == 5 ? "" : "Tweaks mod");
    }
    row("beta", "Beta", 0);
    row("gamma", "Gamma", 1);
    test_view.state = MODLIB_IDLE;
    test_view.generation = 1;
    frame();
    frame();
    verify(lib_group_count == 1, "the parts make one entry row");
    verify(lib_group_count && !strcmp(lib_groups[0], "Tweaks mod"), "the entry row is called by the entry's name");
    verify(lib_group_count && !strcmp(lib_group_state[0], "2 of 6 parts on"), "the entry row counts the parts on");
    verify(lib_row_count == 3, "the entry starts shut: only the mods of their own are rows");
    verify(!has_row("Probe tweak") && has_row("Alpha") && has_row("Gamma"), "shut, the parts are not drawn");

    click(lib_group_low[0], lib_group_high[0]);
    verify(lib_row_count == 9, "a click opens the entry on its six parts");
    verify(has_row("Probe tweak") && has_row("Time attack and support mission tweaks"),
           "the parts show without the entry's name in front, by group_name or by the name before \": \"");
    verify(!has_row("Tweaks mod: Probe tweak"), "no part keeps the entry's name in its row");
    click(lib_group_low[0], lib_group_high[0]);
    verify(lib_row_count == 3, "a second click shuts it");

    // a short list takes only its rows; a long one scrolls in a box no taller than its share of the page
    float short_box = lib_box_height;
    verify(short_box > 0 && short_box < lib_box_max, "a short list's box fits its rows");
    for (int i = 0; i < 40; i++) {
        char id[65], name[96];
        snprintf(id, sizeof id, "mod%02d", i);
        snprintf(name, sizeof name, "Mod %02d", i);
        row(id, name, 0);
    }
    test_view.generation++;
    frame();
    frame();
    verify(lib_box_height <= lib_box_max + 1 && lib_box_height > short_box, "a long list's box stops at its share");
    verify(lib_box_max <= 720 * 0.7f + 1, "the box's share is at most about two thirds of the page");

    // a controller (or the arrow keys) reaches the picked mod's On box from its row, even when the mod's text puts the
    // box well below the row: Right goes over to it, and pressing it switches the mod
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;   // as ui/menu.cpp says while a controller is connected
    snprintf(test_view.rows[0].description, sizeof test_view.rows[0].description, "%s",
             "A long description that wraps over several lines in the right-hand column, so the buttons under it sit "
             "well below the first rows of the list. It goes on for a while, as real mods' descriptions do, with more "
             "text about what it changes and how, and a note about what it needs to work, and still more after that.");
    test_view.generation++;
    switched[0] = 0;
    frame(true);   // arriving at the tab, as ui/menu.cpp asks for its first row
    frame();
    frame();
    verify(GImGui->NavWindow && strstr(GImGui->NavWindow->Name, "library_rows") && GImGui->NavCursorVisible &&
               !switched[0],
           "arriving at the tab highlights the picked mod's row, as the other tabs' first row, pressing nothing");
    // with the controller only, as in game
    auto step = [&](ImGuiKey key) { press(key); frame(); };   // the list is drawn before the pane: a frame to land
    switched[0] = 0;
    step(ImGuiKey_GamepadDpadRight);
    verify(GImGui->NavId == lib_on_id && !switched[0],
           "Right on the d-pad goes from the picked row to its On box, switching nothing");
    step(ImGuiKey_GamepadFaceDown);
    verify(!strcmp(switched, "alpha 0"), "and A presses it");
    step(ImGuiKey_GamepadDpadLeft);
    verify(GImGui->NavId != lib_on_id && GImGui->NavWindow && strstr(GImGui->NavWindow->Name, "library_rows"),
           "Left goes back to the list");
    step(ImGuiKey_GamepadDpadDown);   // the Tweaks entry's row
    step(ImGuiKey_GamepadDpadDown);   // Beta
    switched[0] = 0;
    step(ImGuiKey_GamepadDpadRight);
    step(ImGuiKey_GamepadFaceDown);
    verify(!strcmp(switched, "beta 1"), "Down to another mod, Right picks it and goes to its pane, A turns it on");
    // the arrow keys work the same way
    step(ImGuiKey_LeftArrow);
    switched[0] = 0;
    step(ImGuiKey_RightArrow);
    step(ImGuiKey_Space);
    verify(!strcmp(switched, "beta 1"), "the arrow keys and Space do the same");

    // switching to the tab with R1 from another tab: the picked row has the cursor, and Down goes to the next row, also
    // when the picked mod is the last row in view in the list box. Pick that one first: down until a row's line sits
    // at the box's bottom, then A.
    for (int k = 0; k < 20 && lib_focus_line_bottom < lib_box_bottom - 60; k++) step(ImGuiKey_GamepadDpadDown);
    step(ImGuiKey_GamepadFaceDown);
    other_tab = true;
    frame();
    frame();
    other_tab = false;
    frame(true);
    frame();
    ImGuiID arrived = GImGui->NavId;
    int at = 0;
    while (at < nlist && list_rows[at] != arrived) at++;
    verify(at < nlist - 1, "arriving with R1, the picked row has the cursor");
    step(ImGuiKey_GamepadDpadDown);
    verify(at < nlist - 1 && GImGui->NavId == list_rows[at + 1], "the first Down goes to the next row in the list");
    step(ImGuiKey_GamepadDpadUp);
    verify(GImGui->NavId == arrived, "and Up back to the picked row");
    // from the list's first row, Up leaves the list as ImGui's own move does (to the tabs)
    for (int k = 0; k < 60 && GImGui->NavId != list_rows[0]; k++) step(ImGuiKey_GamepadDpadUp);
    verify(GImGui->NavId == list_rows[0], "Up walks the list to its first row");
    verify(!strcmp(lib_pane_mod, "alpha"), "the pane shows the row the cursor is on, without A");
    step(ImGuiKey_GamepadDpadDown);   // the Tweaks entry's row: the pane stays on Alpha
    step(ImGuiKey_GamepadDpadDown);
    verify(!strcmp(lib_pane_mod, "beta"), "moving down to Beta shows Beta in the pane");
    step(ImGuiKey_GamepadDpadUp);
    step(ImGuiKey_GamepadDpadUp);
    step(ImGuiKey_GamepadDpadUp);
    verify(GImGui->NavWindow && !strstr(GImGui->NavWindow->Name, "library_rows"),
           "Up from the first row leaves the list");

    // arriving on a frame where the list can't take the focus yet (here: the library still loading, no rows), the
    // request holds and the picked row takes it once it is drawn
    other_tab = true;
    frame();
    frame();
    other_tab = false;
    int rows = test_view.count;
    test_view.count = 0;
    test_view.state = MODLIB_LOADING;
    test_view.generation++;
    frame(true);
    test_view.count = rows;
    test_view.state = MODLIB_IDLE;
    test_view.generation++;
    frame();
    frame();
    verify(GImGui->NavWindow && strstr(GImGui->NavWindow->Name, "library_rows") && GImGui->NavCursorVisible,
           "an arrival the list can't take at once still lands on the picked row");

    // Down from the menu's tab goes to the library's picked row, not to Continue under the page
    focus_tab = true;
    frame();
    frame();
    step(ImGuiKey_GamepadDpadDown);
    verify(GImGui->NavWindow && strstr(GImGui->NavWindow->Name, "library_rows"),
           "Down from the tab goes into the library list");

    // nothing in the library (offline) and no Lua mods: Down from the tab lands on Refresh; while it loads (Refresh
    // can't be pressed), ImGui's own move goes on to the menu's Continue instead of being swallowed
    rows = test_view.count;
    test_view.count = 0;
    test_view.generation++;
    focus_tab = true;
    frame();
    frame();
    step(ImGuiKey_GamepadDpadDown);
    frame();
    ImGuiWindow *page = ImGui::FindWindowByName("Mods");
    verify(GImGui->NavWindow && strstr(GImGui->NavWindow->Name, "##mods_page") && GImGui->NavCursorVisible,
           "an empty library: Down from the tab reaches its Refresh");
    test_view.state = MODLIB_LOADING;
    test_view.generation++;
    focus_tab = true;
    frame();
    frame();
    for (int k = 0; k < 3; k++) step(ImGuiKey_GamepadDpadDown);
    frame();
    verify(GImGui->NavWindow == page && GImGui->NavId == page->GetID("Continue"),
           "a loading, empty library: Down from the tab goes on to Continue");
    test_view.state = MODLIB_IDLE;
    test_view.count = rows;
    test_view.generation++;
    focus_tab = true;   // back into the list for the checks below
    frame();
    frame();
    step(ImGuiKey_GamepadDpadDown);

    // a title too long for the list's width takes two lines; a short one one
    row("longname", "A mod with a long name that runs on past the width of the list, as some real mods' names do", 0);
    test_view.generation++;
    frame();
    frame();
    verify(lib_wrapped == 1, "only the long title wraps to a second line");

    // going down the long list with the d-pad, each row's line under its name (On, Off) stays in view
    step(ImGuiKey_GamepadDpadLeft);   // back in the list
    bool line_shows = true;
    for (int k = 0; k < 30; k++) {
        step(ImGuiKey_GamepadDpadDown);
        line_shows &= lib_focus_line_bottom > 0 && lib_focus_line_bottom <= lib_box_bottom + 1;
    }
    verify(line_shows, "the d-pad down the list keeps each row's On / Off line in view");

    ImGui::DestroyContext();
    if (!failures) printf("ok %d\n", checks);
    return failures;
}
