// Day length: the arithmetic of runtime/day_length.c, without game memory, so tests/day_length_check.c can run it on
// made-up clock readings. The game side reads the clock object, calls these, and writes back what they say.
//
// The game's clock (2.0.0) keeps the game time packed in a u32: minute in bits 6-11, hour in bits 12-16, a day count
// from bit 17 (bits 0-5 are left as the game has them). Its real-time stamp, a u64, packs the second in bits 16-21,
// the minute in 22-27 and the hour in 28-32. Each frame where whole real seconds have passed since the stamp, the
// game adds that many game minutes and stores a new stamp: a 24-minute day, at a rate fixed in its code.
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
// the light change in jerks. One extra tick a frame at most, and only once the game took the last one: the clock
// stands still in the game's menus, and owing there must not pile up into a burst after. So the top speed is the frame
// rate plus one game minute a real second; below what the setting asks, the clock runs slower than set, never in jumps.
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

// Whole real seconds from one stamp to another (the real day wraps at midnight).
static inline uint64_t dl_stamp_gap(uint64_t from, uint64_t to) {
    uint64_t a = ((from >> 28) & 31) * 3600 + ((from >> 22) & 63) * 60 + ((from >> 16) & 63);
    uint64_t b = ((to >> 28) & 31) * 3600 + ((to >> 22) & 63) * 60 + ((to >> 16) & 63);
    return b >= a ? b - a : b + 86400 - a;
}

// Slower than the game (and stopped): after each of the game's ticks, the minutes it added are replaced by that many
// times the speed, the fractions carried to the next tick. A tick of the game adds exactly one minute for each real
// second its stamp moved on, so a change of the time that matches the stamp's move is the game's tick, and any other
// change is something else moving the clock (the game's Change Time, a save loading): followed, not scaled, whatever its
// size. A stall therefore stays a tick (a stopped clock doesn't jump after it), and a Change Time that lands on a tick
// is still followed. The game's update runs on another thread, so a frame can see the new time before the new stamp,
// or the stamp before the time: a mismatch is followed only when the next frame still shows it.
typedef struct {
    int have;              // 0 before the first reading
    int pending;           // 1: last frame showed a mismatch (time or stamp moved alone); judged on this frame
    uint64_t last_stamp;   // the stamp at the last tick taken
    uint64_t ours;         // the game time last written (or followed), in minutes
    double carry;          // a fraction of a minute not written yet
} DlSlow;

// 1 with *minutes and *frac (the fraction of a minute the smooth float shows) to write into the clock. Stopped
// (speed 0) also writes between ticks, holding the float the game moves on its own.
static inline int dl_slow_step(DlSlow *st, uint64_t stamp, uint64_t now, double speed, uint64_t *minutes,
                               double *frac) {
    if (!st->have) {
        st->have = 1, st->pending = 0, st->last_stamp = stamp, st->ours = now, st->carry = 0;
        return 0;
    }
    uint64_t gap = dl_stamp_gap(st->last_stamp, stamp);
    if (now == st->ours) {   // the time is ours
        if (gap && !st->pending) {   // the stamp moved alone: the tick's time may follow on the next frame
            st->pending = 1;
        } else {   // no tick, or a stamp move without minutes (the game's menu holds the clock): take the stamp
            st->pending = 0, st->last_stamp = stamp;
        }
        if (speed != 0) return 0;
        *minutes = st->ours, *frac = 0;
        return 1;
    }
    if (gap && now > st->ours && now - st->ours == gap) {   // the game's tick: scale it
        st->pending = 0, st->last_stamp = stamp;
        double add = (double)gap * speed + st->carry;
        uint64_t whole = (uint64_t)floor(add);
        st->carry = add - (double)whole;
        st->ours += whole;
        *minutes = st->ours, *frac = speed == 0 ? 0 : st->carry;
        return 1;
    }
    if (!st->pending) {   // a mismatch: maybe half a tick; look again next frame, writing nothing meanwhile
        st->pending = 1;
        return 0;
    }
    st->pending = 0, st->last_stamp = stamp, st->ours = now, st->carry = 0;   // something else moved it: follow
    return 0;
}
