// runtime/day_length.h on a model of the game's clock: the setting's parsing, the packed time, the stamp's steps back
// and the held stamp, and whole runs of the faster and slower modes at 60 and 30 frames a second, with the game
// holding its clock (its hold flag, a stop), stalls, a time change from the game's menu and a save load moving it,
// and real midnight. No game: the clock is simulated here the way runtime/day_length.h describes the game's update
// (every frame it stores the time now as its stamp and adds a game minute for each whole second, counted field by
// field, since the stamp it replaced). Every run also checks that the game time never goes back except where the test
// moves it (a menu time change, a load), and every slower run that the game adds at most the one minute it was let
// through per frame, so no hour or day is crossed early. Built and run by tests/day_length_test.py.
#include <stdio.h>
#include <string.h>

#include "day_length.h"

static int failures, checks;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        checks++;                                                                                                     \
        if (!(cond)) {                                                                                                \
            failures++;                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

static int near(double a, double b, double tol) { return a >= b - tol && a <= b + tol; }

// ---- the model: real time in ns from the start of a year, the game's stamp, game minutes and the float
typedef struct {
    uint64_t real_ns;   // real time since the start of the year
    uint64_t stamp;     // packed like the game's (day_length.h), with bits we keep
    uint32_t time;      // the packed game time
    float f;            // the time of day float, hour * 1800 + minute * 30
    int paused;         // the clock stands still (the title screen, a load): its update doesn't run
    int flag;           // the update's own hold (+0x8 bit 0): it stores the time now as the stamp and adds nothing
    int updates;        // the game's updates between two of our presents (1; 0 or 2 when the threads drift)
    int moved;          // the test moved the time itself this frame (a menu time change, a load): may go back
    uint64_t low;       // the lowest game time since the test last moved it, for the never-back check
    const char *what;   // the run, for messages
    double snap_back;   // the largest step back of the float the game's own update shows (its minute's snap)
    int jump_ok;        // a test makes the game count many seconds at once (a stall): more than a minute may come
} Game;

// Bits outside the time fields (bits 0-5, the day of the month, the month, the year): they must survive.
static const uint64_t KEEP = 0x2Bull | (9ull << 33) | (5ull << 47) | (2026ull << 51);
static const uint64_t TIME_FIELDS = (1023ull << 6) | (63ull << 16) | (63ull << 22) | (31ull << 28) | (511ull << 38);

static uint64_t pack_stamp(uint64_t ns, uint64_t keep) {
    uint64_t ms = ns / 1000000 % 1000, s = ns / 1000000000ull, day = s / 86400 % 365, sec = s % 86400;
    return (keep & ~TIME_FIELDS) | ms << 6 | (sec % 60) << 16 | (sec / 60 % 60) << 22 | (sec / 3600) << 28 | day << 38;
}
static uint64_t stamp_seconds(uint64_t st) {   // within the day; the hour field may hold up to 31
    return ((st >> 28) & 31) * 3600 + ((st >> 22) & 63) * 60 + ((st >> 16) & 63);
}
// Whole seconds from old to now, field by field as the game counts them (signed).
static int32_t game_elapsed(uint64_t now, uint64_t old) {
    int32_t d = (int32_t)((now >> 16) & 63) - (int32_t)((old >> 16) & 63);
    d += ((int32_t)((now >> 22) & 63) - (int32_t)((old >> 22) & 63)) * 60;
    d += ((int32_t)((now >> 28) & 31) - (int32_t)((old >> 28) & 31)) * 3600;
    d += ((int32_t)((now >> 38) & 511) - (int32_t)((old >> 38) & 511)) * 86400;
    return d;
}

static void game_init(Game *g, uint64_t start_sec, uint64_t game_minutes, const char *what) {
    g->real_ns = start_sec * 1000000000ull + 123000000ull;
    g->stamp = pack_stamp(g->real_ns, KEEP);
    g->time = dl_pack(game_minutes, 0x15);
    g->f = (float)((game_minutes % 1440) * 30);
    g->paused = 0, g->moved = 0, g->low = game_minutes, g->what = what, g->snap_back = 0, g->jump_ok = 0;
    g->flag = 0, g->updates = 1;
}

// The game's update on one frame (main + 0x1e7640 as runtime/day_length.c describes it).
static void game_update(Game *g) {
    if (g->paused) return;
    uint64_t old = g->stamp, now = pack_stamp(g->real_ns, g->stamp);
    g->stamp = now;
    if (g->flag) return;
    int32_t el = game_elapsed(now, old);
    if (el >= 1) {
        uint64_t m = dl_unpack(g->time) + (uint64_t)el;
        g->time = dl_pack(m, g->time);
        g->f = (float)((m % 1440) * 30);
    } else {
        // as the update's code does (main + 0x1e7640, the lifted C): milliseconds that went back count from 1000 more
        int32_t ms = (int32_t)((now >> 6) & 1023) - (int32_t)((old >> 6) & 1023);
        if (ms < 0) ms += 1000;
        g->f += (float)ms * 30.0f / 1000.0f;
    }
}

// One frame: the game's update, then our code as the present runs it (day_length.c run_fast / run_slow).
static void frame(Game *g, DlFast *fast, DlSlow *slow, double speed, double fps) {
    g->real_ns += (uint64_t)(1e9 / fps);
    float shown = g->f;
    for (int u = 0; u < g->updates; u++) game_update(g);
    if (!g->moved && shown - g->f > g->snap_back) g->snap_back = shown - g->f;
    if (speed > 1.0) {
        uint64_t back;
        if (dl_fast_step(fast, g->real_ns, g->stamp, speed, &back)) {
            g->stamp = back;   // our compare-and-swap: the game is not writing in the model
            dl_fast_took(fast, back);
        }
    } else if (speed != 1.0) {
        uint64_t w;
        double frac;
        double shown = g->f / 30.0 - (double)(dl_unpack(g->time) % 1440);
        if (dl_slow_step(slow, g->real_ns, g->stamp, dl_unpack(g->time), g->flag, shown, speed, &w, &frac)) {
            g->stamp = w;
            dl_slow_took(slow, g->real_ns, w);
        }
        CHECK(frac >= 0 && frac <= 1, "%s: the float's fraction %g is within a minute", g->what, frac);
        g->f = (float)((dl_unpack(g->time) % 1440) * 30 + frac * 30);
    }
    uint64_t m = dl_unpack(g->time);
    if (speed < 1.0 && !g->moved && !g->jump_ok)   // a slower day: at most the one minute it let through, per frame
        CHECK(m <= g->low + 1, "%s: %llu minutes in one frame at %gx", g->what, (unsigned long long)(m - g->low),
              speed);
    if (g->moved) g->low = m, g->moved = 0;
    CHECK(m >= g->low, "%s: the game time went back from %llu to %llu", g->what, (unsigned long long)g->low,
          (unsigned long long)m);
    if (m > g->low) g->low = m;
    CHECK((g->time & 63) == 0x15, "%s: the low bits the game keeps in the time are left alone", g->what);
    CHECK((g->stamp & ~TIME_FIELDS) == (KEEP & ~TIME_FIELDS), "%s: the stamp's other bits are left alone", g->what);
}
static void move_time(Game *g, uint64_t minutes) {   // a time change from the game's menu, or a save load
    g->time = dl_pack(minutes, g->time);
    g->f = (float)((minutes % 1440) * 30);
    g->moved = 1;
}

// Game minutes a run of real seconds adds at a speed and frame rate.
static double run(double speed, double fps, double seconds, uint64_t start_sec) {
    Game g;
    DlFast fast = {0};
    DlSlow slow = {0};
    char what[80];
    snprintf(what, sizeof what, "run %gx at %g fps from %llu s", speed, fps, (unsigned long long)start_sec);
    game_init(&g, start_sec, 2 * 1440 + 600, what);
    uint64_t t0 = dl_unpack(g.time);
    for (int i = 0; i < (int)(seconds * fps); i++) frame(&g, &fast, &slow, speed, fps);
    return (double)(dl_unpack(g.time) - t0);
}

static void test_speed(void) {
    CHECK(dl_speed(NULL) == 1.0 && dl_speed("") == 1.0, "unset is the game's own");
    CHECK(dl_speed("24") == 1.0, "24 is the game's own");
    CHECK(dl_speed("12") == 2.0 && dl_speed("1") == 24.0, "shorter days are faster");
    CHECK(dl_speed("48") == 0.5 && near(dl_speed("144"), 1.0 / 6, 1e-12), "longer days are slower");
    CHECK(dl_speed("0") == 0.0, "0 stops the clock");
    CHECK(dl_speed("1.5") == 16.0, "fractions are read (xcx.ini)");
    CHECK(dl_speed("0.5") == 24.0 && dl_speed("0.01") == 24.0, "under a minute counts as 1 (one minute a frame at most)");
    CHECK(dl_speed("abc") == 1.0 && dl_speed("-5") == 1.0 && dl_speed("nan") == 1.0, "nonsense is the game's own");
    CHECK(dl_speed("12x") == 1.0 && dl_speed("12 ") == 1.0, "a number with anything after it is the game's own, as the menu reads it");
    CHECK(near(dl_speed("100000"), 24.0 / 1440, 1e-12), "longer than a real day counts as a real day");
}

static void test_pack(void) {
    CHECK(dl_pack(2 * 1440 + 12 * 60 + 50, 0) == 314496, "day 2, 12:50 packs as the game's clock showed it");
    CHECK(dl_unpack(314496) == 2 * 1440 + 12 * 60 + 50, "and unpacks back");
    CHECK((dl_pack(100, 0xFF) & 63) == 63, "the low 6 bits are kept");
    for (uint64_t m = 0; m < 5 * 1440; m += 7) CHECK(dl_unpack(dl_pack(m, 0)) == m, "round trip at %llu", (unsigned long long)m);
}

static void test_stamp(void) {
    uint64_t base = 40ull * 86400 * 1000000000ull;
    uint64_t s = pack_stamp(base + (10 * 3600 + 5 * 60 + 7) * 1000000000ull, KEEP);
    CHECK(dl_stamp_back_1s(&s) && stamp_seconds(s) == 10 * 3600 + 5 * 60 + 6, "a second back");
    s = pack_stamp(base + (10 * 3600 + 5 * 60) * 1000000000ull, KEEP);
    CHECK(dl_stamp_back_1s(&s) && stamp_seconds(s) == 10 * 3600 + 4 * 60 + 59, "borrows from the minute");
    s = pack_stamp(base + 10 * 3600 * 1000000000ull, KEEP);
    CHECK(dl_stamp_back_1s(&s) && stamp_seconds(s) == 9 * 3600 + 59 * 60 + 59, "borrows from the hour");
    CHECK((s & ~TIME_FIELDS) == (KEEP & ~TIME_FIELDS), "the other bits stay");
    s = pack_stamp(base, KEEP);
    uint64_t before = s;
    CHECK(!dl_stamp_back_1s(&s) && s == before, "real midnight: no step back, stamp unchanged");

    // The held stamp: 31:59:59 today, the day and the milliseconds kept; the game counts no whole second from it until
    // 08:00 of the next real day, whatever the time of day it was held at.
    const uint64_t at[] = {0, 12 * 3600 + 34, 23 * 3600 + 59 * 60 + 30};
    for (int i = 0; i < 3; i++) {
        uint64_t now = pack_stamp(base + at[i] * 1000000000ull, KEEP), h = dl_stamp_held(now);
        CHECK(stamp_seconds(h) == 31 * 3600 + 59 * 60 + 59, "held: 31:59:59");
        CHECK(((h >> 38) & 511) == ((now >> 38) & 511) && ((h >> 6) & 1023) == ((now >> 6) & 1023) &&
                  (h & ~TIME_FIELDS) == (KEEP & ~TIME_FIELDS),
              "held: the day, the milliseconds and the other bits stay");
        CHECK(game_elapsed(now, h) < 0, "held at %llu s: no whole second counted at once", (unsigned long long)at[i]);
        uint64_t before8 = pack_stamp(base + (86400 + 7 * 3600 + 59 * 60 + 59) * 1000000000ull, KEEP);
        uint64_t at8 = pack_stamp(base + (86400 + 8 * 3600) * 1000000000ull, KEEP);
        CHECK(game_elapsed(before8, h) <= 0 && game_elapsed(at8, h) >= 1,
              "held at %llu s: counted from 08:00 the next day (%d, %d)", (unsigned long long)at[i],
              game_elapsed(before8, h), game_elapsed(at8, h));
    }
}

// Every step of the menu at 60 and 30 frames a second: the game minutes a run of real seconds adds, against 24 / len.
static void test_runs(void) {
    static const double lens[] = {1, 2, 3, 6, 12, 24, 48, 96, 144, 0};
    const double fps[] = {60, 30};
    for (int f = 0; f < 2; f++) {
        for (size_t i = 0; i < sizeof lens / sizeof *lens; i++) {
            double len = lens[i], speed = len ? 24.0 / len : 0, secs = len > 12 ? 600 : 120;
            double want = secs * speed, got = run(speed, fps[f], secs, 40 * 86400 + 36000);
            double tol = want * 0.03 > 2 ? want * 0.03 : 2;
            if (len == 0) tol = 0;
            CHECK(near(got, want, tol), "%g-minute day at %g fps: %g game minutes in %g s, want %g", len, fps[f], got,
                  secs, want);
        }
    }
    // Across real midnight, faster and slower: one frame a day can't step back, nothing else changes.
    uint64_t late = 40 * 86400 + 86400;
    CHECK(near(run(24, 60, 60, late - 30), 1440, 1440 * 0.03), "1-minute day across real midnight (%g)",
          run(24, 60, 60, late - 30));
    CHECK(near(run(0.5, 60, 120, late - 60), 60, 2), "48-minute day across real midnight (%g)", run(0.5, 60, 120, late - 60));
    CHECK(run(0, 60, 120, late - 60) == 0, "stopped across real midnight");
    CHECK(run(0, 60, 7200, late - 3600) == 0, "stopped for two real hours across midnight");
}

// Stopped at a minute 59 and at 23:59: the game never sees the next hour or day, not even for a frame.
static void test_no_crossing(void) {
    const uint64_t at[] = {2 * 1440 + 10 * 60 + 59, 2 * 1440 + 23 * 60 + 59};
    for (int k = 0; k < 2; k++) {
        Game g;
        DlFast fast = {0};
        DlSlow slow = {0};
        game_init(&g, 40 * 86400 + 36000, at[k], "stopped at :59");
        for (int i = 0; i < 60 * 60; i++) {
            frame(&g, &fast, &slow, 0, 60);
            CHECK(dl_unpack(g.time) == at[k], "stopped at %llu: the time stays (frame %d: %llu)", (unsigned long long)at[k],
                  i, (unsigned long long)dl_unpack(g.time));
            if (dl_unpack(g.time) != at[k]) break;
        }
    }
    // A 48-minute day: every minute comes once, in order, from the game's own tick (the never-back check in frame()).
    Game g;
    DlFast fast = {0};
    DlSlow slow = {0};
    game_init(&g, 40 * 86400 + 36000, 2 * 1440 + 23 * 60 + 50, "48-minute day over a game midnight");
    for (int i = 0; i < 60 * 40; i++) frame(&g, &fast, &slow, 0.5, 60);
    CHECK(dl_unpack(g.time) >= 3 * 1440, "and it reaches the next game day (%llu)", (unsigned long long)dl_unpack(g.time));
}

// The light: at Stopped the float holds; on a slower day it rises smoothly, never jumping back.
static void test_float(void) {
    const double speeds[] = {0, 0.5, 1.0 / 6};
    for (int k = 0; k < 3; k++) {
        Game g;
        DlFast fast = {0};
        DlSlow slow = {0};
        game_init(&g, 40 * 86400 + 36000, 2 * 1440 + 600, "float");
        for (int i = 0; i < 60; i++) frame(&g, &fast, &slow, speeds[k], 60);
        float prev = g.f, f0 = g.f;
        int backs = 0;
        double big = 0;
        for (int i = 0; i < 60 * 60; i++) {
            frame(&g, &fast, &slow, speeds[k], 60);
            if (g.f < prev) backs++;
            if (g.f - prev > big) big = g.f - prev;
            prev = g.f;
        }
        if (speeds[k] == 0) CHECK(g.f == f0, "stopped: the float holds (%g -> %g)", f0, g.f);
        CHECK(g.snap_back <= 1.0, "%gx: the game's own minute never pulls the light back visibly (%g)", speeds[k], g.snap_back);
        CHECK(backs == 0, "%gx: the float never steps back (%d times)", speeds[k], backs);
        CHECK(big <= 30 * speeds[k] / 60 * 3 + 0.01, "%gx: no jump in the float (largest step %g)", speeds[k], big);
    }
}

// The game holds its clock two ways (traced: its update's own hold flag, +0x8 bit 0, which stores the time now and
// adds nothing; and a stop, where the update doesn't run: the title screen, a load). Nothing owed piles up into a
// burst after either, and a stop isn't counted when it ends, also when the game itself doesn't set its stamp afresh.
static void test_pause(void) {
    for (int how = 0; how < 2; how++) {   // 0: the update stops, 1: the update's hold flag
        const double speeds[] = {24, 0.5, 1.0 / 6, 0};
        for (int k = 0; k < 4; k++) {
            double sp = speeds[k];
            Game g;
            DlFast fast = {0};
            DlSlow slow = {0};
            game_init(&g, 40 * 86400 + 36000, 600, how ? "the game's hold flag" : "a stop");
            for (int i = 0; i < 120; i++) frame(&g, &fast, &slow, sp, 60);
            if (how) g.flag = 1;
            else g.paused = 1;
            uint64_t at_pause = dl_unpack(g.time);
            for (int i = 0; i < 60 * 30; i++) frame(&g, &fast, &slow, sp, 60);
            CHECK(dl_unpack(g.time) == at_pause, "%s at %gx: the clock holds", g.what, sp);
            // Slower days: the stamp as the stop left it (not set afresh), the harder case. Faster days hold nothing,
            // so there the game's own update counts whatever it finds, as at its own speed: the stamp is set afresh,
            // as a load does (traced).
            if (sp > 1) g.stamp = pack_stamp(g.real_ns, g.stamp);
            g.flag = 0, g.paused = 0;
            for (int i = 0; i < 60; i++) frame(&g, &fast, &slow, sp, 60);
            double added = (double)(dl_unpack(g.time) - at_pause);
            CHECK(sp > 1 ? added <= sp + 3 : added <= 1, "%s at %gx: the first second after adds a second's worth, not a "
                  "burst (%g)", g.what, sp, added);
        }
    }
}

// The game's updates and our presents drift apart (some presents see no update, the next sees two): a slower day
// keeps its speed, because what is owed grows by the real time between the game's updates.
static void test_drift(void) {
    const double speeds[] = {0.5, 1.0 / 6};
    for (int k = 0; k < 2; k++) {
        Game g;
        DlFast fast = {0};
        DlSlow slow = {0};
        game_init(&g, 40 * 86400 + 36000, 600, "0 and 2 updates a present");
        uint64_t t0 = dl_unpack(g.time);
        for (int i = 0; i < 600 * 60; i++) {
            g.updates = i % 3 == 0 ? 0 : i % 3 == 1 ? 2 : 1;
            frame(&g, &fast, &slow, speeds[k], 60);
        }
        double got = (double)(dl_unpack(g.time) - t0), want = 600 * speeds[k];
        CHECK(near(got, want, want * 0.03 + 1), "%s at %gx: %g game minutes in 600 s, want %g", g.what, speeds[k], got,
              want);
    }
}

// A load (or the title screen) starts in the frame right after a due minute was let through (the stamp a second back):
// that second back is turned into a hold within 0.1 s, so the load's length isn't counted when the clock runs again.
static void test_stop_after_release(void) {
    Game g;
    DlFast fast = {0};
    DlSlow slow = {0};
    game_init(&g, 40 * 86400 + 36000, 600, "stop after a release");
    for (int i = 0; i < 120; i++) frame(&g, &fast, &slow, 0.5, 60);
    for (int i = 0; i < 600 && !(slow.released && g.stamp == slow.wrote); i++) frame(&g, &fast, &slow, 0.5, 60);
    CHECK(slow.released && g.stamp == slow.wrote, "the case was set up (a second back pending)");
    uint64_t before = dl_unpack(g.time);
    g.paused = 1;
    for (int i = 0; i < 60 * 20; i++) frame(&g, &fast, &slow, 0.5, 60);
    g.paused = 0;
    g.jump_ok = 1;
    for (int i = 0; i < 2; i++) frame(&g, &fast, &slow, 0.5, 60);
    uint64_t got = dl_unpack(g.time) - before;
    CHECK(got <= 1, "a 20 s stop right after a release: %llu game minutes, not 20", (unsigned long long)got);
}

// A stall: no frames for 15 real seconds, then the game's update counts them all. A slower day's hold covers it,
// except in the one frame a due minute is let through (the stamp a second back): a stall starting exactly there is
// counted whole, as the game's own speed would count it (accepted, day_length.c run_slow).
static void test_stall(void) {
    const double speeds[] = {0, 0.5};
    for (int k = 0; k < 2; k++) {
        for (int at_release = 0; at_release < 2; at_release++) {
            if (at_release && speeds[k] == 0) continue;   // Stopped never lets one through
            Game g;
            DlFast fast = {0};
            DlSlow slow = {0};
            game_init(&g, 40 * 86400 + 36000, 600, "stall");
            for (int i = 0; i < 120; i++) frame(&g, &fast, &slow, speeds[k], 60);
            for (int i = 0; i < 600 && (at_release ? g.stamp != slow.wrote || slow.owed < 1 : slow.owed >= 0.5); i++)
                frame(&g, &fast, &slow, speeds[k], 60);
            uint64_t before = dl_unpack(g.time);
            g.real_ns += 15ull * 1000000000ull;
            g.jump_ok = 1;
            for (int i = 0; i < 2; i++) frame(&g, &fast, &slow, speeds[k], 60);
            uint64_t got = dl_unpack(g.time) - before;
            if (at_release)
                CHECK(got <= 17, "a 15 s stall from the release frame at %gx: counted whole at most (%llu)", speeds[k],
                      (unsigned long long)got);
            else
                CHECK(got <= 1, "a 15 s stall at %gx: %llu game minutes", speeds[k], (unsigned long long)got);
        }
    }
}

// A time change from the game's menu (or a load) moves the clock: followed, and the slower speed goes on from there.
static void test_jumps(void) {
    Game g;
    DlFast fast = {0};
    DlSlow slow = {0};
    game_init(&g, 40 * 86400 + 36000, 600, "jumps");
    for (int i = 0; i < 600; i++) frame(&g, &fast, &slow, 0.5, 60);
    uint64_t before = dl_unpack(g.time);
    move_time(&g, before + 9 * 60);   // a menu time change: 9 hours on
    for (int i = 0; i < 2; i++) frame(&g, &fast, &slow, 0.5, 60);
    CHECK(dl_unpack(g.time) >= before + 9 * 60, "a jump forward is kept");
    uint64_t after = dl_unpack(g.time);
    for (int i = 0; i < 120 * 60; i++) frame(&g, &fast, &slow, 0.5, 60);
    CHECK(near((double)(dl_unpack(g.time) - after), 60, 2), "half speed goes on after the jump (%llu)",
          (unsigned long long)(dl_unpack(g.time) - after));
    move_time(&g, 300);   // a save loaded: earlier
    for (int i = 0; i < 2; i++) frame(&g, &fast, &slow, 0.5, 60);
    CHECK(dl_unpack(g.time) < 310, "a jump back is followed, not undone");
    // Stopped also follows a jump: a menu time change still moves a stopped clock, which then holds there.
    for (int i = 0; i < 60; i++) frame(&g, &fast, &slow, 0, 60);
    uint64_t held = dl_unpack(g.time);
    move_time(&g, held + 6 * 60);
    for (int i = 0; i < 2 * 60; i++) frame(&g, &fast, &slow, 0, 60);
    CHECK(dl_unpack(g.time) == held + 6 * 60, "stopped: a menu time change moves it, then it holds there (%lld)",
          (long long)(dl_unpack(g.time) - (held + 6 * 60)));
}

// A lost hold: the game's update runs twice before our code (or its own store wins the compare-and-swap) on the frame a
// real second changes. The minute it adds is kept (forward, never back); Stopped then holds again.
static void test_lost_hold(void) {
    Game g;
    DlFast fast = {0};
    DlSlow slow = {0};
    game_init(&g, 40 * 86400 + 36000, 600, "lost hold");
    for (int i = 0; i < 120; i++) frame(&g, &fast, &slow, 0, 60);
    uint64_t held = dl_unpack(g.time);
    while ((g.real_ns + (uint64_t)(1e9 / 60)) / 1000000000ull == g.real_ns / 1000000000ull)
        frame(&g, &fast, &slow, 0, 60);
    g.stamp = pack_stamp(g.real_ns, g.stamp);   // the game stored its own stamp after our hold
    for (int i = 0; i < 120; i++) frame(&g, &fast, &slow, 0, 60);
    CHECK(dl_unpack(g.time) - held == 1, "a lost hold on a turning second adds one minute, kept (%llu)",
          (unsigned long long)(dl_unpack(g.time) - held));
    CHECK(slow.slipped == 1, "and it is counted for the log (%u)", slow.slipped);
}

// A change of speed starts the modes afresh (day_length.c read_setting); leaving a slower day lets the game run again.
static void test_switch(void) {
    Game g;
    DlFast fast = {0};
    DlSlow slow = {0};
    game_init(&g, 40 * 86400 + 36000, 600, "switch");
    for (int i = 0; i < 600; i++) frame(&g, &fast, &slow, 0.5, 60);
    for (int i = 0; i < 600; i++) frame(&g, &fast, &slow, 24, 60);   // the fast mode moved the clock on
    uint64_t now = dl_unpack(g.time);
    memset(&slow, 0, sizeof slow);
    memset(&fast, 0, sizeof fast);
    frame(&g, &fast, &slow, 0.5, 60);
    CHECK(dl_unpack(g.time) >= now, "back to slow after fast: the clock isn't pulled back");
    // A slower day straight to Stopped (a fresh state, as read_setting starts one): the light stays where it is, the
    // fraction of a minute it showed included (the suite's Stopped case read it as a minute back, 2026-11-23).
    memset(&slow, 0, sizeof slow);
    for (int i = 0; i < 60 * 9 + 20; i++) frame(&g, &fast, &slow, 1.0 / 6, 60);
    float lit = g.f;
    memset(&slow, 0, sizeof slow);
    frame(&g, &fast, &slow, 0, 60);
    CHECK(g.f >= lit - 0.6f, "1/6x to Stopped: the light isn't pulled back (%g -> %g)", lit, g.f);
    float held_f = g.f;
    for (int i = 0; i < 600; i++) frame(&g, &fast, &slow, 0, 60);   // stopped, held an hour ahead
    CHECK(g.f == held_f, "and Stopped holds it there (%g -> %g)", held_f, g.f);
    uint64_t stopped = dl_unpack(g.time);
    for (int i = 0; i < 600; i++) frame(&g, &fast, &slow, 1.0, 60);   // the game's own again: one held frame at most
    CHECK(near((double)(dl_unpack(g.time) - stopped), 10, 1), "after Stopped the game's own speed runs at once (%llu)",
          (unsigned long long)(dl_unpack(g.time) - stopped));
}

int main(void) {
    test_speed();
    test_pack();
    test_stamp();
    test_runs();
    test_no_crossing();
    test_float();
    test_pause();
    test_drift();
    test_stop_after_release();
    test_stall();
    test_jumps();
    test_lost_hold();
    test_switch();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
