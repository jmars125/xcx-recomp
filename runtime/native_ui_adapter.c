// Guest side of the native Online-screen hooks for the pinned XCX DE 2.0.0 build. The generator (recomp/native_ui.py)
// puts `rt_native_ui_hook(id, c)` at the entry of five checked functions; every hook answers only when its caller is the
// exact call site named below and a mod holds the matching registration. Otherwise the original function runs. Guest
// access is synchronous on the calling guest thread and no guest pointer is kept.
#include "host.h"
#define MESSAGE_TEXT_STORAGE // test builds keep the one copy of the message row table here (runtime/message_text.h)
#include "message_text.h"
#include "mod_native_ui.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#if __has_include("native_ui_config.h")
#include "native_ui_config.h"
#else
#define NATIVE_UI_HOOKS 0
#endif
extern const char rt_game_name[];
#define MAIN 0x7000000000ull
enum { HOOK_STORY, HOOK_ONLINE, HOOK_FLAG, HOOK_SQUAD_IDLE, HOOK_SQUAD_INPUT, HOOK_SQUAD_STATE, HOOK_SQUAD_MESSAGE,
       HOOK_DETAILS_ONLINE, HOOK_DETAILS_TIMER, HOOK_DETAILS_CLOSE, HOOK_ONLINE_READY };
enum { INPUT_CONFIRM = 4 };
int rt_native_ui_capable(void) { return NATIVE_UI_HOOKS && !strcmp(rt_game_name, "2.0.0"); }
static int readable_writable(uint64_t p, uint64_t n) {
    RtMemoryInfo i;
    return p && n && p <= UINT64_MAX - n && !mem_query(p, &i) && p >= i.base && p - i.base <= i.size &&
           n <= i.size - (p - i.base) && (i.perm & 3) == 3;
}
// Nested guest calls use the caller's thread and stack, and restore the complete caller Cpu.
static uint64_t call(Cpu *c, uint64_t fn, int n, uint64_t a, uint64_t b, uint64_t d) {
    Cpu saved = *c;
    uint64_t r = rt_call(c, MAIN + fn, n, a, b, d);
    *c = saved;
    return r;
}
// Squad screen's idle input handler runs the original option code on Confirm; offline that code refuses or dereferences
// a missing online session. While a registration owns the screen, Confirm never reaches it: the pad is read without
// consuming input, a new press runs the option's mod action, and the handler is skipped for as long as Confirm is held.
// Navigation, Back and every other button still reach the original handler.
// Create card and join card (options 2 and 3) are not swallowed: the game's own handler raises its
// confirmation dialog (messages 0x12ef and 0x12f6) because the online query is answered for those two options, and the
// state handler hook below takes over once the player answers the dialog. Option 0 and 1 keep the unavailable message.
static int dialog_option(unsigned option) {
    return (option == MOD_NATIVE_CREATE_PRIVATE || option == MOD_NATIVE_PLAY_PRIVATE) && mod_native_ui_has_action(option);
}
// Squad details screen (Phase B prototype). After a mod reports success the game's own Details screen is opened with
// f_70008f09a8(parentUi, mode, passcode), as the original does after its create or join request. While that screen is up,
// three checked hooks stand in for the online session it would otherwise read: the "still online?" query, the countdown
// refresh (skipped: the text keeps the screen's initial value) and the close (logged; the notice manager's status that the
// button press set is cleared so squad screen is not left waiting on it). All three answer only while the adapter opened the
// screen, only for its squad screen view, and only at their one guarded call site.
extern uint64_t (*rt_native_ui_read64)(uint64_t); // defined below; the tests replace it
static void details_label_apply(const char *label);   // defined with the message rows below
static void details_label_restore(void);
static void details_status_apply(Cpu *c, uint64_t view);
static atomic_int details_open;
static atomic_int details_closing; // set by the close hook: the screen is still fading out, so the hooks keep answering until squad screen runs again
static atomic_uint_fast64_t details_view;
static char details_passcode[MOD_NATIVE_CODE_MAX + 1];
static int details_mode;
static unsigned details_timer_calls, details_input_calls;
static int details_enabled(void) { return (mod_native_ui_features() & MOD_NATIVE_DETAILS) != 0; }
// The game's own busy window: the original turns it on (f_700040f1dc) right after it sends its online
// request and off (f_700040f1ec) when the answer arrives; both take the object held at guest word 0x7001dfb608.
static int busy_shown;
static uint64_t busy_since;
// The mod answers within a frame on loopback; the window then stays up at least this long so it is seen (the tests set 0).
uint64_t rt_native_ui_busy_min_ns = 700000000ull;
static void busy_set(Cpu *c, int on) {
    if (on == busy_shown) return;
    uint64_t object = rt_native_ui_read64(0x7001dfb000ull + 0x608);
    if (!object) { rt_log("native ui: busy window not %s: no object", on ? "shown" : "hidden"); return; }
    call(c, on ? 0x40f1dc : 0x40f1ec, 1, object, 0, 0);
    busy_shown = on;
    if (on) busy_since = kern_now_ns();
    rt_log("native ui: busy window %s", on ? "shown" : "hidden");
}
static uint64_t message_manager(void) {
    uint64_t holder = rt_native_ui_read64(0x7001d54000ull + 0xe40);
    return holder ? rt_native_ui_read64(holder) : 0;
}
static void details_show(Cpu *c, uint64_t view, int create) {
    uint64_t parent = LD64(view + 0x88);
    ST8(view + 0x94, 0); // idle again, as the original's state 0x11 does after the open
    if (!readable_writable(parent, 0x10)) { rt_log("native ui: Details not opened: the squad screen parent object is not readable"); return; }
    // What the mod reported: its passcode and mode, or the mode the option implies and a placeholder.
    int mode = mod_native_ui_details(details_passcode, sizeof details_passcode);
    if (!details_passcode[0]) snprintf(details_passcode, sizeof details_passcode, "000000");
    details_mode = mode >= 0 ? mode : create ? MOD_NATIVE_MODE_CREATED : MOD_NATIVE_MODE_JOINED;
    { char label[MOD_NATIVE_LABEL_MAX + 1], status[MOD_NATIVE_STATUS_MAX + 1]; // the mod's own timer label, if it sent one
      mod_native_ui_details_text(label, sizeof label, status, sizeof status);
      details_label_apply(label); }
    // The original hides squad screen with its "out" animation and clears view+0xb2 (the "shown" flag) before the open, and its
    // state 0x11 brings it back with "in" afterwards; the close hook below does the second half.
    ST8(view + 0xb2, 0);
    call(c, 0x55f8d4, 2, view + 8, MAIN + 0x177b865, 0);
    call(c, 0x8f09a8, 3, parent, (uint64_t)details_mode, (uint64_t)(uintptr_t)details_passcode);
    call(c, 0x8f0c84, 1, parent, 0, 0);
    atomic_store(&details_view, view);
    atomic_store(&details_closing, 0);
    atomic_store(&details_open, 1);
    details_timer_calls = details_input_calls = 0;
    rt_log("native ui: Details screen opened (mode %d, passcode %s)", details_mode, details_passcode);
}
static int details_hook(unsigned id, Cpu *c) {
    int open = atomic_load(&details_open);
    if (!open && !atomic_load(&details_closing)) return 0;
    uint64_t caller = c->x[30];
    switch (id) {
    case HOOK_DETAILS_ONLINE:
        if (caller != MAIN + 0x715c84) return 0;
        c->x[0] = 1;
        return 1;
    case HOOK_DETAILS_TIMER:
        if (caller != MAIN + 0x715ca4) return 0;
        if (details_timer_calls++ == 0) rt_log("native ui: Details countdown refresh replaced by the mod's status text");
        details_status_apply(c, c->x[0]); // c->x[0] is the Details view; no text from the mod: the screen keeps its own
        return 1;
    case HOOK_DETAILS_CLOSE: {
        if (!open || caller != MAIN + 0x715d78) return 0;
        uint64_t view = c->x[19]; // the Details view (the caller's callee-saved register is still intact here)
        unsigned button = readable_writable(view, 0x98) ? LD32(view + 0x70) : ~0u;
        uint64_t manager = message_manager();
        unsigned before = manager ? (unsigned)call(c, 0x928744, 1, manager, 0, 0) : ~0u;
        // Button 0 is the start button (the return button in the in-squad mode); button 1 is the leave button. The original's Confirm has just
        // set the notice status to button + 1 (1 for the start or return button), and its squad screen state 0x11 reads that: a 1
        // sends the view to its exit state, so the game leaves the menu; any other status returns the view to idle.
        int leaves = button == 0;
        uint64_t squad = atomic_load(&details_view);
        int exits = leaves && readable_writable(squad, 0xc8);
        if (!leaves && manager) call(c, 0x928734, 2, manager, 0, 0); // Leave: clear it, squad screen comes back below
        atomic_store(&details_open, 0);
        atomic_store(&details_closing, 1); // the fade-out frames keep the mod's text instead of the game's own 00:00:00
        details_label_restore();
        if (exits) {
            // State 0x11 reads the status through the word at 0x7001dc1410; make sure that object carries the 1 as well.
            uint64_t holder = rt_native_ui_read64(0x7001dc1410ull);
            if (holder && holder != manager) call(c, 0x928734, 2, holder, 1, 0);
            ST8(squad + 0x94, 0x11); // the original code leaves squad screen from here
        } else if (readable_writable(squad, 0xc8)) { // Squad screen comes back, as the original's state 0x11 does
            ST8(squad + 0xb2, 1);
            call(c, 0x55f8d4, 2, squad + 8, MAIN + 0x178fc1b, 0);
        }
        unsigned action = button == 1 ? MOD_NATIVE_DETAILS_LEAVE : button == 0 && details_mode != MOD_NATIVE_MODE_IN_SQUAD ? MOD_NATIVE_DETAILS_START : ~0u;
        int sent = action != ~0u && mod_native_ui_confirm(action);
        rt_log("native ui: Details closed by button %u (notice manager status was %u, %s; %s; %s)", button, before,
               leaves ? "kept for the exit" : "cleared", exits ? "squad screen leaves (state 0x11)" : "squad screen returns",
               action == ~0u ? "no action" : sent ? "the mod was asked" : "the mod has no action for it");
        return 0; // the original close runs
    }
    }
    return 0;
}
static atomic_int confirm_down;
static atomic_uint_fast64_t confirm_view;
static int squad_input(Cpu *c) {
    uint64_t view = c->x[0];
    if (!readable_writable(view, 0xc8)) return 0;
    if (!atomic_load(&details_open)) atomic_store(&details_closing, 0); // Squad screen runs again: the Details screen is gone
    if (atomic_load(&details_open)) {
        if (atomic_load(&details_view) == view) { // the Details screen is on top: squad screen takes no input meanwhile
            if (details_input_calls < 3) rt_log("native ui: squad screen input hook still called while Details is open (call %u)", ++details_input_calls);
            return 1;
        }
        atomic_store(&details_open, 0); // a different squad screen view: the Details screen is gone
        details_label_restore();
    }
    uint32_t input = (uint32_t)call(c, 0x40b93c, 1, 0, 0, 0);
    if (!(input & INPUT_CONFIRM)) { atomic_store(&confirm_down, 0); return 0; }
    unsigned selected = LD32(view + 0x90);
    if (dialog_option(selected)) { atomic_store(&confirm_down, 0); return 0; }
    if (atomic_exchange(&confirm_view, view) != view) atomic_store(&confirm_down, 0);
    if (atomic_exchange(&confirm_down, 1)) return 1; // held; still skipped
    if (selected < MOD_NATIVE_OPTIONS && mod_native_ui_confirm(selected)) {
        // The original handler starts a Confirm with the button's "push" then "sel_loop" animation; replay only that so
        // the press is visible. The strings are the ones the original passes (+717560).
        call(c, 0x55faf4, 3, view + 0x48 + ((uint64_t)selected << 4), MAIN + 0x176ac54, MAIN + 0x178bee1);
        rt_log("native ui: squad screen option %u confirmed", selected);
    } else if (selected < 2) {
        // First card (0) and second card (1) with no mod action: let the game's own handler answer as Play with
        // Friends, which ends in its offline-refusal toast. First card's own original code
        // dereferences a missing online result and crashes offline. The nested call has a different caller than the
        // hook's guarded one, so it is not intercepted; the highlight index is restored afterwards.
        ST32(view + 0x90, 1);
        call(c, 0x717460, 1, view, 0, 0);
        ST32(view + 0x90, selected);
        // That handler also started card 1's press animation. When first card is the card the player is on, undo it
        // with the call the original makes on the card being left when the highlight moves (+717988: slot, string at
        // +1773a83), so only the card the player is on stays lit.
        if (selected != 1) call(c, 0x55f8d4, 2, view + 0x48 + (1u << 4), MAIN + 0x1773a83, 0);
        rt_log("native ui: squad screen option %u answered by the game's unavailable message", selected);
    } else {
        rt_log("native ui: squad screen option %u confirmed with no mod action", selected);
    }
    return 1;
}
// The squad screen view's state byte (view+0x94). 0 is idle. 0xb and 0xe wait for the answer to the Create and Play in
// Private squad dialogs; the game then sends its online request, which a mod cannot answer, so this takes over:
// Confirm runs the option's mod action and waits in 0xc (create) or 0xf (join) for the mod's report, then the view
// returns to idle on success or raises the game's own failure window on failure. Cancel returns to idle.
// Both failures use state 0x17: it posts one message window and, once the player answers it, the original state 0xf code
// returns the view to idle. State 0x16 (the offline-fallback join failure) is not used because after
// its window the game leaves squad screen altogether (it ends in the "exit" state 1 and sets view+0x95). A failed Join
// shows a different message than 0x12f5: the hook on the message post below swaps the id for that one window.
enum { STATE_IDLE = 0, STATE_CREATE_DIALOG = 0xb, STATE_CREATE_WAIT = 0xc, STATE_JOIN_DIALOG = 0xe, STATE_JOIN_WAIT = 0xf,
       STATE_KEYBOARD = 0x12, STATE_FAILED = 0x17, WAIT_FRAMES = 60 * 20 };
// Messages of the game's own table: 0x12f5 is what state 0x17 posts. When a mod reports a failure with its own text, the text
// goes into one of a small pool of spare rows of the English table the game holds in guest memory (runtime/message_text.h)
// and the failure window shows that row's id instead. If the rows cannot be found (another language, a modified table) the
// game's own text is used: 0x12f5 (create-failed) for Host, 0x156c (squad-not-found) for Join.
enum { MESSAGE_CREATE_FAILED = 0x12f5, MESSAGE_SQUAD_NOT_FOUND = 0x156c };
static unsigned failure_message; // message id for the next failure window, 0 = the game's own text

// Where the guest memory to search is. The next scannable region (normal memory the game can read and write) at or after
// `after`; a hook so the synthetic fixture can supply a small fake heap instead of the whole address space.
enum { MEMORY_STATE_NORMAL = 5, ROW_COPIES = 8, ROW_SCANS = 4 };
static int next_region(uint64_t after, uint64_t *base, uint64_t *size) {
    while (after < (1ull << 39)) {
        RtMemoryInfo i;
        if (mem_query(after, &i) || i.base + i.size <= after) return 0;
        if (i.state == MEMORY_STATE_NORMAL && (i.perm & 3) == 3) {
            *base = after > i.base ? after : i.base;
            *size = i.base + i.size - *base;
            return 1;
        }
        after = i.base + i.size;
    }
    return 0;
}
int (*rt_native_ui_next_region)(uint64_t, uint64_t *, uint64_t *) = next_region;
static uint64_t row_at[MESSAGE_ROWS][ROW_COPIES];       // guest addresses of every copy of each row
static unsigned row_copies[MESSAGE_ROWS], row_scans, row_clock, row_used[MESSAGE_ROWS];
static char row_text[MESSAGE_ROWS][MOD_NATIVE_TEXT_MAX + 1]; // what each row shows now; "" = the game's original text
// The original bytes of each row, copied from the game's table when the row was found (its length and hash matched), so the row can
// be put back. They live in memory only: the repository holds no game text, only each row's length and hash (runtime/message_text.h).
static char row_original[MESSAGE_ROWS][MESSAGE_ORIGINAL_MAX];
static int row_original_known[MESSAGE_ROWS];
// Locates the pool's rows: the game's tables are found by their shape and each row by its ID, then checked by length and hash.
// Copies found earlier are checked and dropped when they no longer hold what was written (the table was reloaded); a search of
// memory happens only while a row has no live copy, and at most ROW_SCANS times, so another language or a modified table costs a
// few searches and no more.
static void ensure_rows(void) {
    for (unsigned k = 0; k < MESSAGE_ROWS; k++) {
        unsigned live = 0, length = message_text_row(k)->length;
        for (unsigned i = 0; i < row_copies[k]; i++)
            if (readable_writable(row_at[k][i], length + 1) && row_original_known[k] &&
                message_text_holds((const uint8_t *)(uintptr_t)row_at[k][i], row_original[k], length, row_text[k]))
                row_at[k][live++] = row_at[k][i];
        if (!live) row_text[k][0] = 0; // a copy found later is the game's original again
        row_copies[k] = live;
    }
    unsigned missing = 0;
    for (unsigned k = 0; k < MESSAGE_ROWS; k++) missing += !row_copies[k];
    if (!missing || row_scans >= ROW_SCANS) return;
    row_scans++;
    uint64_t found[MESSAGE_ROWS][ROW_COPIES];
    unsigned have[MESSAGE_ROWS] = {0};
    uint64_t base, size, after = 0;
    while (rt_native_ui_next_region(after, &base, &size)) {
        for (unsigned k = 0; k < MESSAGE_ROWS; k++) {
            if (row_copies[k]) continue;
            uint64_t at[ROW_COPIES];
            unsigned n = 0;
            message_text_find((const uint8_t *)(uintptr_t)base, size, message_text_row(k), at, ROW_COPIES - have[k], &n);
            for (unsigned i = 0; i < n; i++) found[k][have[k]++] = base + at[i];
        }
        if (base + size <= after) break;
        after = base + size;
    }
    for (unsigned k = 0; k < MESSAGE_ROWS; k++) {
        for (unsigned i = 0; i < have[k]; i++) row_at[k][row_copies[k]++] = found[k][i];
        if (have[k] && !row_original_known[k] && message_text_row(k)->length < sizeof row_original[k]) { // the bytes that matched the hash
            memcpy(row_original[k], (const void *)(uintptr_t)found[k][0], message_text_row(k)->length);
            row_original[k][message_text_row(k)->length] = 0;
            row_original_known[k] = 1;
        }
    }
    rt_log("native ui: message rows found: %u, %u and %u copies, toast row %u", row_copies[0], row_copies[1], row_copies[2],
           row_copies[MESSAGE_TOAST_ROW]);
}
// The message id of a row that shows `text`: a row that already shows it, else the least recently used row, rewritten in
// every copy. 0 when no row could be found.
static unsigned custom_message_id(const char *text) {
    ensure_rows();
    int pick = -1;
    for (unsigned k = 0; k < MESSAGE_POOL_ROWS; k++) // a row that already shows the text
        if (row_copies[k] && !strcmp(row_text[k], text)) pick = (int)k;
    if (pick < 0) // else the least recently used row (a row never used counts as oldest)
        for (unsigned k = 0; k < MESSAGE_POOL_ROWS; k++)
            if (row_copies[k] && (pick < 0 || row_used[k] < row_used[(unsigned)pick])) pick = (int)k;
    if (pick < 0) return 0;
    unsigned k = (unsigned)pick;
    if (strcmp(row_text[k], text)) {
        for (unsigned i = 0; i < row_copies[k]; i++) message_text_write((uint8_t *)(uintptr_t)row_at[k][i], message_text_row(k)->length, text);
        snprintf(row_text[k], sizeof row_text[k], "%s", text);
    }
    row_used[k] = ++row_clock;
    return message_text_row(k)->id;
}
// Reads one guest word. A hook so the synthetic fixture, which has no guest address space, can supply the dialog holder.
static uint64_t read_guest_word(uint64_t address) { return LD64(address); }
uint64_t (*rt_native_ui_read64)(uint64_t) = read_guest_word;
// A toast with the mod's own words. The game raises its red offline-refusal toast by id: the squad screen input
// handler calls f_7000928f20(manager, 0x241) at +7178b8 (manager = *(*(0x7001d54000 + 0xe40)), the same object that receives its
// dialogs) and the toast shows row 0x23e of table 98 of the message file. A mod's text goes into that row, in the table copy the
// game holds in guest memory (as for the failure window), the toast is requested the same way, and the row is put back after
// TOAST_HOLD_NS of real time. The request only queues the id on the manager; the text is read when the toast is shown, so the
// row must stay until then. Only one toast at a time; a text asked for meanwhile waits. If the row cannot be found the request
// is dropped. The clock is real time, not frames: the squad screen update calls the state hook only outside idle (state 0),
// so a frame count stalled after the first run (owner run 5, 2026-11-12); the idle hook ticks too.
enum { TOAST_ID = 0x241 };
static const uint64_t TOAST_HOLD_NS = 4000000000ull; // owner run 6: the toast stays up 2-3 s; 4 s keeps a margin
static uint64_t toast_until; // kern_now_ns() at which the row is put back; 0 = no toast showing
static void toast_restore(void) {
    for (unsigned i = 0; i < row_copies[MESSAGE_TOAST_ROW]; i++)
        if (readable_writable(row_at[MESSAGE_TOAST_ROW][i], message_text_row(MESSAGE_TOAST_ROW)->length + 1))
            message_text_restore((uint8_t *)(uintptr_t)row_at[MESSAGE_TOAST_ROW][i], row_original[MESSAGE_TOAST_ROW],
                                 message_text_row(MESSAGE_TOAST_ROW)->length);
    row_text[MESSAGE_TOAST_ROW][0] = 0;
}
// Puts the row back once the hold has passed. Needs no registration: a withdrawn owner still gets its row restored.
static int toast_expire(void) {
    if (!toast_until || (int64_t)(kern_now_ns() - toast_until) < 0) return 0;
    toast_until = 0;
    toast_restore();
    rt_log("native ui: toast row restored");
    return 1;
}
static void toast_tick(Cpu *c) {
    if (toast_until) { toast_expire(); return; }
    char text[MESSAGE_TOAST_MAX + 1];
    if (!mod_native_ui_take_toast(text, sizeof text)) return;
    ensure_rows();
    unsigned k = MESSAGE_TOAST_ROW;
    if (!row_copies[k]) { rt_log("native ui: toast dropped: the toast row was not found"); return; }
    uint64_t holder = rt_native_ui_read64(0x7001d54000ull + 0xe40);
    uint64_t manager = holder ? rt_native_ui_read64(holder) : 0;
    if (!manager) { rt_log("native ui: toast dropped: no message manager"); return; }
    for (unsigned i = 0; i < row_copies[k]; i++) message_text_write((uint8_t *)(uintptr_t)row_at[k][i], message_text_row(k)->length, text);
    snprintf(row_text[k], sizeof row_text[k], "%s", text);
    call(c, 0x928f20, 2, manager, TOAST_ID, 0);
    toast_until = kern_now_ns() + TOAST_HOLD_NS;
    rt_log("native ui: toast shown");
}
// The Details screen's timer: the timer label is a row of the message table, borrowed the way the toast row is and put
// back when the screen closes; the countdown text is a pane the screen refreshes every frame from its own timer function, which the
// hook replaces: when the mod sent a status text, the same calls the original makes to fill the pane (a variant holding the text,
// the pane found by name in the screen's layout, the pane's text setter) are made with that text.
static void details_label_apply(const char *label) {
    if (!label[0]) return;
    ensure_rows();
    unsigned k = MESSAGE_LABEL_ROW;
    if (!row_copies[k]) { rt_log("native ui: Details label not set: the timer label row was not found"); return; }
    for (unsigned i = 0; i < row_copies[k]; i++) message_text_write((uint8_t *)(uintptr_t)row_at[k][i], message_text_row(k)->length, label);
    snprintf(row_text[k], sizeof row_text[k], "%s", label);
}
static void details_label_restore(void) {
    unsigned k = MESSAGE_LABEL_ROW;
    if (!row_text[k][0]) return;
    for (unsigned i = 0; i < row_copies[k]; i++)
        if (readable_writable(row_at[k][i], message_text_row(k)->length + 1))
            message_text_restore((uint8_t *)(uintptr_t)row_at[k][i], row_original[k], message_text_row(k)->length);
    row_text[k][0] = 0;
}
static char details_status_text[MESSAGE_STATUS_MAX + 1];
static _Alignas(16) uint8_t details_variant[0x80]; // the game's text variant (the original keeps it in a 0x48-byte stack frame)
static void details_status_apply(Cpu *c, uint64_t view) {
    char label[MESSAGE_LABEL_MAX + 1];
    mod_native_ui_details_text(label, sizeof label, details_status_text, sizeof details_status_text);
    if (!details_status_text[0] || !readable_writable(view, 0x30)) return;
    uint64_t variant = (uint64_t)(uintptr_t)details_variant;
    call(c, 0x926058, 1, variant, 0, 0);                                    // the variant, empty
    call(c, 0x9262cc, 2, variant, (uint64_t)(uintptr_t)details_status_text, 0); // ... holding the text
    uint64_t pane = call(c, 0x55f16c, 2, view + 0x28, MAIN + 0x1784196, 0); // the countdown pane, found by name in the layout
    uint64_t setter = call(c, 0x915110, 1, pane, 0, 0);
    call(c, 0x9172a0, 2, setter, variant, 0);                               // the pane's text setter
    call(c, 0x926214, 1, variant, 0, 0);                                    // the variant released
}
static int waiting_option = -1;
static unsigned waiting_frames;
// The Join passcode. A mod that registered `details` gets the game's own keyboard: after the Join dialog is confirmed the original
// asks for it with f_70008f0c08(parentUi) and moves to its state 0x12, which polls parentUi+0x182 (0 = open, 1 = text entered,
// 2 = cancelled) and then reads the passcode string at parentUi+0x184 for its online request. The adapter makes the same request and
// owns state 0x12 itself (the original's next states need the online client), then hands the typed text to the mod's join action as
// the field `text` of its ui.action event. The recomp's own software keyboard (runtime/swkbd*.c) is what shows the text field.
static uint64_t keyboard_view;
static int local_failure; // the typed passcode cannot name a squad: the wait ends as a failure, after the same busy window
static void keyboard_start(Cpu *c, uint64_t view, uint64_t parent) {
    call(c, 0x8f0c08, 1, parent, 0, 0);
    ST8(view + 0x94, STATE_KEYBOARD);
    keyboard_view = view;
    rt_log("native ui: passcode keyboard requested");
}
static int keyboard_poll(Cpu *c, uint64_t view) {
    uint64_t parent = LD64(view + 0x88);
    if (!readable_writable(parent, 0x1a0)) { keyboard_view = 0; ST8(view + 0x94, STATE_IDLE); return 1; }
    unsigned result = LD8(parent + 0x182);
    if (!result) return 1; // the keyboard is open
    keyboard_view = 0;
    if (result != 1) { ST8(view + 0x94, STATE_IDLE); rt_log("native ui: passcode keyboard cancelled"); return 1; }
    char text[MOD_NATIVE_CODE_MAX + 1];
    unsigned n = 0;
    for (; n < MOD_NATIVE_CODE_MAX; n++) { char ch = (char)LD8(parent + 0x184 + n); if (!ch) break; text[n] = ch; }
    text[n] = 0;
    if (!mod_native_ui_code_valid(text)) {
        // Nothing the mod could join (empty, or characters a passcode cannot have). The game's server would answer "not found" after
        // its busy window like for any other code, so this takes the same road: the wait ends as a failure by itself.
        mod_native_ui_begin(); // no text left over from an earlier answer
        local_failure = 1;
        waiting_option = MOD_NATIVE_PLAY_PRIVATE;
        waiting_frames = 0;
        busy_set(c, 1);
        ST8(view + 0x94, STATE_JOIN_WAIT);
        rt_log("native ui: passcode typed (%s) cannot be a passcode; it will end as not found", text);
        return 1;
    }
    ensure_rows();
    mod_native_ui_begin();
    if (mod_native_ui_confirm_text(MOD_NATIVE_PLAY_PRIVATE, text)) {
        waiting_option = MOD_NATIVE_PLAY_PRIVATE;
        waiting_frames = 0;
        busy_set(c, 1);
        ST8(view + 0x94, STATE_JOIN_WAIT);
        rt_log("native ui: passcode typed (%s); the mod was asked to join", text);
    } else {
        ST8(view + 0x94, STATE_IDLE);
        rt_log("native ui: passcode typed but the mod did not accept the join");
    }
    return 1;
}
static int squad_state(Cpu *c) {
    uint64_t view = c->x[0];
    if (!readable_writable(view, 0xc8)) return 0;
    toast_tick(c);
    unsigned state = LD8(view + 0x94);
    if (state == STATE_CREATE_DIALOG || state == STATE_JOIN_DIALOG) {
        unsigned option = state == STATE_CREATE_DIALOG ? MOD_NATIVE_CREATE_PRIVATE : MOD_NATIVE_PLAY_PRIVATE;
        // The same two queries the original makes on the dialog (+716678, +716688): the global dialog holder is the
        // guest word at 0x7001dc1410. The first reports an answer (0 until answered), the second consumes it.
        uint64_t holder = rt_native_ui_read64(0x7001dc1410ull);
        int32_t answer = (int32_t)call(c, 0x92a0d4, 1, holder, 0, 0);
        if (!answer) return 1; // still open
        call(c, 0x92a0a4, 1, holder, 0, 0);
        if (answer == 1 && option == MOD_NATIVE_PLAY_PRIVATE && details_enabled() && !mod_native_ui_session_active()) { // Join: the game's own keyboard asks for the passcode first (not while a session is up)
            uint64_t parent = LD64(view + 0x88);
            if (readable_writable(parent, 0x1a0)) { keyboard_start(c, view, parent); return 1; }
        }
        if (answer == 1) { // Confirm
            ensure_rows(); // find the message rows now, before the mod answers (the first search can take a moment)
            mod_native_ui_begin();
            if (mod_native_ui_confirm(option)) {
                waiting_option = (int)option;
                waiting_frames = 0;
                if (details_enabled()) busy_set(c, 1);
                ST8(view + 0x94, option == MOD_NATIVE_CREATE_PRIVATE ? STATE_CREATE_WAIT : STATE_JOIN_WAIT);
                rt_log("native ui: squad screen option %u confirmed in the game's dialog", option);
            } else {
                ST8(view + 0x94, STATE_IDLE);
                rt_log("native ui: squad screen option %u confirmed but the mod did not accept it", option);
            }
        } else if (answer == -1 || answer == 2) { // Cancel or Back
            ST8(view + 0x94, STATE_IDLE);
            rt_log("native ui: squad screen option %u dialog cancelled", option);
        }
        return 1;
    }
    if (state == STATE_KEYBOARD && keyboard_view == view) return keyboard_poll(c, view);
    if ((state == STATE_CREATE_WAIT || state == STATE_JOIN_WAIT) && waiting_option >= 0) {
        unsigned result = local_failure ? MOD_NATIVE_FAILED : mod_native_ui_result();
        if (result == MOD_NATIVE_PENDING && ++waiting_frames < WAIT_FRAMES) return 1;
        if (result != MOD_NATIVE_PENDING && busy_shown && (int64_t)(kern_now_ns() - busy_since - rt_native_ui_busy_min_ns) < 0)
            return 1; // answered already, but the busy window has not been up long enough to be seen
        local_failure = 0;
        busy_set(c, 0); // the answer (or the wait running out) ends the window
        int create = waiting_option == MOD_NATIVE_CREATE_PRIVATE;
        waiting_option = -1;
        if (result == MOD_NATIVE_OK) {
            ST8(view + 0x94, STATE_IDLE);
            rt_log("native ui: %s reported ready", create ? "host" : "join");
            if (details_enabled()) details_show(c, view, create);
        } else { // reported failure, or no answer in time
            char text[MOD_NATIVE_TEXT_MAX + 1];
            mod_native_ui_message(text, sizeof text);
            unsigned id = text[0] ? custom_message_id(text) : 0;
            failure_message = id ? id : create ? 0 : MESSAGE_SQUAD_NOT_FOUND;
            ST8(view + 0x94, STATE_FAILED);
            rt_log("native ui: %s %s; showing the game's failure message", create ? "host" : "join",
                   result == MOD_NATIVE_FAILED ? "failed" : "did not answer");
        }
        return 1;
    }
    return 0;
}
// The message post inside state 0x17 (+716cb8) carries the id in the 16-bit field at +8 of its argument. When a failed
// Join is being shown, the id is replaced once; the call itself still runs.
// The argument is a local of the calling frame, so it is validated against the thread's own stack pointer (the memory
// query does not report guest thread stacks as writable).
static void squad_message(Cpu *c) {
    unsigned replacement = failure_message;
    failure_message = 0;
    uint64_t arg = c->x[1];
    if (!replacement) return;
    if (!arg || (arg & 1) || arg < c->sp || arg - c->sp > 0x1000) {
        rt_log("native ui: failure message post: argument %#llx is not in the caller's frame", (unsigned long long)arg);
        return;
    }
    unsigned id = LD16(arg + 8);
    rt_log("native ui: failure message post: id %#x replaced by %#x%s", id, replacement,
           id == MESSAGE_CREATE_FAILED ? "" : " (skipped: unexpected id)");
    if (id == MESSAGE_CREATE_FAILED) ST16(arg + 8, replacement);
}
// The distinct callers of the game's online check seen while a session was up (a log of who asks, at most 64).
static void online_caller_note(const char *query, uint64_t caller) {
    static uint64_t seen[64];
    static unsigned count;
    uint64_t key = caller ^ (query[0] == 's' ? 1ull << 63 : 0); // the two queries are listed separately
    for (unsigned i = 0; i < count; ++i) if (seen[i] == key) return;
    if (count >= sizeof seen / sizeof *seen) return;
    seen[count++] = key;
    rt_log("native ui: the %s is asked by main+%#llx while a session is up (answered yes)", query, (unsigned long long)(caller - MAIN));
}
int rt_native_ui_hook(unsigned id, Cpu *c) {
    if (!rt_native_ui_capable()) return 0;
    if (id == HOOK_ONLINE_READY) {
        // The online parameter object's "ready" query that every main-menu Online row's validator makes after the session query.
        if (!(mod_native_ui_features() & MOD_NATIVE_SQUAD_SELECT) || !mod_native_ui_session_active()) return 0;
        online_caller_note("ready query", c->x[30]);
        c->x[0] = 1;
        return 1;
    }
    if (id == HOOK_DETAILS_ONLINE) {
        if (details_hook(id, c)) return 1;
        // The game's "am I in an online squad" query (the Online menu, the player-list row, the Connect entry and others ask it). While
        // the mod says a session is up every caller is told yes, not only the Details screen's own call.
        if (!(mod_native_ui_features() & MOD_NATIVE_SQUAD_SELECT) || !mod_native_ui_session_active()) return 0;
        online_caller_note("session query", c->x[30]);
        c->x[0] = 1;
        return 1;
    }
    if (id >= HOOK_DETAILS_ONLINE) return details_hook(id, c);
    unsigned features = mod_native_ui_features();
    if (!features) return 0;
    uint64_t caller = c->x[30];
    switch (id) {
    case HOOK_STORY:
        // Only the story condition inside connect entry's enabled check (+838324) is answered.
        if (!(features & MOD_NATIVE_ONLINE_ENTRY) || c->x[0] != 0x2280 || c->x[1] != 0 || caller != MAIN + 0x8383e8) return 0;
        c->x[0] = 1;
        return 1;
    case HOOK_ONLINE: // the connect state machine's state 14 online check
        if (!(features & MOD_NATIVE_SQUAD_SELECT)) return 0;
        if (caller == MAIN + 0x7174a4) { // Squad screen's input handler: yes for the two options with a mod action
            uint64_t view = c->x[19];
            if (!readable_writable(view, 0xc8) || !dialog_option(LD32(view + 0x90))) return 0;
            c->x[0] = 1;
            return 1;
        }
        if (caller != MAIN + 0x78b8e4) {
            // While the mod says a session is up, every other caller of the game's online check is told "yes" as well, so the
            // screens that ask it (the player-list row and the other online menus) see the game online. Each distinct caller is logged.
            if (!mod_native_ui_session_active()) return 0;
            online_caller_note("online check", caller);
        }
        c->x[0] = 1;
        return 1;
    case HOOK_FLAG: // the named flag the connect state machine's state 18 tests before building squad screen
        if (!(features & MOD_NATIVE_SQUAD_SELECT) || caller != MAIN + 0x78bab4) return 0;
        atomic_store(&details_open, 0); // a new squad screen view is being built: no Details screen belongs to it
        atomic_store(&details_closing, 0);
        { uint64_t manager = message_manager(); if (manager) call(c, 0x928734, 2, manager, 0, 0); } // an earlier start left status 1
        details_label_restore();
        c->x[0] = 1;
        return 1;
    case HOOK_SQUAD_IDLE: // Squad screen's per-frame idle connection check
        toast_expire(); // before the registration test: a withdrawn owner's toast row is still put back
        if (!(features & MOD_NATIVE_SQUAD_SELECT) || caller != MAIN + 0x71632c) return 0;
        toast_tick(c); // the state hook is not called in idle (state 0), so the toast is driven from here too
        c->x[0] = 1;
        return 1;
    case HOOK_SQUAD_INPUT:
        if (!(features & MOD_NATIVE_SQUAD_SELECT) || caller != MAIN + 0x716338) return 0;
        return squad_input(c);
    case HOOK_SQUAD_STATE:
        toast_expire();
        if (!(features & MOD_NATIVE_SQUAD_SELECT) || caller != MAIN + 0x71631c) return 0;
        return squad_state(c);
    case HOOK_SQUAD_MESSAGE:
        if (!(features & MOD_NATIVE_SQUAD_SELECT)) return 0;
        if (caller != MAIN + 0x716cb8) return 0;
        squad_message(c);
        return 0; // never answers: the original post runs with the (possibly replaced) id
    }
    return 0;
}
