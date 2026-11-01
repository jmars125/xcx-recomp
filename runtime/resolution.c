// Game resolution: the size the game composes its frame in, above or beside the shipped 1920x1080 (2560x1440,
// 3840x2160, 21:9 3440x1440). The game does the rendering itself, as with the community resolution mods for 1.0.1 by
// Fl4sh_ (github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods), which this follows.
//
// The game reads its frame size from a table in its read-only data, main+0x17fe914 in 2.0.0 (1.0.0 +0x180d840):
// {docked width, height, handheld width, height}, shipped {1920, 1080, 1280, 720}. Its users (main+0x971a30,
// +0x9ca190, +0xa478d8) take the frame size and the aspect ratio from it, so a wider table widens the 3D view as well.
// The mods rewrite the table; res_init writes it once the modules are loaded, before the game runs: the docked size is
// XCX_OUTPUT_RES = <w>x<h> (default 1920x1080), the handheld size half of it.
//
// One more site reads the table: main+0x9ccce0 sizes two buffers from a width it loads (main+0x9cccec) and the table's
// docked height (main+0x9ccd00). The mods replace both loads with sizes of their own that differ per mod (1280x720 at
// 3840x2160, 1920x1080 at 2560x1440, none at 5160x2160). The patch set "resbuf" (recomp/patches-upd.txt) loads them
// from the patch page's +0x50 and +0x54 instead: XCX_RES_BUFFER = <w>x<h> switches it on, unset keeps the game's.
//
// XCX_RENDER_SCALE is TotK Recomp's scene size and has no counterpart here: XCX scales its 3D scene itself.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define BASE_W 1920
#define BASE_H 1080
#define TABLE (MOD_MAIN_BASE + 0x17fe914)

static struct {
    int decided;
    unsigned ow, oh;   // output (docked)
    int on;            // res_init wrote the table
} R;

static int parse(const char *s, unsigned *w, unsigned *h) {
    char tail;
    return s && sscanf(s, "%ux%u%c", w, h, &tail) == 2 && *w >= 640 && *h >= 360 && *w <= 7680 && *h <= 4320;
}

static void decide(void) {
    if (R.decided) return;
    R.decided = 1;
    R.ow = BASE_W;
    R.oh = BASE_H;
    const char *o = cfg_value("XCX_OUTPUT_RES");
    unsigned w, h;
    if (parse(o, &w, &h)) {
        R.ow = w & ~1u;
        R.oh = h & ~1u;
    } else {
        rt_log("resolution: XCX_OUTPUT_RES=%s is not <width>x<height> in 640x360..7680x4320; 1920x1080", o);
    }
}

// TotK Recomp grows the texture heap through its HeapDef table; XCX has no such table, and the 1.0.1 mods run at
// 3840x2160 without a bigger heap.
uint64_t res_heap_extra_kib(void) { return 0; }
uint64_t res_heap_extra_bytes(void) { return 0; }
int res_heapdef_build(const void *in, size_t n, void **out, size_t *out_n) {
    (void)in, (void)n, (void)out_n;
    *out = NULL;
    return 0;
}
void res_heapdef_served(void) {}

void res_init(void) {
    decide();
    const char *s = cfg_value("XCX_RENDER_SCALE");
    if (s && strcmp(s, "game")) rt_log("resolution: XCX_RENDER_SCALE=%s has no effect; XCX scales its 3D scene itself", s);
    uint32_t table[4];
    if (!plat_mem_read(TABLE, table, sizeof table) || table[0] != BASE_W || table[1] != BASE_H || table[2] != 1280 ||
        table[3] != 720) {
        rt_log("resolution: no {1920, 1080, 1280, 720} at main+0x17fe914; the game keeps its own resolution");
        return;
    }
    unsigned bw, bh;
    int resbuf = rt_patch_set("resbuf");
    uint8_t *page = rt_patch_page();
    // patches.c switched the set on for any value but 0; only a size keeps it on.
    if (resbuf >= 0 && page && parse(cfg_get("XCX_RES_BUFFER"), &bw, &bh)) {
        uint32_t v[2] = {bw, bh};
        memcpy(page + 0x50, v, sizeof v);
        rt_patch_switch(resbuf, 1);
        rt_log("resolution: the buffers of main+0x9ccce0 at %ux%u", bw, bh);
    } else if (resbuf >= 0) {
        rt_patch_switch(resbuf, 0);
    }
    if (R.ow == BASE_W && R.oh == BASE_H) {
        rt_log("resolution: the game's own (1920x1080)");
        return;
    }
    uint32_t want[4] = {R.ow, R.oh, (R.ow / 2) & ~1u, (R.oh / 2) & ~1u};
    uint64_t page_at = TABLE & ~0xfffull;
    if (!plat_mem_protect(page_at, 0x1000, PLAT_PROT_RW)) {
        rt_log("resolution: main+0x17fe914 cannot be made writable; the game keeps its own resolution");
        return;
    }
    memcpy((void *)(uintptr_t)TABLE, want, sizeof want);
    plat_mem_protect(page_at, 0x1000, PLAT_PROT_READ);
    R.on = 1;
    rt_log("resolution: %ux%u (handheld %ux%u)", want[0], want[1], want[2], want[3]);
}

void res_sizes(unsigned out[2], unsigned scene[2]) {
    decide();
    out[0] = R.on ? R.ow : BASE_W, out[1] = R.on ? R.oh : BASE_H;
    scene[0] = out[0] * 5 / 6, scene[1] = out[1] * 5 / 6;
}
