// Host test of runtime/load_menu.c on synthetic memory: the row table, the handler hook and the guest calls it makes.
// No game bytes and no generated code. Build: tests/menu/load_menu_check.py (once with the game name 2.0.0, once with another).
#define _GNU_SOURCE
#include "host.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

const char rt_game_name[] = TEST_GAME;
#define MAIN 0x7000000000ull
#define ROWS (MAIN + 0x2763140)
#define SAVE_HANDLER (MAIN + 0x838fd8)
#define PARENT_HANDLER (MAIN + 0x838fc0)
#define REQUEST (MAIN + 0x928620)
#define RUN_SCRIPT (MAIN + 0x12ebe4)
#define SCRIPT_NAME (MAIN + 0x1766356)
#define MANAGER_GOT (MAIN + 0x1d54e40)
#define RESULT_WORD (MAIN + 0x271fFa0)
#define SHOW_ROW (MAIN + 0x560464)
#define SET_TEXT (MAIN + 0x91720c)

// ---- the host services load_menu.c uses
static int allocs;
static char logbuf[256];
void rt_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vsnprintf(logbuf, sizeof logbuf, fmt, a); va_end(a); }
static uint64_t stack_lo, stack_hi;   // the guest stack is not reported as readable memory by mem_query (as in the game)
uint32_t mem_query(uint64_t p, RtMemoryInfo *i) {
    if (p >= stack_lo && p < stack_hi) return 1;
    i->base = p & ~0xfffull; i->size = 0x1000 - (p & 0xfff) + (p & ~0xfffull) - i->base + 0x100000; i->perm = 3; return 0; }
void *mem_kernel_alloc(uint64_t n, uint64_t align) { (void)align; assert(n == 0x70); allocs++; return aligned_alloc(16, 0x1000); }

// ---- recorded guest calls
typedef struct { uint64_t fn, a0, a1, a2, a3; int n; } Call;
static Call calls[8];
static int ncalls;
static uint64_t ended_answer;   // what the original handler answers to op 4
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    Call *k = &calls[ncalls++];
    va_list a; va_start(a, n);
    k->fn = fn; k->n = n;
    k->a0 = n > 0 ? va_arg(a, uint64_t) : 0; k->a1 = n > 1 ? va_arg(a, uint64_t) : 0;
    k->a2 = n > 2 ? va_arg(a, uint64_t) : 0; k->a3 = n > 3 ? va_arg(a, uint64_t) : 0;
    va_end(a);
    for (int i = 0; i < 19; i++) c->x[i] = 0xdead0000 + i;   // a guest call clobbers the caller-saved registers
    c->x[30] = 0xbad;
    return fn == SAVE_HANDLER ? ended_answer : 0;
}

static void map(uint64_t p, size_t n) { assert(mmap((void *)p, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)p); }
static uint8_t menu[0x1600] __attribute__((aligned(16)));

static void build_rows(void) {
    memset((void *)ROWS, 0, 8 * 0x30);
    for (int i = 0; i < 8; i++) { ST64(ROWS + i * 0x30, MAIN + 0x838c00 + i * 0x10); ST16(ROWS + i * 0x30 + 0x10, 0x3a0 + i); }
    uint64_t save = ROWS + 6 * 0x30;
    ST64(save, SAVE_HANDLER); ST64(save + 8, 0); ST16(save + 0x10, 0x3ca); ST8(save + 0x28, 1);
    memset(menu, 0, sizeof menu);
    ST64((uint64_t)menu + 0x10c0, ROWS); ST32((uint64_t)menu + 0x10c8, 8);
}

static Cpu cpu(uint64_t x0, uint64_t op) {
    Cpu c = {0};
    c.x[0] = x0; c.x[1] = op; c.x[30] = 0x1234;
    for (int i = 2; i < 19; i++) c.x[i] = 0x5000 + i;
    return c;
}

int main(void) {
    char here; stack_lo = (uint64_t)(uintptr_t)&here - 0x100000; stack_hi = (uint64_t)(uintptr_t)&here + 0x100000;
    map(MAIN + 0x2762000, 0x2000); map(MAIN + 0x1d54000, 0x1000); map(MAIN + 0x271f000, 0x1000); map(MAIN + 0x1766000, 0x1000);
    memcpy((void *)SCRIPT_NAME, "fld_continue", 13);
    uint64_t manager_var = (uint64_t)(uintptr_t)aligned_alloc(16, 64), manager = 0x4d3690000ull;
    ST64(MANAGER_GOT, manager_var); ST64(manager_var, manager);
    build_rows();
    uint64_t save = ROWS + 6 * 0x30, original[6];
    memcpy(original, (void *)save, sizeof original);
    uint8_t others[6 * 0x30]; memcpy(others, (void *)ROWS, sizeof others);

    rt_load_menu_update((uint64_t)menu);
    if (strcmp(rt_game_name, "2.0.0")) {   // another game version: nothing changes
        assert(!memcmp((void *)save, original, sizeof original) && !allocs);
        puts("load menu runtime: other version left alone");
        return 0;
    }

    // ---- the table: parent row, two child rows, nothing else touched
    assert(allocs == 1);
    assert(LD64(save) == PARENT_HANDLER && LD64(save + 8) == 0 && LD16(save + 0x10) == 0x60d && LD32(save + 0x20) == 2 && LD8(save + 0x28) == 0);
    uint64_t kids = LD64(save + 0x18);
    assert(kids);
    assert(!memcmp((void *)kids, original, 0x30));   // child 0 is the original Save row, byte for byte
    assert(LD64(kids + 0x30) == SAVE_HANDLER && LD64(kids + 0x38) == 2 && LD16(kids + 0x40) == 0x120f && LD8(kids + 0x58) == 1);
    assert(!memcmp((void *)ROWS, others, sizeof others));
    assert(!strcmp((char *)(kids + 0x60), "Data") && !strcmp((char *)(kids + 0x65), "Load"));   // the two labels follow the child rows
    assert(!memcmp((void *)(ROWS + 7 * 0x30), (void *)(ROWS + 7 * 0x30), 0x30) && LD32((uint64_t)menu + 0x10c8) == 8);
    rt_load_menu_update((uint64_t)menu);   // a table already changed is left alone, nothing is allocated again
    assert(allocs == 1 && LD64(save + 0x18) == kids);
    build_rows();                            // a new menu object with the game's table again: the same child rows are reused
    rt_load_menu_update((uint64_t)menu);
    assert(allocs == 1 && LD64(ROWS + 6 * 0x30 + 0x18) == kids);
    build_rows(); ST16(ROWS + 6 * 0x30 + 0x10, 0x999);   // not the game's Save row: untouched
    rt_load_menu_update((uint64_t)menu);
    assert(LD64(ROWS + 6 * 0x30) == SAVE_HANDLER && LD32(ROWS + 6 * 0x30 + 0x20) == 0);
    build_rows(); ST32((uint64_t)menu + 0x10c8, 9);       // not eight rows: untouched
    rt_load_menu_update((uint64_t)menu);
    assert(LD64(ROWS + 6 * 0x30) == SAVE_HANDLER);

    // ---- the handler hook
    uint64_t real = (uint64_t)menu;
    Cpu c = cpu(real, 6);                                  // the original Save row: the hook does nothing
    assert(!rt_load_menu_handler(&c) && c.x[0] == real && !ncalls);
    c = cpu(real + 1, 3);                                  // the Load row asked something else: the real object goes on to the original code
    assert(!rt_load_menu_handler(&c) && c.x[0] == real && !ncalls);
    c = cpu(real + 1, 1);
    assert(!rt_load_menu_handler(&c) && c.x[0] == real);

    c = cpu(real + 1, 6);                                  // choose: one request in load mode, registers and result kept
    assert(rt_load_menu_handler(&c) && c.x[0] == 1 && c.x[30] == 0x1234 && c.x[2] == 0x5002 && c.x[18] == 0x5012);
    assert(ncalls == 1 && calls[0].fn == REQUEST && calls[0].n == 4 && calls[0].a0 == manager && calls[0].a1 == 0 && calls[0].a2 == 1 && calls[0].a3 == 0);

    ncalls = 0; ended_answer = 0; ST32(RESULT_WORD, 4);    // op 4 while the screen is still up: the original answer, nothing else
    c = cpu(real + 1, 4);
    assert(rt_load_menu_handler(&c) && c.x[0] == 0 && ncalls == 1 && calls[0].fn == SAVE_HANDLER && calls[0].a0 == real && calls[0].a1 == 4);

    ncalls = 0; ended_answer = 1; ST32(RESULT_WORD, (uint32_t)-1);   // ended and cancelled: the answer passes, no script
    c = cpu(real + 1, 4);
    assert(rt_load_menu_handler(&c) && c.x[0] == 1 && ncalls == 1);

    ncalls = 0; ended_answer = 1; ST32(RESULT_WORD, 4);              // ended again with no request of ours pending: no script
    c = cpu(real + 1, 4);
    assert(rt_load_menu_handler(&c) && ncalls == 1);

    ncalls = 0; c = cpu(real + 1, 6); rt_load_menu_handler(&c);       // a new request, ended with a positive result: fld_continue runs
    ncalls = 0; ended_answer = 1; ST32(RESULT_WORD, 4);
    c = cpu(real + 1, 4);
    assert(rt_load_menu_handler(&c) && c.x[0] == 1 && c.x[30] == 0x1234 && ncalls == 2);
    assert(calls[1].fn == RUN_SCRIPT && calls[1].n == 1 && calls[1].a0 == SCRIPT_NAME);
    ncalls = 0; c = cpu(real + 1, 4);                                  // and only once
    assert(rt_load_menu_handler(&c) && ncalls == 1);

    ncalls = 0; ST64(manager_var, 0);                                  // no scene manager: no request, the row answers "chosen"
    c = cpu(real + 1, 6);
    assert(rt_load_menu_handler(&c) && c.x[0] == 1 && !ncalls);
    // ---- the label hook: only the two new rows, and only when shown
    static uint8_t view[0x1000] __attribute__((aligned(16)));
    uint64_t v = (uint64_t)view, span[2] = {0x1234, 0x60d};
    ST64(v + 0xb8 + 6 * 0xb8 + 0x10, 0xaaa); ST64(v + 0x680 + 1 * 0xb8 + 0x10, 0xbbb);
    ncalls = 0;
    c = cpu(v, 0); c.x[1] = 0; c.x[2] = 6; c.x[3] = (uint64_t)span;                 // the Data row of the main list
    assert(rt_load_menu_label(&c) && c.x[30] == 0x1234 && c.x[2] == 6 && c.x[5] == 0x5005 && ncalls == 2);
    assert(calls[0].fn == SHOW_ROW && calls[0].a0 == v + 0xb8 + 6 * 0xb8 && calls[0].a1 == 1);
    assert(calls[1].fn == SET_TEXT && calls[1].a0 == 0xaaa && !strcmp((char *)calls[1].a1, "Data"));
    ncalls = 0; span[1] = 0x120f;
    c = cpu(v, 0); c.x[1] = 1; c.x[2] = 1; c.x[3] = (uint64_t)span;                 // the Load row of the sub-list
    assert(rt_load_menu_label(&c) && ncalls == 2 && calls[0].a0 == v + 0x680 + 0xb8 && calls[1].a0 == 0xbbb && !strcmp((char *)calls[1].a1, "Load"));
    ncalls = 0;
    c = cpu(v, 0); c.x[1] = 0; c.x[2] = 1; c.x[3] = (uint64_t)span;                 // the Load message in the main list: not ours
    assert(!rt_load_menu_label(&c) && !ncalls);
    span[1] = 0x3ca; c = cpu(v, 0); c.x[1] = 1; c.x[2] = 0; c.x[3] = (uint64_t)span;   // the Save row keeps the game's text
    assert(!rt_load_menu_label(&c) && !ncalls);
    span[0] = 0; span[1] = 0x60d; c = cpu(v, 0); c.x[1] = 0; c.x[2] = 6; c.x[3] = (uint64_t)span;   // a hidden row: the game hides it
    assert(!rt_load_menu_label(&c) && !ncalls);
    span[0] = 1; c = cpu(v, 0); c.x[1] = 2; c.x[2] = 6; c.x[3] = (uint64_t)span;        // another list level
    assert(!rt_load_menu_label(&c) && !ncalls);
    puts("load menu runtime: table, handler hook, load request, fld_continue only after a positive result, labels");
    return 0;
}
