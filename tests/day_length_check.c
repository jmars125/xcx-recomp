// runtime/day_length.h on a model of the game's clock: the setting's parsing, the packed time, the stamp's one-second
// step back, and whole runs of the faster and slower modes at 60 and 30 frames a second, with the game's menu holding
// the clock, the game's Change Time and a save load moving it, and real midnight. No game: the clock is simulated here
// the way runtime/day_length.c describes the game's update (a game minute for every whole real second since the stamp).
// Built and run by tests/day_length_test.py.
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

// ---- the model: real time in ns from a start of day, the game's stamp and game minutes
typedef struct {
    uint64_t real_ns;   // real time of day
    uint64_t stamp;     // packed like the game's: second bits 16-21, minute 22-27, hour 28-32, plus bits we keep
    uint32_t time;      // the packed game time
    int paused;         // the game's menu: its update doesn't run
} Game;

static uint64_t pack_stamp(uint64_t sec_of_day, uint64_t keep) {
    uint64_t h = sec_of_day / 3600 % 24, m = sec_of_day / 60 % 60, s = sec_of_day % 60;
    return keep | s << 16 | m << 22 | h << 28;
}
static uint64_t stamp_seconds(uint64_t st) {
    return ((st >> 28) & 31) * 3600 + ((st >> 22) & 63) * 60 + ((st >> 16) & 63);
}
static const uint64_t KEEP = 0xABull | (7ull << 33);   // bits outside the time fields must survive

static void game_init(Game *g, uint64_t start_sec, uint64_t game_minutes) {
    g->real_ns = start_sec * 1000000000ull;
    g->stamp = pack_stamp(start_sec, KEEP);
    g->time = dl_pack(game_minutes, 0x15);
    g->paused = 0;
}

// The game's update on one frame: whole seconds since the stamp become game minutes.
static void game_update(Game *g) {
    if (g->paused) return;
    uint64_t now = g->real_ns / 1000000000ull % 86400, then = stamp_seconds(g->stamp);
    if (now + 3600 < then) now += 86400;   // the real day rolled over
    if (now > then) {
        g->time = dl_pack(dl_unpack(g->time) + (now - then), g->time);
        g->stamp = pack_stamp(now % 86400, g->stamp & ~((63ull << 16) | (63ull << 22) | (31ull << 28)));
    }
}

// One frame with our code after the game's update, as the present runs after the frame (day_length.c).
static void frame(Game *g, DlFast *fast, DlSlow *slow, double speed, double fps) {
    g->real_ns += (uint64_t)(1e9 / fps);
    game_update(g);
    if (speed > 1.0) {
        uint64_t back;
        if (dl_fast_step(fast, g->real_ns, g->stamp, speed, &back)) {
            g->stamp = back;   // our compare-and-swap: the game is not writing in the model
            dl_fast_took(fast, back);
        }
    } else if (speed != 1.0) {
        uint64_t minutes;
        double frac;
        if (dl_slow_step(slow, g->stamp, dl_unpack(g->time), speed, &minutes, &frac)) {
            g->time = dl_pack(minutes, g->time);
            CHECK(frac >= 0 && frac < 1, "the float's fraction %g is within a minute", frac);
        }
    }
}

// Game minutes a run of real seconds adds at a speed and frame rate.
static double run(double speed, double fps, double seconds, uint64_t start_sec) {
    Game g;
    DlFast fast = {0};
    DlSlow slow = {0};
    game_init(&g, start_sec, 2 * 1440 + 600);
    uint64_t t0 = dl_unpack(g.time);
    for (int i = 0; i < (int)(seconds * fps); i++) frame(&g, &fast, &slow, speed, fps);
    CHECK((g.time & 63) == 0x15, "the low bits the game keeps in the time are left alone");
    CHECK((g.stamp & KEEP) == KEEP, "the bits outside the stamp's time fields are left alone");
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
    uint64_t s = pack_stamp(10 * 3600 + 5 * 60 + 7, KEEP);
    CHECK(dl_stamp_back_1s(&s) && stamp_seconds(s) == 10 * 3600 + 5 * 60 + 6, "a second back");
    s = pack_stamp(10 * 3600 + 5 * 60, KEEP);
    CHECK(dl_stamp_back_1s(&s) && stamp_seconds(s) == 10 * 3600 + 4 * 60 + 59, "borrows from the minute");
    s = pack_stamp(10 * 3600, KEEP);
    CHECK(dl_stamp_back_1s(&s) && stamp_seconds(s) == 9 * 3600 + 59 * 60 + 59, "borrows from the hour");
    CHECK((s & KEEP) == KEEP, "the other bits stay");
    s = pack_stamp(0, KEEP);
    uint64_t before = s;
    CHECK(!dl_stamp_back_1s(&s) && s == before, "real midnight: no step back, stamp unchanged");
}

// Every step of the menu at 60 and 30 frames a second: the game minutes a run of real seconds adds, against 24 / len.
static void test_runs(void) {
    static const double lens[] = {1, 2, 3, 6, 12, 24, 48, 96, 144, 0};
    const double fps[] = {60, 30};
    for (int f = 0; f < 2; f++) {
        for (size_t i = 0; i < sizeof lens / sizeof *lens; i++) {
            double len = lens[i], speed = len ? 24.0 / len : 0, secs = len > 12 ? 600 : 120;
            double want = secs * speed, got = run(speed, fps[f], secs, 36000);
            double tol = want * 0.03 > 2 ? want * 0.03 : 2;
            if (len == 0) tol = 0;
            CHECK(near(got, want, tol), "%g-minute day at %g fps: %g game minutes in %g s, want %g", len, fps[f], got,
                  secs, want);
        }
    }
    // Across real midnight, faster and slower: one frame a day can't step back, nothing else changes.
    CHECK(near(run(24, 60, 60, 86400 - 30), 1440, 1440 * 0.03), "1-minute day across real midnight (%g)",
          run(24, 60, 60, 86400 - 30));
    CHECK(near(run(0.5, 60, 120, 86400 - 60), 60, 2), "48-minute day across real midnight (%g)",
          run(0.5, 60, 120, 86400 - 60));
    CHECK(run(0, 60, 120, 86400 - 60) == 0, "stopped across real midnight");
}

// The game's menu holds the clock: nothing owed piles up into a burst when it closes. Modelled both ways the game may
// keep its stamp meanwhile: frozen (then set to now on the way out), or moved on each second with no minutes added.
static void test_pause(void) {
    for (int moving = 0; moving < 2; moving++) {
        const double speeds[] = {24, 0.5, 0};
        for (int k = 0; k < 3; k++) {
            double sp = speeds[k];
            Game g;
            DlFast fast = {0};
            DlSlow slow = {0};
            game_init(&g, 36000, 600);
            for (int i = 0; i < 120; i++) frame(&g, &fast, &slow, sp, 60);
            g.paused = 1;
            uint64_t at_pause = dl_unpack(g.time);
            for (int i = 0; i < 600; i++) {
                frame(&g, &fast, &slow, sp, 60);
                if (moving) g.stamp = pack_stamp(g.real_ns / 1000000000ull % 86400, g.stamp & KEEP);
            }
            CHECK(dl_unpack(g.time) == at_pause, "paused (stamp %s, %gx): the clock holds", moving ? "moving" : "frozen", sp);
            g.stamp = pack_stamp(g.real_ns / 1000000000ull % 86400, g.stamp & KEEP);
            g.paused = 0;
            for (int i = 0; i < 60; i++) frame(&g, &fast, &slow, sp, 60);
            double added = (double)(dl_unpack(g.time) - at_pause);
            CHECK(added <= sp + 3, "the first second after the menu adds a second's worth at %gx, not a burst (%g)", sp,
                  added);
        }
    }
}

// A stall: no frames and no game update for 15 real seconds, then the game's tick adds 15 minutes at once. That is
// the game's own tick (it matches the stamp), so it is scaled: a stopped clock stays, half speed adds about 7.
static void test_stall(void) {
    const double speeds[] = {0, 0.5};
    for (int k = 0; k < 2; k++) {
        Game g;
        DlFast fast = {0};
        DlSlow slow = {0};
        game_init(&g, 36000, 600);
        for (int i = 0; i < 120; i++) frame(&g, &fast, &slow, speeds[k], 60);
        uint64_t before = dl_unpack(g.time);
        g.real_ns += 15ull * 1000000000ull;
        for (int i = 0; i < 2; i++) frame(&g, &fast, &slow, speeds[k], 60);
        uint64_t got = dl_unpack(g.time) - before;
        CHECK(speeds[k] == 0 ? got == 0 : got >= 7 && got <= 9, "a 15 s stall at %gx: %llu game minutes", speeds[k],
              (unsigned long long)got);
    }
}

// Change Time of a few minutes in the very frame the game ticks: not a tick (it doesn't match the stamp), followed.
static void test_change_on_tick(void) {
    const double speeds[] = {0, 0.5};
    for (int k = 0; k < 2; k++) {
        Game g;
        DlFast fast = {0};
        DlSlow slow = {0};
        game_init(&g, 36000, 600);
        for (int i = 0; i < 125; i++) frame(&g, &fast, &slow, speeds[k], 60);
        // run to the frame just before a tick, then move the time 5 minutes on and let the tick happen
        while ((g.real_ns + (uint64_t)(1e9 / 60)) / 1000000000ull == g.real_ns / 1000000000ull)
            frame(&g, &fast, &slow, speeds[k], 60);
        uint64_t before = dl_unpack(g.time);
        g.time = dl_pack(before + 5, g.time);
        for (int i = 0; i < 3; i++) frame(&g, &fast, &slow, speeds[k], 60);
        uint64_t got = dl_unpack(g.time) - before;
        CHECK(got >= 5 && got <= 6, "Change Time +5 on a tick at %gx: kept (%llu)", speeds[k], (unsigned long long)got);
        uint64_t there = dl_unpack(g.time);
        for (int i = 0; i < 120; i++) frame(&g, &fast, &slow, speeds[k], 60);
        if (speeds[k] == 0) CHECK(dl_unpack(g.time) == there, "and a stopped clock holds there");
    }
}

// The game's Change Time (or a load) moves the clock: followed, not scaled, and the scaling goes on from there.
static void test_jumps(void) {
    Game g;
    DlFast fast = {0};
    DlSlow slow = {0};
    game_init(&g, 36000, 600);
    for (int i = 0; i < 600; i++) frame(&g, &fast, &slow, 0.5, 60);
    uint64_t before = dl_unpack(g.time);
    g.time = dl_pack(before + 9 * 60, g.time);   // Change Time: 9 hours on
    for (int i = 0; i < 2; i++) frame(&g, &fast, &slow, 0.5, 60);
    CHECK(dl_unpack(g.time) >= before + 9 * 60, "a jump forward is kept");
    uint64_t after = dl_unpack(g.time);
    for (int i = 0; i < 120 * 60; i++) frame(&g, &fast, &slow, 0.5, 60);
    CHECK(near((double)(dl_unpack(g.time) - after), 60, 2), "half speed goes on after the jump (%llu)",
          (unsigned long long)(dl_unpack(g.time) - after));
    g.time = dl_pack(300, g.time);   // a save loaded: earlier
    for (int i = 0; i < 2; i++) frame(&g, &fast, &slow, 0.5, 60);
    CHECK(dl_unpack(g.time) < 310, "a jump back is followed, not undone");
    // Stopped also follows a jump: Change Time still moves a stopped clock.
    for (int i = 0; i < 60; i++) frame(&g, &fast, &slow, 0, 60);
    uint64_t held = dl_unpack(g.time);
    g.time = dl_pack(held + 6 * 60, g.time);
    for (int i = 0; i < 2 * 60; i++) frame(&g, &fast, &slow, 0, 60);
    // (the game's own tick in the frame of the jump may add its minute before the jump is seen)
    CHECK(dl_unpack(g.time) - (held + 6 * 60) <= 1, "stopped: Change Time moves it, then it holds there (%lld)",
          (long long)(dl_unpack(g.time) - (held + 6 * 60)));
    uint64_t there = dl_unpack(g.time);
    for (int i = 0; i < 2 * 60; i++) frame(&g, &fast, &slow, 0, 60);
    CHECK(dl_unpack(g.time) == there, "and holds");
}

// The game's tick read half way (its update runs on another thread): one frame sees the new time with the old stamp,
// or the new stamp with the old time; the next frame sees both. Either order is the game's tick, not someone else's
// change: a stopped clock must not creep, half speed must still add half.
static void slow_only(Game *g, DlSlow *slow, double speed) {
    uint64_t minutes;
    double frac;
    if (dl_slow_step(slow, g->stamp, dl_unpack(g->time), speed, &minutes, &frac)) g->time = dl_pack(minutes, g->time);
}
static void test_half_tick(void) {
    for (int order = 0; order < 2; order++) {
        const double speeds[] = {0, 0.5};
        for (int k = 0; k < 2; k++) {
            Game g;
            DlFast fast = {0};
            DlSlow slow = {0};
            game_init(&g, 36000, 600);
            for (int i = 0; i < 120; i++) frame(&g, &fast, &slow, speeds[k], 60);
            uint64_t held = dl_unpack(g.time);
            for (int s = 0; s < 40; s++) {
                g.real_ns += 1000000000ull;   // a real second, then the game's tick in two halves with a frame between
                uint64_t now = g.real_ns / 1000000000ull % 86400;
                uint64_t new_stamp = pack_stamp(now, g.stamp & KEEP);
                uint32_t new_time = dl_pack(dl_unpack(g.time) + 1, g.time);
                if (order) g.stamp = new_stamp;
                else g.time = new_time;
                slow_only(&g, &slow, speeds[k]);
                if (order) g.time = dl_pack(dl_unpack(g.time) + 1, g.time);
                else g.stamp = new_stamp;
                slow_only(&g, &slow, speeds[k]);
                slow_only(&g, &slow, speeds[k]);
            }
            double got = (double)(dl_unpack(g.time) - held);
            CHECK(speeds[k] == 0 ? got == 0 : near(got, 20, 2), "half-seen ticks (%s first) at %gx: %g game minutes in 40 s",
                  order ? "stamp" : "time", speeds[k], got);
        }
    }
}

// A change of speed starts the modes afresh (day_length.c read_setting): a fresh slow state takes the clock as it is.
static void test_switch(void) {
    Game g;
    DlFast fast = {0};
    DlSlow slow = {0};
    game_init(&g, 36000, 600);
    for (int i = 0; i < 600; i++) frame(&g, &fast, &slow, 0.5, 60);
    for (int i = 0; i < 600; i++) frame(&g, &fast, &slow, 24, 60);   // the fast mode moved the clock on
    uint64_t now = dl_unpack(g.time);
    memset(&slow, 0, sizeof slow);
    memset(&fast, 0, sizeof fast);
    frame(&g, &fast, &slow, 0.5, 60);
    CHECK(dl_unpack(g.time) >= now, "back to slow after fast: the clock isn't pulled back (%llu < %llu)",
          (unsigned long long)dl_unpack(g.time), (unsigned long long)now);
}

int main(void) {
    test_speed();
    test_pack();
    test_stamp();
    test_runs();
    test_pause();
    test_stall();
    test_change_on_tick();
    test_jumps();
    test_half_tick();
    test_switch();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
