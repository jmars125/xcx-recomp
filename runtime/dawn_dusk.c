// Dawn and dusk (XCX_DAWN_DUSK, the Game tab's row under Day length): original | smoothed | extended.
//
// The light of a map changes with the time of day through three systems, each on its own timetable:
//   - the light and fog keys of the map header (the DLGT section of map/<map>.wismhd): looks with a start time and a
//     blend length, blended in straight lines by the map light update (main+0x982b40) from the time it copies at
//     main+0x982b6c;
//   - the post-effect sets (exposure and the rest, built from the map header's post-effect table): the post manager
//     [main+0x1fefba0] blends them field by field in main+0x3fb374; each set has its start (+0x2c, minutes) and blend
//     (+0x30, minutes) and the colour table it uses (+0x3c);
//   - the colour table (a 16x16x16 3D table) of the active post set. The game fades between tables over 120 frames,
//     but the colour pass that runs during that fade samples one table and never reads the fade weight, so the new
//     table appears at once: the step at 05:00 and at 18:45 in the original.
// Original leaves all three alone. Smoothed keeps the game's times and smooths the changes; Smoothed & Extended also
// moves them (dawn 04:00-08:00, dusk 17:00-21:00):
//   - the colour table is crossfaded in its data: a clean copy of each table is kept, and while a post set blends both
//     the old and the new table hold old + (new - old) * t, byte by byte (same layout, so the swizzle doesn't matter);
//     the game's own 120-frame fade is held at the same share so it keeps both tables until the blend ends. When it
//     ends, both get their clean data back;
//   - the post-effect blend is curved by direction (patch set ddcurve: t + (t^2 - t) * k): squared into night, so
//     the exposure doesn't rise ahead of the falling light, and front-loaded into day;
//   - the light keys see an eased time at dawn (patch set ddlight: start + length * smoothstep(u^power)), because a
//     straight light ramp looks fast first and slow later on screen;
//   - Extended rewrites the key and set times in memory, kept from the originals so another choice puts them back.
//     The DLGT key time data is the one the light key blend reads (patch set ddkeys saves it on the patch page). The
//     table manager comes from patch set ddlut.
// Night darkness (XCX_DARK_NIGHTS, the row below): original | darker | darkest. Both lower the intensity of the night
// light keys (Mira's 18:45 keys; key_target), which light the ground, people and things, and leave the fog keys and the
// colour tables' colours, so the night sky stays the game's. The game blends that key in and out itself, so the night
// darkens and lifts with whichever Dawn and dusk timing is in use. With Dawn and dusk at Original, Night darkness also
// crossfades the colour tables on the game's own straight blend, so the game's table switch doesn't step the light.
// Applies at once; a map load (the header is read again) is caught within a second and written again. Back at
// Original + Original everything is given back (only where memory still holds what was written) and the four patch
// sets are switched off. Needs the clock (rt_day_length_state 1), like Day length.
//
// Accepted, not traced further: inside the dawn window the light keys see the clock's time plus the ease's offset, so
// a scene with a time of its own (a cutscene) would get that offset too; and when the game rebuilds only some of the
// post sets, the ones still holding our moved values no longer match a move, so they are not listed again and keep
// those values until the map is loaded again.
//
// Patch page: +0x920 colour-table manager (ddlut), +0x940 curve k (ddcurve), +0x950/+0x954 light time a, b
// (ddlight), +0x968 light key time data (ddkeys).
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { DD_ORIGINAL, DD_SMOOTHED, DD_EXTENDED };

typedef struct { int from, start, blend; } KeyMove;                    // minutes
typedef struct { int from, from_blend, start, blend; } SetMove;         // minutes
typedef struct {
    const KeyMove *keys;
    int nkeys;
    const SetMove *sets;
    int nsets;
    double k_dusk, k_dawn;                    // ddcurve: into night, into day
    double ease_start, ease_len, ease_pow;    // ddlight window, minutes; len 0 = none
} Profile;

static const KeyMove ext_keys[] = {{280, 240, 240}, {300, 240, 240}, {1020, 1020, 120}, {1125, 1140, 120}};
static const SetMove ext_sets[] = {{300, 120, 285, 120}, {1125, 15, 1140, 120}};
static const Profile profiles[] = {
    [DD_ORIGINAL] = {0},
    [DD_SMOOTHED] = {.k_dusk = 1.0, .k_dawn = -0.7, .ease_start = 280, .ease_len = 140, .ease_pow = 1.0},
    [DD_EXTENDED] = {.keys = ext_keys, .nkeys = 4, .sets = ext_sets, .nsets = 2, .k_dusk = 1.0, .k_dawn = -0.7,
                     .ease_start = 240, .ease_len = 240, .ease_pow = 1.2},
};

// Game addresses (2.0.0) and our patch page slots.
#define POST_MGR 0x1fefba0   // main + this: the post manager pointer
#define PG_LUT 0x920         // ddlut: the table manager
#define PG_CURVE 0x940       // ddcurve: k
#define PG_LIGHT 0x950       // ddlight: a, b
#define PG_KEYS 0x968        // ddkeys: the key time data

static int mode = -1;
static unsigned cfg_seen = ~0u;
static int s_ddlut = -2, s_curve, s_light, s_env;

static double wsec(void) {
    int64_t w = frame_step_world_us();
    return w < 0 ? -1.0 : w / 1e6;
}

static uint64_t main_base(void) {
    const RtLoaded *m = ldr_module(0);
    return m ? m->base : 0;
}

// ---- light and fog keys: the time data the light key blend reads (patch set ddkeys saves its address T on the page).
// A table of groups at T (count at +0, offset at +4), each a list of weather entries (12 bytes: count at +4, offset at
// +8), each a list of keys. Light keys are 28 bytes (look: RGB, intensity at +12, one more; blend at +20, start at
// +24), fog keys 76 (blend at +60, start at +64); times are day fractions. Which lists hold fog keys is told by their
// stride (the gap to the next list over its count). This was traced on Mira (ma9990), whose keys start at 0 (always),
// 04:40, 05:00, 17:00 and 18:45; data whose keys start at any other time is left alone.
//
// Each block of key data seen is kept apart (another map, another key blender; nothing else reads keys): every timed
// key with its original start, blend and intensity and what was written last. Only a block the key blend read in the
// last 2 s is written (a map left behind may have been freed, its memory still holding our values); one not read for
// 10 s is forgotten without a write. Once a second each block in use is checked in full: one that no longer holds what
// was written was read again by the game (a map load) and is read afresh, or isn't key data any more and is let go.
// An address that isn't key data is tried again after 30 s.
#define BLOCK_MAX 4
#define BLOCK_KEYS 512
#define LIST_MAX 512    // key lists in one block of key data
#define BAD_MAX 8
typedef struct {
    uint64_t addr;                   // the key's blend; the start follows it, a light key's intensity is 8 bytes before
    float start, blend, inten;       // the game's
    float ws, wb, wi;                // written last
    uint8_t fog, night;              // night: its list's night key (the latest start at or after 18:00)
} Key;
typedef struct { uint64_t T; double seen; int n; Key k[BLOCK_KEYS]; } Block;
static Block blocks[BLOCK_MAX];       // T == 0: free
static Key scratch[BLOCK_KEYS];
static struct { uint64_t T; double at; } bad[BAD_MAX];
static int bad_next;
static double night_light = 1.0;      // Night darkness: the night light keys' intensity scale

static int rd32(uint64_t a, uint32_t *v) { return plat_mem_read(a, v, 4); }

typedef struct { uint32_t off, count; } KList;
static int cmp_list(const void *a, const void *b) {
    uint32_t x = ((const KList *)a)->off, y = ((const KList *)b)->off;
    return x < y ? -1 : x > y;
}

// Every timed key of the data at T into scratch: the count, 0 when it doesn't read as Mira's key data (or has more
// timed keys than a block holds; logged by the caller).
static int keys_read(uint64_t T) {
    uint32_t n = 0, off = 0;
    if (!rd32(T, &n) || !rd32(T + 4, &off) || !n || n > 64) return 0;
    static KList L[LIST_MAX];
    int nl = 0;
    for (uint32_t g = 0; g < n; g++) {
        uint32_t wn = 0, wo = 0;
        if (!rd32(T + off + 12 * g + 4, &wn) || !rd32(T + off + 12 * g + 8, &wo) || wn > 64) return 0;
        for (uint32_t w = 0; w < wn; w++) {
            uint32_t c = 0, o = 0;
            if (nl >= LIST_MAX) {
                static int said;
                if (!said++) rt_log("dawn/dusk: light key data with more than %d lists at %#llx; left alone", LIST_MAX, (unsigned long long)T);
                return 0;
            }
            if (!rd32(T + wo + 12 * w + 4, &c) || !rd32(T + wo + 12 * w + 8, &o) || c > 64) return 0;
            if (c) L[nl].off = o, L[nl].count = c, nl++;
        }
    }
    qsort(L, (size_t)nl, sizeof *L, cmp_list);
    int at = 0;
    for (int i = 0; i < nl; i++) {
        int fog = i + 1 < nl && (L[i + 1].off - L[i].off) / L[i].count >= 76, night = -1;
        uint32_t st = fog ? 76 : 28, bo = fog ? 60 : 20;
        for (uint32_t k = 0; k < L[i].count; k++) {
            uint64_t a = T + L[i].off + st * k + bo;
            float b, s, in = 0;
            if (!plat_mem_read(a, &b, 4) || !plat_mem_read(a + 4, &s, 4) || !(s >= 0 && s < 1) || !(b >= 0 && b < 1)) return 0;
            int m = (int)lrint(s * 1440);
            if (m != 0 && m != 280 && m != 300 && m != 1020 && m != 1125) return 0;
            if (m == 0 && b == 0) continue;   // untimed (always)
            if (at >= BLOCK_KEYS) {
                static int said;
                if (!said++) rt_log("dawn/dusk: more than %d timed light keys at %#llx; left alone", BLOCK_KEYS, (unsigned long long)T);
                return 0;
            }
            if (!fog && !plat_mem_read(a - 8, &in, 4)) return 0;
            Key *e = &scratch[at++];
            memset(e, 0, sizeof *e);
            e->addr = a, e->start = s, e->blend = b, e->inten = in, e->fog = (uint8_t)fog;
            if (m >= 18 * 60 && (night < 0 || s > scratch[night].start)) night = at - 1;
        }
        if (night >= 0 && !fog) scratch[night].night = 1;
    }
    return at;
}

// What a key gets for a profile and night scale: its moved start and blend, and (a night light key) its intensity.
static void key_target(const Key *e, const Profile *p, double nl, float *s, float *b, float *in) {
    int m = (int)lrint(e->start * 1440);
    *s = e->start, *b = e->blend, *in = e->inten;
    for (int j = 0; j < p->nkeys; j++)
        if (p->keys[j].from == m) *s = p->keys[j].start / 1440.0f, *b = p->keys[j].blend / 1440.0f;
    if (e->night) *in = e->inten * (float)nl;
}

// Blend and start are next to each other: one 8-byte store when aligned, so the game never sees a new start with an old
// blend (two 4-byte stores otherwise; a mixed frame is one frame of a slightly different blend).
static void put_pair(uint64_t a, float b, float s) {
    float v[2] = {b, s};
    if (!(a & 7)) {
        uint64_t u;
        memcpy(&u, v, 8);
        __atomic_store_n((uint64_t *)(uintptr_t)a, u, __ATOMIC_RELAXED);   // guest memory is host memory
    } else
        memcpy((void *)(uintptr_t)a, v, 8);
}

static void block_write(Block *bl, const Profile *p, double nl) {
    for (int i = 0; i < bl->n; i++) {
        Key *e = &bl->k[i];
        float s, b, in;
        key_target(e, p, nl, &s, &b, &in);
        put_pair(e->addr, b, s);
        if (!e->fog) memcpy((void *)(uintptr_t)(e->addr - 8), &in, 4);
        e->ws = s, e->wb = b, e->wi = in;
    }
}

// The block still holds every value written last. Run once a second on the present thread: the keys (a few hundred) are
// read in one go when they lie within 64 KB (one read instead of ~550 of ~0.24 us each), else one by one.
static int block_holds(const Block *bl) {
    static uint8_t span[0x10000];
    uint64_t lo = ~0ull, hi = 0;
    for (int i = 0; i < bl->n; i++) {
        if (bl->k[i].addr - 8 < lo) lo = bl->k[i].addr - 8;
        if (bl->k[i].addr + 8 > hi) hi = bl->k[i].addr + 8;
    }
    int whole = bl->n && hi - lo <= sizeof span && plat_mem_read(lo, span, (size_t)(hi - lo));
    for (int i = 0; i < bl->n; i++) {
        const Key *e = &bl->k[i];
        float v[2], in = 0;
        if (whole) {
            memcpy(v, span + (e->addr - lo), 8);
            if (!e->fog) memcpy(&in, span + (e->addr - 8 - lo), 4);
        } else if (!plat_mem_read(e->addr, v, 8) || (!e->fog && !plat_mem_read(e->addr - 8, &in, 4)))
            return 0;
        if (v[0] != e->wb || v[1] != e->ws || (!e->fog && in != e->wi)) return 0;
    }
    return 1;
}

// Gives a block's keys back the game's values (when it is in use and still ours) and frees it.
static void block_release(Block *bl, double now) {
    if (bl->T && now - bl->seen <= 2 && block_holds(bl)) block_write(bl, &profiles[DD_ORIGINAL], 1.0);
    bl->T = 0, bl->n = 0;
}

static int is_bad(uint64_t T, double now) {
    for (int i = 0; i < BAD_MAX; i++)
        if (bad[i].T == T && now - bad[i].at < 30) return 1;
    return 0;
}

static void keys_update(const Profile *p, int changed, double now) {
    uint8_t *pg = rt_patch_page();
    uint64_t T = 0;
    if (pg) memcpy(&T, pg + PG_KEYS, 8);
    for (int i = 0; i < BLOCK_MAX; i++)
        if (T && blocks[i].T == T) blocks[i].seen = now;
    for (int i = 0; i < BLOCK_MAX; i++) {
        Block *bl = &blocks[i];
        if (!bl->T) continue;
        if (now - bl->seen > 10) {   // not read for a while: its map is gone; nothing is written there
            rt_log("dawn/dusk: light keys at %#llx no longer in use", (unsigned long long)bl->T);
            bl->T = 0, bl->n = 0;
            continue;
        }
        if (now - bl->seen > 2) continue;
        if (!block_holds(bl)) {   // read again by the game, or not key data any more
            int n = keys_read(bl->T);
            if (n) {
                memcpy(bl->k, scratch, sizeof *scratch * (size_t)n), bl->n = n;
                block_write(bl, p, night_light);
                rt_log("dawn/dusk: light keys at %#llx read again (%d)", (unsigned long long)bl->T, n);
            } else {
                rt_log("dawn/dusk: light keys at %#llx gone", (unsigned long long)bl->T);
                bl->T = 0, bl->n = 0;
            }
        } else if (changed)
            block_write(bl, p, night_light);
    }
    if (!T) return;
    for (int i = 0; i < BLOCK_MAX; i++)
        if (blocks[i].T == T) return;
    if (is_bad(T, now)) return;
    int n = keys_read(T);
    if (!n) {
        bad[bad_next].T = T, bad[bad_next].at = now, bad_next = (bad_next + 1) % BAD_MAX;
        rt_log("dawn/dusk: %#llx doesn't read as light key data; left alone", (unsigned long long)T);
        return;
    }
    Block *bl = NULL;
    for (int i = 0; i < BLOCK_MAX && !bl; i++)
        if (!blocks[i].T) bl = &blocks[i];
    if (!bl) {   // all taken: the one seen longest ago goes (its keys get the game's values back if still ours)
        bl = &blocks[0];
        for (int i = 1; i < BLOCK_MAX; i++)
            if (blocks[i].seen < bl->seen) bl = &blocks[i];
        rt_log("dawn/dusk: light keys at %#llx let go for %#llx", (unsigned long long)bl->T, (unsigned long long)T);
        block_release(bl, now);
    }
    bl->T = T, bl->seen = now, bl->n = n;
    memcpy(bl->k, scratch, sizeof *scratch * (size_t)n);
    block_write(bl, p, night_light);
    rt_log("dawn/dusk: %d timed light and fog keys at %#llx", n, (unsigned long long)T);
}

static void keys_release_all(double now) {
    for (int i = 0; i < BLOCK_MAX; i++) block_release(&blocks[i], now);
}

// ---- post-effect sets: the post manager's groups (+0x60, count +0x68), each with its sets (+8 on, count +0x248);
// a set's start at +0x2c (int, minutes), blend at +0x30 (float, minutes). Only the sets the choice moves are kept (in
// Mira most of them: each group has its own dawn and night sets), with the game's values and what was written; once a second they are checked, and the sets are listed
// afresh when the first group's first set moved or a kept one no longer holds what was written (the sets were built
// again: a map load). The game's other sets are never written. A change of choice gives the kept sets back and lists
// them again for the new one; while there are no sets (loading), that waits.
#define SET_MAX 256
typedef struct { uint64_t addr; int32_t start, ws; float blend, wb; } Set;
static Set sets[SET_MAX];
static int nsets, sets_listed;
static uint64_t sets_pm, sets_groups, sets_first;   // the post manager, its groups and the first group's first set
                                                    // when the sets were listed: any of them new = listed again

static uint64_t post_manager(void) {
    uint64_t b = main_base(), pm = 0;
    return b && plat_mem_read(b + POST_MGR, &pm, 8) ? pm : 0;
}

// The manager's sets that p moves, into out (at most SET_MAX: more is logged and the rest left alone). The count.
static int sets_list(uint64_t pm, const Profile *p, Set *out) {
    uint64_t groups = 0;
    uint32_t ng = 0;
    if (!pm || !plat_mem_read(pm + 0x60, &groups, 8) || !groups || !plat_mem_read(pm + 0x68, &ng, 4) || !ng) return 0;
    if (ng > 64) {
        static int said;
        if (!said++) rt_log("dawn/dusk: %u post-effect groups; sets left alone", ng);
        return 0;
    }
    int n = 0;
    for (uint32_t g = 0; g < ng; g++) {
        uint64_t grp = 0, ns = 0;
        if (!plat_mem_read(groups + 8ull * g, &grp, 8) || !grp || !plat_mem_read(grp + 0x248, &ns, 8)) continue;
        if (ns > 64) {
            static int said;
            if (!said++) rt_log("dawn/dusk: a post-effect group with %llu sets; it is left alone", (unsigned long long)ns);
            continue;
        }
        for (uint64_t s = 0; s < ns; s++) {
            uint64_t a = 0;
            uint8_t v[8];   // start (+0x2c) and blend (+0x30)
            int32_t st;
            float bl;
            if (!plat_mem_read(grp + 8 + 8 * s, &a, 8) || !a || !plat_mem_read(a + 0x2c, v, 8)) continue;
            memcpy(&st, v, 4), memcpy(&bl, v + 4, 4);
            int moved = 0;
            for (int j = 0; j < p->nsets && !moved; j++) moved = p->sets[j].from == st && lrintf(bl) == p->sets[j].from_blend;
            if (!moved) continue;
            if (n >= SET_MAX) {
                static int said;
                if (!said++) rt_log("dawn/dusk: more than %d post-effect sets to move; the rest left alone", SET_MAX);
                return n;
            }
            Set *e = &out[n++];
            e->addr = a, e->start = e->ws = st, e->blend = e->wb = bl;
        }
    }
    return n;
}

static int sets_hold(void) {
    for (int i = 0; i < nsets; i++) {
        uint8_t v[8];   // start (+0x2c) and blend (+0x30) in one read
        int32_t s;
        float b;
        if (!plat_mem_read(sets[i].addr + 0x2c, v, 8)) return 0;
        memcpy(&s, v, 4), memcpy(&b, v + 4, 4);
        if (s != sets[i].ws || b != sets[i].wb) return 0;
    }
    return 1;
}

static void sets_write(const Profile *p) {
    for (int i = 0; i < nsets; i++) {
        Set *e = &sets[i];
        int32_t s = e->start;
        float b = e->blend;
        for (int j = 0; j < p->nsets; j++)
            if (p->sets[j].from == e->start && lrintf(e->blend) == p->sets[j].from_blend) s = p->sets[j].start, b = (float)p->sets[j].blend;
        memcpy((void *)(uintptr_t)(e->addr + 0x2c), &s, 4);
        memcpy((void *)(uintptr_t)(e->addr + 0x30), &b, 4);
        e->ws = s, e->wb = b;
    }
}

static void sets_release(void) {
    if (nsets && sets_hold()) sets_write(&profiles[DD_ORIGINAL]);
    nsets = 0, sets_listed = 0, sets_pm = sets_groups = sets_first = 0;
}

// Only Smoothed & Extended moves sets, so a change of choice that gets here (a Night darkness change) keeps the same
// sets and values: they are written again where they still hold ours, never put back in between (a frame that read the
// game's times there could start the colour-table switch this setting removes). Leaving Extended: sets_release.
static void sets_update(const Profile *p, int changed) {
    uint64_t pm = post_manager(), groups = 0, grp0 = 0, first = 0;
    if (pm && plat_mem_read(pm + 0x60, &groups, 8) && groups && plat_mem_read(groups, &grp0, 8) && grp0)
        plat_mem_read(grp0 + 8, &first, 8);
    if (!first) return;   // no sets now (loading): listed once they are there
    if (sets_listed && sets_pm == pm && sets_groups == groups && sets_first == first && sets_hold()) {
        if (changed) sets_write(p);
        return;
    }
    nsets = sets_list(pm, p, sets), sets_listed = 1, sets_pm = pm, sets_groups = groups, sets_first = first;
    rt_log("dawn/dusk: %d post-effect sets moved", nsets);
    sets_write(p);
}

// ---- colour tables (Dawn and dusk other than Original, and Night darkness)
// A slot per table object seen: where its data is, which table it holds, a clean copy, and (dirty) a hash of what was
// written last. The table manager keeps a small pool of table objects and points them at other tables as needed (a
// table id at +0x8, the loaded one at +0xa, the data at +0x30, its size at +0xa0), so a slot is trusted only while its
// object still holds the same table at the same place; the clean copy is taken again whenever the slot isn't written
// to, and data is put back only where it still holds what was written. Slots not written to are reused when all are
// taken.
#define XF_MAX 6
#define XF_SIZE 0x4000
static struct { uint64_t obj, addr, hash; uint16_t id; uint8_t clean[XF_SIZE]; int dirty; } xf[XF_MAX];

// FNV-1a over 8-byte words (n a multiple of 8): it only tells whether a table still holds what we wrote, and a word at
// a time costs ~1.5 us for a table where byte by byte took ~12 us, on every frame of a blend.
static uint8_t xf_buf[XF_SIZE];   // the mix (tables_update) or what is in memory now (xf_put_back); never both at once

static uint64_t fnv(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull, w;
    for (size_t i = 0; i + 8 <= n; i += 8) memcpy(&w, p + i, 8), h = (h ^ w) * 0x100000001b3ull;
    return h;
}

// Put back the slot's clean data, only where the memory still holds what was written (else it isn't ours any more).
static void xf_put_back(int i) {
    uint8_t *now = xf_buf;
    uint64_t addr = 0;
    uint16_t id = 0;
    if (xf[i].dirty && plat_mem_read(xf[i].obj + 0x30, &addr, 8) && addr == xf[i].addr &&
        plat_mem_read(xf[i].obj + 0xa, &id, 2) && id == xf[i].id && plat_mem_read(xf[i].addr, now, XF_SIZE) &&
        fnv(now, XF_SIZE) == xf[i].hash)
        memcpy((void *)(uintptr_t)xf[i].addr, xf[i].clean, XF_SIZE);   // guest memory is host memory
    xf[i].dirty = 0;
}

static int xf_find(uint64_t obj) {
    uint64_t addr = 0, size = 0;
    uint16_t id = 0;
    uint8_t o[0xa8];   // the object up to its size: the id at +0xa, the data at +0x30, the size at +0xa0
    if (!plat_mem_read(obj, o, sizeof o)) return -1;
    memcpy(&id, o + 0xa, 2), memcpy(&addr, o + 0x30, 8), memcpy(&size, o + 0xa0, 8);
    if (size != XF_SIZE || !addr) return -1;
    int i = -1;
    for (int j = 0; j < XF_MAX && i < 0; j++)
        if (xf[j].obj == obj) i = j;
    if (i >= 0 && (xf[i].addr != addr || xf[i].id != id)) xf[i].obj = 0, xf[i].dirty = 0, i = -1;   // another table now
    if (i >= 0 && xf[i].dirty) return i;
    if (i < 0)
        for (int j = 0; j < XF_MAX && i < 0; j++)
            if (!xf[j].obj) i = j;
    if (i < 0)
        for (int j = 0; j < XF_MAX && i < 0; j++)
            if (!xf[j].dirty) i = j;   // all taken: one not written to
    if (i < 0) {
        static int said;
        if (!said++) rt_log("dawn/dusk: more than %d colour tables in a blend at once; this one is left alone", XF_MAX);
        return -1;
    }
    if (!plat_mem_read(addr, xf[i].clean, XF_SIZE)) return -1;
    xf[i].obj = obj, xf[i].addr = addr, xf[i].id = id, xf[i].dirty = 0;
    return i;
}

static void xf_restore(void) {
    for (int i = 0; i < XF_MAX; i++)
        if (xf[i].dirty) xf_put_back(i);
}

static void xf_forget(void) {
    for (int i = 0; i < XF_MAX; i++) xf[i].obj = 0, xf[i].dirty = 0;
}

// ---- the active post set (the post manager's +0xc8), read once a frame for the curve and the tables
typedef struct {
    uint64_t pm;     // the post manager (0: none)
    int ok;          // the time of day and the active set were read
    int32_t start;   // the active set's start, minutes
    float blend;     // its blend, minutes
    double h;        // the time of day, hours (-1: none)
    double since;    // minutes since the set's start, [0, 1440)
} Active;

static Active active_read(void) {
    Active a = {0};
    uint64_t set = 0;
    uint8_t v[8];   // start (+0x2c) and blend (+0x30)
    a.pm = post_manager(), a.h = rt_day_length_hours();
    if (a.h < 0 || !a.pm || !plat_mem_read(a.pm + 0xc8, &set, 8) || !set || !plat_mem_read(set + 0x2c, v, 8)) return a;
    memcpy(&a.start, v, 4), memcpy(&a.blend, v + 4, 4);
    a.since = a.h * 60.0 - a.start;
    if (a.since < 0) a.since += 1440;
    a.ok = 1;
    return a;
}

// While the active post set blends: hold the game's fade of slot A0 (table manager +0x350 new texture, +0x358 old,
// +0x364 elapsed, +0x368 length) at the blend's share and write the mixed table into both. k: the blend's curve.
static void tables_update(const Active *a, double k) {
    uint8_t *pg = rt_patch_page();
    uint64_t mgr = 0, cur = 0, old = 0;
    float len = 0;
    if (pg) memcpy(&mgr, pg + PG_LUT, 8);
    // a new post manager (a map load, the title screen) may come with new tables and a new table manager: the slots
    // are given back and forgotten, and the manager is taken again from the game's next table request
    static uint64_t last_pm;
    if (a->pm != last_pm) {
        if (last_pm) xf_restore(), xf_forget(), memset(pg + PG_LUT, 0, 8), mgr = 0;
        last_pm = a->pm;
    }
    uint8_t m[0x1c];   // the manager from +0x350: new texture, old (+8), length (+0x18)
    if (!mgr || !a->ok || !plat_mem_read(mgr + 0x350, m, sizeof m)) {
        xf_restore();
        return;
    }
    memcpy(&cur, m, 8), memcpy(&old, m + 8, 8), memcpy(&len, m + 0x18, 4);
    // the tables end with the blend (both then hold the new table); the curve runs a minute longer (curve_update)
    double t = a->blend > 0 ? a->since / a->blend : 1;
    int c = -1, o = -1;   // the slots only while a blend runs (outside it they'd copy both tables every frame)
    if (old && cur && t < 1) c = xf_find(cur), o = xf_find(old);
    if (!old || t >= 1 || c < 0 || o < 0 || c == o) {
        xf_restore();
        return;
    }
    if (t < 0) t = 0;
    t += (t * t - t) * k;
    float el = (float)(t * len);   // the render code resets the length each frame; the game adds a frame before the next
    memcpy((void *)(uintptr_t)(mgr + 0x364), &el, 4);
    uint8_t *mix = xf_buf;
    int w = (int)(t * 256.0 + 0.5);
    for (int j = 0; j < XF_SIZE; j++) {
        int a = xf[o].clean[j], n = xf[c].clean[j];
        mix[j] = (uint8_t)(a + (((n - a) * w + 128) >> 8));
    }
    uint64_t hash = fnv(mix, XF_SIZE);
    memcpy((void *)(uintptr_t)xf[c].addr, mix, XF_SIZE);
    memcpy((void *)(uintptr_t)xf[o].addr, mix, XF_SIZE);
    xf[c].dirty = xf[o].dirty = 1, xf[c].hash = xf[o].hash = hash;
}

// ---- curves
// k for the active post set: only while its blend runs (the field blend main+0x3fb374 also serves another, shorter
// fade of the post manager, which keeps its straight line outside the dawn and dusk blends).
static double curve_update(const Profile *p, const Active *a) {
    uint8_t *pg = rt_patch_page();
    double k = 0;
    // until a minute past the blend's end, so the field blend's last frames still get the curve
    if (a->ok && a->blend > 0 && a->since < a->blend + 1) k = a->start >= 12 * 60 ? p->k_dusk : p->k_dawn;
    float f = (float)k;
    if (pg) memcpy(pg + PG_CURVE, &f, 4);
    return k;
}

static void light_time_update(const Profile *p, double h) {
    uint8_t *pg = rt_patch_page();
    if (!pg) return;
    // the time the light keys see: time * a + b. a stays 1 and b is the eased time's offset from the real one, which
    // is 0 at both edges of the window and moves little from frame to frame, so a read that takes a and b from two
    // different writes is still right (the patched code loads them one at a time)
    float ab[2] = {1.0f, 0.0f};
    if (h >= 0 && p->ease_len > 0) {
        double u = (h * 60.0 - p->ease_start) / p->ease_len;
        if (u > 0 && u < 1) {
            double v = pow(u, p->ease_pow);
            v = v * v * (3 - 2 * v);
            ab[1] = (float)((p->ease_len * v - p->ease_len * u) / 1440.0);
        }
    }
    uint64_t v;
    memcpy(&v, ab, 8);
    __atomic_store_n((uint64_t *)(pg + PG_LIGHT), v, __ATOMIC_RELEASE);
}

static int parse_mode(const char *s) {
    return !strcmp(s, "extended") ? DD_EXTENDED : !strcmp(s, "smoothed") ? DD_SMOOTHED : DD_ORIGINAL;   // else Original
}

// The night light keys' intensity for a Night darkness choice. Measured on screen at a Mira view at 22:00 (the near
// ground's brightness, the game's night 34): x0.5 gave 64 %, x0.35 55 %; Darker x0.70 gives ~78 % (as dark as the
// game's own dip at 05:00), Darkest x0.42 ~60 %. The sky (fog keys) stays the game's.
static double night_scale(const char *d) {
    return !strcmp(d, "darkest") ? 0.42 : !strcmp(d, "darker") ? 0.70 : 1.0;   // else Original
}

static void set_night(const char *d) {
    double nl = night_scale(d);
    if (nl != night_light) rt_log("dark nights: %s, night light keys x%.2f", d, nl);
    night_light = nl;
}

static void read_setting(void) {
    unsigned v = cfg_version();
    if (v == cfg_seen) return;
    cfg_seen = v;
    int m = parse_mode(cfg_value("XCX_DAWN_DUSK"));
    set_night(cfg_value("XCX_DARK_NIGHTS"));
    if (m == mode) return;
    mode = m;
    rt_log("dawn/dusk: %s", m == DD_EXTENDED ? "smoothed & extended" : m == DD_SMOOTHED ? "smoothed" : "original");
}

// Every present (boot.c).
void rt_dawn_dusk_update(void) {
    static int on;   // anything of ours is in game memory or switched on
    static int env_on, keys_wait;   // ddkeys is on; the keys wait for the game's next key address to be written
    if (s_ddlut == -2) {
        s_ddlut = rt_patch_set("ddlut"), s_curve = rt_patch_set("ddcurve"), s_light = rt_patch_set("ddlight");
        s_env = rt_patch_set("ddkeys");
        // the four sets follow the settings, whatever their XCX_DD_*_PATCH keys say, and their page slots start
        // neutral: no curve, the real time, no saved pointers
        uint8_t *pg = rt_patch_page();
        if (pg) {
            float k = 0, ab[2] = {1.0f, 0.0f};
            memcpy(pg + PG_CURVE, &k, 4), memcpy(pg + PG_LIGHT, ab, 8), memset(pg + PG_LUT, 0, 8), memset(pg + PG_KEYS, 0, 8);
        }
        rt_patch_switch(s_ddlut, 0), rt_patch_switch(s_curve, 0), rt_patch_switch(s_light, 0), rt_patch_switch(s_env, 0);
    }
    if (s_ddlut < 0 || s_curve < 0 || s_light < 0 || s_env < 0 || !rt_patch_page()) return;
    int was = mode;
    double was_nl = night_light;
    read_setting();
    int changed = mode != was || night_light != was_nl;   // the keys and sets are written again
    const Profile *p = &profiles[mode];
    int want = (mode != DD_ORIGINAL || night_light < 1) && rt_day_length_state() == 1;
    if (!want) {   // Original + Original (or no clock): give everything back once, then nothing at all
        if (on) {
            rt_patch_switch(s_curve, 0), rt_patch_switch(s_light, 0), rt_patch_switch(s_ddlut, 0), rt_patch_switch(s_env, 0);
            env_on = 0, keys_wait = 0;
            xf_restore(), xf_forget(), keys_release_all(wsec()), sets_release();
            uint8_t *pg = rt_patch_page();
            memset(pg + PG_LUT, 0, 8), memset(pg + PG_KEYS, 0, 8);
            on = 0;
            rt_log("dawn/dusk: everything given back");
        }
        return;
    }
    on = 1;
    // the keys only when the choice moves or dims some (Smoothed & Extended, Night darkness), the sets only when it
    // moves some (Smoothed & Extended); otherwise what was written is given back and they are left alone.
    int need_keys = p->nkeys || night_light < 1, need_sets = p->nsets > 0;
    double w = wsec();
    if (!need_keys && env_on) {
        rt_patch_switch(s_env, 0), env_on = 0;
        keys_release_all(w);
    }
    if (!need_sets && sets_listed) sets_release();
    // the page first, then the code that reads it
    Active a = active_read();
    double k = mode != DD_ORIGINAL ? curve_update(p, &a) : 0;
    if (mode != DD_ORIGINAL) light_time_update(p, a.h);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    rt_patch_switch(s_curve, mode != DD_ORIGINAL), rt_patch_switch(s_light, mode != DD_ORIGINAL);
    rt_patch_switch(s_ddlut, 1);
    if (need_keys && !env_on) {
        // ddkeys from off: its slot cleared first (a game thread inside the site when it was switched off may have
        // stored an old map's address after we cleared it), so only an address the game stores from now on is used;
        // the keys are then looked at every frame until it does, so the change applies at once
        memset(rt_patch_page() + PG_KEYS, 0, 8);
        rt_patch_switch(s_env, 1), env_on = 1, keys_wait = 1;
    }
    // the tables: crossfaded on every choice but Original + Original (on Original's times with Night darkness, the
    // game's straight blend, so its table switch at 05:00 and 18:45 doesn't step the light)
    tables_update(&a, k);
    // key and set times: every second (a map load reads them again), at once on a change of choice
    static double last = -10;
    if (changed && need_keys) keys_wait = 1;
    if (need_keys && keys_wait && w >= 0) {
        uint64_t T = 0;
        memcpy(&T, rt_patch_page() + PG_KEYS, 8);
        if (T) keys_update(p, 1, w), keys_wait = 0;
    }
    if (w < 0 || (!changed && w - last < 1.0)) return;
    last = w;
    if (need_keys) keys_update(p, 0, w);
    if (need_sets) sets_update(p, changed);
}
