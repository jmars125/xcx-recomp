// The in-game main menu's Data row (XCX DE 2.0.0 only; recomp/load_menu.py checks the build before the hooks exist).
//
// The menu's rows are a table of eight 0x30-byte entries (docs/menu-load-game.md): +0 and +8 a member function pointer
// (function, this adjustment), +0x10 a message id, +0x18 and +0x20 the child rows (pointer, count), +0x28 a flag byte.
// The Save row has no children and opens the Save screen when its handler is asked op 6. This file turns it into the
// Data row with two children, the original Save row and a Load row:
//   - parent: the plain parent handler the Intel row also uses; its label is our own text "Data";
//   - Save child: a copy of the original row, so it behaves exactly as before, with the game's own "Save" label;
//   - Load child: the same handler with a this adjustment of 1 (a marker, never a real object offset) and our own label
//     "Load". The entry hook on the handler sees the marker, answers op 6 by requesting the Save and Load screen in load
//     mode, and passes every other op to the original code with the real this.
// The game has no message that reads only "Data" or "Load", so the two rows keep a message id that no other row of
// the menu uses (it marks the row) and an entry hook on the menu's label setter writes our text instead, through the
// game's own text pane. Nothing is drawn by the host.
// The screen is the one the title's Load Game opens: the request is the one the title makes, with the mode byte 0.
// The title then starts the saved game by running the field script "fld_continue" once the request ends with a positive
// result. The menu does the same when the request it made ends the same way (op 4 asks "has the request ended?").
#include "host.h"
#include <stdatomic.h>
#include <string.h>

extern const char rt_game_name[];
#define MAIN 0x7000000000ull
#define ENTRY_SIZE 0x30
#define MAIN_ROWS 8
#define SAVE_ROW 6
#define SAVE_HANDLER (MAIN + 0x838fd8)
#define PARENT_HANDLER (MAIN + 0x838fc0)
#define SAVE_MESSAGE 0x3ca        // the Save row's own label
#define LOAD_MESSAGE 0x120f       // marks the Load row (the game's message for the title's Load Game; our label replaces its text)
#define PARENT_MESSAGE 0x60d      // marks the Data row (the game's message for a combined save and load title; our label replaces its text)
#define F_SHOW_ROW (MAIN + 0x560464)           // (row widget, shown): shows or hides a label row of the menu's list
#define F_SET_TEXT (MAIN + 0x91720c)           // (text pane, UTF-8): sets a text pane's text through the game's layouter
#define ROW_STRIDE 0xb8                        // the label rows of a list are 0xb8 bytes apart in the menu's view
#define MAIN_LIST_ROWS 0xb8                    // offset of the main list's first row in the view, and of the sub-list's:
#define SUB_LIST_ROWS 0x680
#define LABELS_OFFSET (2 * ENTRY_SIZE)         // our two texts follow the child rows in the same block
#define LABEL_DATA "Data"
#define LABEL_LOAD "Load"
#define LOAD_TAG 1                // this adjustment 2 (the entry stores adjustment << 1 | virtual bit): this = menu + 1
#define OP_CHOOSE 6
#define SCENE_MANAGER_GOT (MAIN + 0x1d54e40)   // GOT slot of the global holding the scene manager
#define F_REQUEST_SAVE_LOAD (MAIN + 0x928620)  // (manager, mode, a, b): the request the title's Load Game makes
#define OP_ENDED 4
#define RESULT_WORD (MAIN + 0x271fFa0)         // result of the last Save and Load request: 0 none, below 0 cancelled
#define F_RUN_FIELD_SCRIPT (MAIN + 0x12ebe4)   // (script name): what the title's Continue runs
#define CONTINUE_SCRIPT (MAIN + 0x1766356)     // "fld_continue"

static atomic_flag init_lock = ATOMIC_FLAG_INIT;
static atomic_uintptr_t children;   // the two child rows, process lifetime
static int load_pending;            // a Load request made from the menu has not ended yet (game thread only)

static int readable(uint64_t p, uint64_t n) {
    RtMemoryInfo i;
    return p && n && p <= UINT64_MAX - n && !mem_query(p, &i) && p >= i.base && p - i.base <= i.size && n <= i.size - (p - i.base) && (i.perm & 1);
}

// The Save row as the game builds it: no children, the Save handler, the Save message.
static int original_save_row(uint64_t row) {
    return readable(row, ENTRY_SIZE) && LD64(row) == SAVE_HANDLER && LD64(row + 8) == 0 && LD16(row + 0x10) == SAVE_MESSAGE &&
           LD64(row + 0x18) == 0 && LD32(row + 0x20) == 0;
}

// The child rows and, after them, the two label texts (guest memory, process lifetime).
static uint64_t child_rows(uint64_t save_row) {
    uint64_t b = atomic_load_explicit(&children, memory_order_acquire);
    if (b) return b;
    void *owned = mem_kernel_alloc(LABELS_OFFSET + 16, 16);
    if (!owned) return 0;
    b = (uint64_t)(uintptr_t)owned;
    memcpy(owned, (void *)(uintptr_t)save_row, ENTRY_SIZE);                       // Save: the original row
    memcpy((char *)owned + ENTRY_SIZE, (void *)(uintptr_t)save_row, ENTRY_SIZE);  // Load: the same handler, marked
    ST64(b + ENTRY_SIZE + 8, (uint64_t)LOAD_TAG << 1);
    ST16(b + ENTRY_SIZE + 0x10, LOAD_MESSAGE);
    memcpy((char *)owned + LABELS_OFFSET, LABEL_DATA "\0" LABEL_LOAD, sizeof LABEL_DATA + sizeof LABEL_LOAD);
    atomic_store_explicit(&children, (uintptr_t)b, memory_order_release);
    return b;
}

// Runs at the top of the menu's per-frame update, before the first list is built. A table already changed, or not
// the game's, is left alone, so the call is cheap after the first one for each table.
void rt_load_menu_update(uint64_t menu) {
    if (strcmp(rt_game_name, "2.0.0") || !readable(menu, 0x1568)) return;
    uint64_t rows = LD64(menu + 0x10c0);
    if (!rows || LD32(menu + 0x10c8) != MAIN_ROWS || !readable(rows, MAIN_ROWS * ENTRY_SIZE)) return;
    uint64_t row = rows + SAVE_ROW * ENTRY_SIZE;
    if (!original_save_row(row) || atomic_flag_test_and_set_explicit(&init_lock, memory_order_acquire)) return;
    uint64_t b = child_rows(row);
    if (b) {
        ST64(row, PARENT_HANDLER);
        ST64(row + 8, 0);
        ST16(row + 0x10, PARENT_MESSAGE);
        ST64(row + 0x18, b);
        ST32(row + 0x20, 2);
        ST8(row + 0x28, 0);   // like the other rows with children: the parent opens a list, it opens no screen itself
        rt_log("load menu: the Save row is now the Data row, holding Save and Load");
    }
    atomic_flag_clear_explicit(&init_lock, memory_order_release);
}

static uint64_t scene_manager(void) {
    uint64_t slot = LD64(SCENE_MANAGER_GOT);
    return readable(slot, 8) ? LD64(slot) : 0;
}

// Entry hook of the Save row's handler. Returns 1 when it has answered the call (the result is in x0).
int rt_load_menu_handler(Cpu *c) {
    uint64_t self = c->x[0];
    if ((self & 7) != LOAD_TAG) return 0;
    uint32_t op = (uint32_t)c->x[1];
    uint64_t menu = self - LOAD_TAG;
    if (op != OP_CHOOSE && op != OP_ENDED) {
        c->x[0] = menu;   // the original code gets the real menu object
        return 0;
    }
    uint64_t saved[19], lr = c->x[30], result = 1;
    memcpy(saved, c->x, sizeof saved);
    if (op == OP_CHOOSE) {
        uint64_t manager = scene_manager();
        if (manager) {
            rt_call(c, F_REQUEST_SAVE_LOAD, 4, manager, 0ull, 1ull, 0ull);   // rt_call reads every argument as 64 bits
            load_pending = 1;
        } else {
            rt_log("load menu: scene manager not found, Load ignored");
        }
    } else {
        result = rt_call(c, SAVE_HANDLER, 2, menu, (uint64_t)OP_ENDED);   // the original answer, with the real menu object
        if (load_pending && (result & 1)) {
            load_pending = 0;
            int32_t outcome = (int32_t)LD32(RESULT_WORD);
            rt_log("load menu: request ended, result %d", outcome);
            if (outcome > 0 && !memcmp((const void *)(uintptr_t)CONTINUE_SCRIPT, "fld_continue", 13)) {
                rt_call(c, F_RUN_FIELD_SCRIPT, 1, CONTINUE_SCRIPT);
                rt_log("load menu: started fld_continue");
            }
        }
    }
    memcpy(c->x, saved, sizeof saved);
    c->x[30] = lr;
    c->x[0] = result & 1;
    return 1;
}

// Entry hook of the menu's label setter, (view, level, row index, {text table, message id}). For the two rows this file
// adds it shows the row and sets our text; every other row, and a row that is hidden, is left to the game's code.
int rt_load_menu_label(Cpu *c) {
    uint64_t blocks = atomic_load_explicit(&children, memory_order_acquire), view = c->x[0], span = c->x[3];
    uint32_t level = (uint32_t)c->x[1], index = (uint32_t)c->x[2];
    // The descriptor is on the guest stack, where mem_query does not report readable memory; the game reads it too.
    if (!blocks || (level != 0 && level != 1) || !span || !LD64(span)) return 0;
    uint32_t message = LD16(span + 8);
    uint64_t text = blocks + LABELS_OFFSET;
    if (level == 0 && message == PARENT_MESSAGE) { /* Data */ }
    else if (level == 1 && message == LOAD_MESSAGE) text += sizeof LABEL_DATA;
    else return 0;
    uint64_t row = view + (level ? SUB_LIST_ROWS : MAIN_LIST_ROWS) + (uint64_t)index * ROW_STRIDE;
    if (!readable(row, ROW_STRIDE)) return 0;
    uint64_t saved[19], lr = c->x[30];
    memcpy(saved, c->x, sizeof saved);
    rt_call(c, F_SHOW_ROW, 2, row, 1ull);
    rt_call(c, F_SET_TEXT, 2, LD64(row + 0x10), text);
    memcpy(c->x, saved, sizeof saved);
    c->x[30] = lr;
    return 1;
}
