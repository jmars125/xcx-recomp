// Day length (XCX_DAY_LENGTH, the Game tab's Day length row): how many real minutes a game day lasts. 24 is the game's
// own, a game minute each real second; 12 runs the clock twice as fast, 48 half as fast, 0 stops it. It applies at
// once and can be changed while playing. The light, the sky, enemies and crowds follow the clock as they do for the
// game's own Change Time, which keeps working on top.
//
// The game's clock object (2.0.0) is at main + 0x1FEFC38. Its update, main + 0x1e7640, runs every frame:
//   +0x18 u64  the real time of its last tick (the system clock, packed: day_length.h)
//   +0x2C u32  the game time, packed: minute in bits 6-11, hour in bits 12-16, a day count from bit 17
//   +0x30 f32  the time of day, hour * 1800 + minute * 30, moved smoothly between ticks
// On a frame where whole real seconds have passed since +0x18, the update adds that many game minutes to +0x2C, stores
// the new real time and recomputes +0x30 and the time period from it. The rate is fixed in its code, so this file
// changes the clock from outside, once a frame from the present (boot.c rt_frame_presented): faster by winding the
// stamp back a second whenever an extra minute is due, slower by rescaling each tick's minutes (day_length.h).
//
// Only for 2.0.0, the build the addresses are from. Before it first writes, it also checks that the clock reads as one
// (the float follows the packed time), and leaves it alone for the session if three checks in a row say no; the row
// then says so (rt_day_length_state).
//
// Not traced, accepted: on each of the game's ticks at Stopped or a slower day, the game first computes everything that
// follows from its new minute (the time period, last frame's float at +0x34) before the time is put back; at Stopped on
// a minute 59, it sees the hour change once a real second. Whether any hourly game logic acts on that is not known;
// nothing was seen in the tests. Between ticks the game moves the float a little and Stopped puts it back each frame,
// so the light could move by up to a game minute's worth before it is put back.
#include "cfg.h"
#include "day_length.h"
#include "host.h"
#include "platform/platform.h"
#include <math.h>
#include <string.h>

extern const char rt_game_name[];

#define GAMEDATA_PTR 0x1DC1408ull   // the game data object's pointer, set once a save has loaded
#define CLOCK 0x1FEFC38ull
#define C_STAMP 0x18
#define C_TIME 0x2C
#define C_FLOAT 0x30

static double speed = 1.0;
static unsigned cfg_seen = ~0u;
static int running, trusted, broken, bad_checks;   // running/trusted: kept for the session (see clock_ready)
static float first_float;    // the clock's first nonzero float: it runs once the float moved from there
static int64_t last_check_us = -1;
static DlFast fast;
static DlSlow slow;

// 0 on a game build without the clock's addresses, 2 when the clock didn't read as one this session, else 1.
int rt_day_length_state(void) { return strcmp(rt_game_name, "2.0.0") ? 0 : broken ? 2 : 1; }

static uint64_t main_base(void) {
    const RtLoaded *m = ldr_module(0);
    return m ? m->base : 0;
}

static int save_loaded(uint64_t b) {
    uint64_t p = 0;
    return plat_mem_read(b + GAMEDATA_PTR, &p, sizeof p) && p && p < RT_ADDRESS_LIMIT;
}

static int clock_consistent(uint64_t b);
static int clock_ready(uint64_t b, int64_t wu);

// The time of day now in hours [0, 24) from the clock's float, or -1 without the clock (another game build, no save
// loaded yet, or a clock that doesn't read as one yet: clock_ready, the same check Day length makes before its first
// write, so both settings trust the clock from the same moment and give up on it together). For other settings that
// follow the time (dawn_dusk.c).
double rt_day_length_hours(void) {
    uint64_t b = main_base();
    int64_t wu = frame_step_world_us();
    float f = 0;
    if (rt_day_length_state() != 1 || wu < 0 || !b || !save_loaded(b) || !clock_ready(b, wu) ||
        !plat_mem_read(b + CLOCK + C_FLOAT, &f, 4) || !isfinite(f))
        return -1;
    double h = fmod(f / 1800.0, 24.0);
    return h < 0 ? h + 24.0 : h;
}

// The float follows the packed time: from the minute's start to the next one's (a little slack for the frame between
// the two writes, and midnight's wrap). A few misses in a row: this isn't the clock, leave it alone.
static int clock_consistent(uint64_t b) {
    uint8_t v[8];   // the packed time (+0x2C) and the float (+0x30) in one read (stored apart: the slack covers that)
    uint32_t t;
    float f;
    if (!plat_mem_read(b + CLOCK + C_TIME, v, sizeof v)) return 0;
    memcpy(&t, v, 4), memcpy(&f, v + C_FLOAT - C_TIME, 4);
    if (!isfinite(f)) return 0;
    double d = f - (double)(dl_unpack(t) % 1440) * 30.0;
    if (d < -43200.0 + 90.0) d += 43200.0;
    return d > -30.0 && d < 90.0;
}

// The setting, re-read when the settings change (the menu, a reloaded xcx.ini). A change starts both modes afresh:
// what one kept (the minutes it last wrote, a stamp it wound back) means nothing to the other speed.
static void read_setting(void) {
    unsigned v = cfg_version();
    if (v == cfg_seen) return;
    cfg_seen = v;
    double s = dl_speed(cfg_value("XCX_DAY_LENGTH"));
    if (s == speed) return;
    speed = s;
    memset(&fast, 0, sizeof fast);
    memset(&slow, 0, sizeof slow);
    if (s == 0) rt_log("day length: stopped");
    else rt_log("day length: %g real minutes (clock speed %gx)", 24.0 / s, s);
}

static void run_fast(uint64_t b) {
    uint64_t *sp = (uint64_t *)(uintptr_t)(b + CLOCK + C_STAMP);
    uint64_t s = __atomic_load_n(sp, __ATOMIC_ACQUIRE), back;
    // Accepted: a swap landing between the game's read of the stamp and its store of the new one is overwritten; that
    // extra minute is then lost (a rare undercount, never a burst).
    if (dl_fast_step(&fast, plat_now_ns(), s, speed, &back) &&
        __atomic_compare_exchange_n(sp, &s, back, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        dl_fast_took(&fast, back);
}

static void run_slow(uint64_t b) {
    uint32_t *tp = (uint32_t *)(uintptr_t)(b + CLOCK + C_TIME);
    uint64_t stamp = __atomic_load_n((uint64_t *)(uintptr_t)(b + CLOCK + C_STAMP), __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n(tp, __ATOMIC_RELAXED);
    uint64_t minutes;
    double frac;
    if (!dl_slow_step(&slow, stamp, dl_unpack(t), speed, &minutes, &frac)) return;
    __atomic_store_n(tp, dl_pack(minutes, t), __ATOMIC_RELAXED);
    float f = (float)((minutes % 1440) * 30.0 + frac * 30.0);
    memcpy((void *)(uintptr_t)(b + CLOCK + C_FLOAT), &f, 4);
}

// The clock can be used: it runs (the float is 0 while the save loads, then ticks) and has read as a clock once,
// checked once a second of world clock; three misses in a row and it is left alone for the session (state 2: the Day
// and night rows are greyed out). Both settings ask here (rt_day_length_update, rt_day_length_hours), so the clock is
// checked by one rule, at most once a second, whichever of them is on.
static int clock_ready(uint64_t b, int64_t wu) {
    if (trusted) return 1;
    if (broken) return 0;
    if (!running) {
        float f = 0;
        if (!plat_mem_read(b + CLOCK + C_FLOAT, &f, 4) || f == 0) return 0;
        if (first_float == 0) first_float = f;
        if (f == first_float) return 0;
        running = 1;
    }
    if (last_check_us >= 0 && wu - last_check_us < 1000000) return 0;
    last_check_us = wu;
    if (clock_consistent(b)) return trusted = 1;
    if (++bad_checks >= 3) {
        broken = 1;
        rt_log("day length: the game's clock doesn't read as expected; Day and night settings left alone for this session");
    }
    return 0;
}

// Every present (boot.c rt_frame_presented).
void rt_day_length_update(void) {
    read_setting();
    if (speed == 1.0 || rt_day_length_state() != 1) return;
    // The world clock runs from the first present (the title screen too); the guard is the save's game data. Not
    // traced: whether that pointer is cleared on a return to the title, so a held clock may still be written there.
    int64_t wu = frame_step_world_us();
    uint64_t b = main_base();
    if (wu < 0 || !b || !save_loaded(b)) return;
    if (!clock_ready(b, wu)) return;
    if (speed > 1.0) run_fast(b);
    else run_slow(b);
}
