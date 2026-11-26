// Day length: the arithmetic of runtime/day_length.c, without game memory, so tests/day_length_check.c can run it on
// made-up clock readings. The game side reads the clock object, calls these, and writes back what they say.
//
// The game's clock (2.0.0) keeps the game time packed in a u32: minute in bits 6-11, hour in bits 12-16, a day count
// from bit 17 (bits 0-5 are left as the game has them). Its real-time stamp, a u64, is the system's calendar time:
// milliseconds in bits 6-15, the second in 16-21, the minute in 22-27, the hour in 28-32, then the day of the month,
// the day of the year (38-46), the month and the year. Every frame its update reads the stamp, stores the time now in
// its place, and counts whole seconds from the old stamp to now (second, minute, hour and day of the year, signed).
// One or more: it adds that many game minutes and sets the float to the new minute. Less (the usual frame, inside a
// second): it moves the float on by the milliseconds between them. So the clock gains a game minute each time the real
// second changes between two frames: a 24-minute day, at a rate fixed in its code.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

// The clock speed for an XCX_DAY_LENGTH value, in real minutes a game day lasts: 24 is the game's own (1.0), 12 twice
// as fast, 48 half as fast, 0 stopped. NULL, "" or text that is not a number are the game's own; so are negative
// values and NaN. Above 1440 (a game day a real day) counts as 1440, and under 1 as 1: the faster mode adds at most
// one minute a frame (dl_fast_step), so a 1-minute day already needs 24 frames a second.
static inline double dl_speed(const char *value) {
    if (!value || !*value) return 1.0;
    char *end;
    double len = strtod(value, &end);
    if (end == value || *end || !(len >= 0)) return 1.0;   // not a number as a whole, negative or NaN: the game's own
    if (len == 0) return 0.0;
    if (len > 1440) len = 1440;
    if (len < 1) len = 1;
    return 24.0 / len;
}

// Packed game time <-> minutes since day 0. low: bits 0-5, kept as the game left them.
static inline uint64_t dl_unpack(uint32_t t) {
    return (uint64_t)(t >> 17) * 1440 + ((t >> 12) & 31) * 60 + ((t >> 6) & 63);
}
static inline uint32_t dl_pack(uint64_t minutes, uint32_t low) {
    uint32_t day = (uint32_t)(minutes / 1440), m = (uint32_t)(minutes % 1440);
    return (day & 0x7FFF) << 17 | (m / 60) << 12 | (m % 60) << 6 | (low & 63);
}

// The stamp one real second earlier, borrowing from the minute and the hour. 0 (stamp unchanged) when that would reach
// into the day: the caller skips that frame, at real midnight.
static inline int dl_stamp_back_1s(uint64_t *s) {
    uint64_t sec = (*s >> 16) & 63, min = (*s >> 22) & 63, hr = (*s >> 28) & 31;
    if (sec > 0) sec--;
    else if (min > 0) sec = 59, min--;
    else if (hr > 0) sec = 59, min = 59, hr--;
    else return 0;
    *s = (*s & ~((63ull << 16) | (63ull << 22) | (31ull << 28))) | sec << 16 | min << 22 | hr << 28;
    return 1;
}

// Faster than the game: extra ticks. Each frame owes (speed - 1) * frame time game minutes on top of the game's own
// minute a second. When a whole one is owed, the stamp goes back a second, so the game's next update sees a second gone
// and adds one minute its own way (carries, time period, the smooth float): the clock counts every minute, only
// faster, and the light moves smoothly. Multiplying each tick's minutes instead made the clock jump 24 at a time and
// the light change in jerks. One extra tick a frame at most, and only once the game took the last one: while the
// clock stands still (the title screen, a load) owing must not pile up into a burst after. So the top speed is the
// frame rate plus one game minute a real second; below what the setting asks, the clock runs slower than set, never in jumps.
typedef struct {
    uint64_t last_ns;   // the frame time of the last call, 0 before the first
    uint64_t wrote;     // the stamp the last extra tick wrote, 0 for none
    double owed;        // game minutes owed, at most 2
} DlFast;

// ns: a monotonic clock now; stamp: the clock's stamp now. 1 with *back = the stamp to compare-and-swap in; on success
// the caller calls dl_fast_took.
static inline int dl_fast_step(DlFast *st, uint64_t ns, uint64_t stamp, double speed, uint64_t *back) {
    double dt = st->last_ns ? (double)(ns - st->last_ns) / 1e9 : 0;
    st->last_ns = ns;
    if (dt > 0.1) dt = 0.1;   // a stall: no burst after it
    if (st->wrote && stamp == st->wrote) return 0;   // the game has not taken the last one yet
    st->owed += (speed - 1.0) * dt;
    if (st->owed < 1.0) return 0;
    if (st->owed > 2.0) st->owed = 2.0;
    uint64_t n = stamp;
    if (!dl_stamp_back_1s(&n)) return 0;
    *back = n;
    return 1;
}
static inline void dl_fast_took(DlFast *st, uint64_t back) {
    st->wrote = back;
    st->owed -= 1.0;
}

// The stamp held: hour 31, minute 59, second 59, with its milliseconds and day fields kept. The game counts the seconds
// between stamps field by field, so a held stamp reads as ahead of the time now until 08:00 of the next real day at
// the earliest (31:59:59 today is 07:59:59 tomorrow): the game's next update counts no whole second, however long the
// clock stood still meanwhile (a stall, a load, the title screen), and adds no minute. No day field is touched.
static inline uint64_t dl_stamp_held(uint64_t s) {
    return (s & ~((63ull << 16) | (63ull << 22) | (31ull << 28))) | 59ull << 16 | 59ull << 22 | 31ull << 28;
}

// Slower than the game (and stopped): the game's minutes are held back, never taken back. Every frame the game stores
// the time now as its stamp; after it, while no minute is owed, the stamp is held (dl_stamp_held, a compare-and-swap
// as the faster mode does), so the game's next update counts no whole second and adds nothing. Game minutes owed grow
// by speed * the real time between two updates of the game (its stamp moved since the last call; not while its clock
// stands still, nor while the update's own hold flag, the clock's +0x8 bit 0, is set: game_holds). Once one is owed
// the stamp goes a second back instead, as the faster mode does, and the game's next update adds the minute its own
// way, with everything that follows from it; a second back the game hasn't used within 0.1 s (its clock stopped for a
// load or the title screen) is turned into a hold, so a long stop isn't counted. The time is never written, so it never
// steps back, and an hour or a day only changes when the game itself moves on into it. A minute the game adds is taken
// off what is owed; a change it didn't make the usual way (a time change from the game's menu, a save loading:
// backwards, or more than 2 minutes) is followed and counted for the log, and nothing owed is carried past it. The
// float the game moves between minutes runs at the game's own speed, so the caller sets it from the time and *frac,
// the part of the next minute owed so far (0 to 1): the light moves smoothly at the slower speed and holds still at
// Stopped.
typedef struct {
    int have;              // 0 before the first reading
    int released;          // the last write was a second back (a minute let through), not a hold
    uint64_t ran_ns;       // the frame time of the last call that saw the game's update run (owed grows from there)
    uint64_t wrote_ns;     // the frame time of the last write
    uint64_t last_min;     // the game time at the last call, in minutes
    uint64_t last_stamp;   // the stamp at the last call
    uint64_t wrote;        // the stamp the last write put there, 0 for none
    double owed;           // game minutes owed, -1 to 1.5
    unsigned slipped;      // game minutes added while held (a lost hold): counted for the log
    unsigned followed;     // changes of the time followed (a time change, a load, a stall past the hold): for the log
} DlSlow;

// ns: a monotonic clock now; stamp and minutes: the clock now; game_holds: the update's own hold flag; shown: the part
// of the next minute the float shows now (read only on the first call, so a change of speed takes the light over where
// it is instead of pulling it back to the minute's start). Sets *frac always; 1 with *write = the stamp to
// compare-and-swap in, and on success the caller calls dl_slow_took.
static inline int dl_slow_step(DlSlow *st, uint64_t ns, uint64_t stamp, uint64_t minutes, int game_holds, double shown,
                               double speed, uint64_t *write, double *frac) {
    if (!st->have) {
        st->have = 1, st->released = 0, st->ran_ns = ns, st->wrote_ns = 0, st->last_min = minutes, st->wrote = 0;
        st->owed = shown > 0 && shown < 1 ? shown : 0;
    } else {
        if (minutes != st->last_min) {
            if (minutes > st->last_min && minutes - st->last_min <= 2) {   // the game's minute
                if (st->owed < 1) st->slipped += (unsigned)(minutes - st->last_min);   // none was let through
                st->owed -= (double)(minutes - st->last_min);
                // one that slipped past a hold is kept, never taken back; the next minute comes that much later
                if (st->owed < -1) st->owed = -1;
            } else {
                st->owed = 0, st->followed++;   // a time change from the game's menu, a load: followed
            }
            st->last_min = minutes;
        }
        if (stamp != st->wrote && stamp != st->last_stamp) {   // the game's update ran since the last call
            double dt = (double)(ns - st->ran_ns) / 1e9;
            if (dt > 0.25) dt = 0.25;   // after a stall or a stop: no burst
            if (!game_holds) st->owed += speed * dt;
            st->ran_ns = ns;
        }
        if (st->owed > 1.5) st->owed = 1.5;
    }
    st->last_stamp = stamp;
    *frac = st->owed < 0 ? 0 : st->owed > 1 ? 1 : st->owed;
    if (stamp == st->wrote) {   // our last write is still there: the game's update hasn't run since
        if (!st->released || ns - st->wrote_ns < 100000000ull) return 0;
        *write = dl_stamp_held(stamp);   // a second back left unused: its clock stopped; don't let the stop count
        return 1;
    }
    uint64_t w = stamp;
    if (speed > 0 && st->owed >= 1) {
        // A minute is due: the stamp a second back, so the game's next update counts one second and adds the minute
        // at once (the light then lines up with it). Not in the last tenth of a real second, where that update could
        // count the next second too (at 10 frames a second and more); nor at real midnight (dl_stamp_back_1s). A
        // frame later instead.
        if (((stamp >> 6) & 1023) >= 900 || !dl_stamp_back_1s(&w)) return 0;
    } else {
        w = dl_stamp_held(stamp);
    }
    *write = w;
    return 1;
}
static inline void dl_slow_took(DlSlow *st, uint64_t ns, uint64_t write) {
    st->released = ((write >> 28) & 31) != 31;   // a held stamp has hour 31; a second back keeps the hour of now
    st->wrote = write, st->wrote_ns = ns;
}
