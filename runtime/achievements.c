// Recomp Achievements: unlocks reported to RecompLauncher, which shows the toast, keeps them and decides what counts
// as Ranked. Our own client of the channel its spec describes (github.com/elripalda/RecompAchievements, docs/SPEC.md,
// version 1; its license lets any game use the format and channel); none of its library's code is here.
//
// RecompLauncher starts the game with RL_SESSION naming a local channel (a named pipe on Windows, a Unix socket
// elsewhere; platform.h plat_ipc_*). Without it this file does nothing. Both sides send one JSON object per line.
//
// The list is an "events" list: the launcher maps each event name to an achievement, and an event no list names does
// nothing. The events are the game's own achievements: ach_<id> when the game awards its achievement <id>. The
// launcher unlocks an achievement once. A thread looks at the game's own data twice a second; nothing runs on the
// game's threads. What it reads, all from the game data object whose pointer is at main + GAME_DATA (runtime/cheats.c,
// the port's Max Funds, reads the same object):
//   achievements ACH_IDS records ACH_STRIDE apart from +ACH_RECORDS, one per row of the game's own list (below)
//   characters   CHARACTERS slots CHARACTER_STRIDE apart from the object, the level a u16 at +LEVEL in each
//   story        u16 at +STORY, the scenario number (below)
// The levels and the scenario number only tell a save load from play.
//
// The game's achievements are the rows of its table SCL_AchiveList (844 rows: IDs 1..843, then ACHIVE_END, an empty
// row), and <id> is the row's ID. Each row names a statistic and a count; the game awards the achievement when that
// statistic is counted up to the count and keeps the result in a 16-byte record per ID: main+0x4fa03c, which the
// achievements menu asks, reads the record at [main+0x4e4054's block] + 0x17AB0 + id * 16, that block being the game
// data object + 0x79A78, so the record is at +ACH_RECORDS + id * 16. Its first word holds flags: ACH_EARNED (bit 1) is
// set when the achievement is awarded, bit 2 once the menu has shown it, bit 0 with bit 1; the third word is the time
// it was awarded (seconds since 1970). The flags are saved with the rest of the object.
//
// The scenario number is what the game's scripts read with getScenario (its native at main+0x3ed320 asks the flag
// store at main+0xc96d0 for 16-bit value 1). The flag store lives inside the game data object at +0x72E78 and keeps its
// 16-bit values from +0x5800 in it, so value 1 is at +STORY. It counts chapter * 1000 + step: the prologue runs 1..8,
// chapter 1 starts at 1001, chapter 12 at 12001, and the Definitive Edition's chapter 13 runs 13002..15026, 15027 being
// the end. The value only grows in play, by one step or from one chapter's last step to the next one's first.
//
// A load must not fire anything: the object holds defaults for the first moments after boot, then the save the game
// reads at start, and loading another save rewrites it in place (the pointer stays). A load is when the
// object first appears or changes to another one, when LOAD_LEVELS or more characters' levels change in one look, when
// any level or the scenario goes down, when the scenario skips a chapter, when an earned achievement is no longer
// earned, or when more than LOAD_ACH achievements are earned in one look. An achievement arms only once it was seen not
// earned with the values settled SETTLE_MS since the last load, and fires when it is then earned: what a save already
// has never fires (as the spec's memory rules wait for a false frame). A load none of these catch (another save at
// nearly the same story point and levels, with at most LOAD_ACH more achievements) is not seen; its extra achievements
// could then fire.
//
// Also sent: hello on connect; save_loaded at each load after the object first appears (the save read at start is one);
// casual "cheats" while a cheat is switched on, so its unlocks are kept as Casual. From the launcher, peek (its dev
// tools' memory viewer) is answered from guest memory; list and quiet need nothing from an events list.
//
// XCX_ACH_TRACE=1 logs each value read when it changes, to check the offsets on another build.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "achievements.h"
#include "cheats.h"
#include "host.h"
#include "platform/platform.h"

#define PERIOD_MS 500
#define RECONNECT_MS 5000
#define GAME_DATA 0x1DC1408u   // main + this: the game data object's pointer, 2.0.0 (build EC4ED46BBC4A9EBB)
#define CHARACTERS 23
#define CHARACTER_STRIDE 0x7F8u
#define LEVEL 0x172u
#define STORY 0x7867Au          // the scenario number: the flag store (+0x72E78) 16-bit value 1 (+0x5800 + 2)
#define STORY_MAX 15027         // the last scenario number; one read above it means the offsets do not fit this build
#define LEVEL_MAX 99            // a level read above it is not a level: the offsets do not fit this build
#define LOAD_LEVELS 3           // characters whose level changes in one look: a save loaded, not play
#define ACH_RECORDS 0x91528u    // the game's achievement records: block +0x79A78, records +0x17AB0 in it
#define ACH_STRIDE 16u
#define ACH_IDS 843             // SCL_AchiveList's achievements, IDs 1..843; record 0 is unused
#define ACH_EARNED 2u           // record flags: awarded
#define ACH_FLAGS 7u            // every flag the game sets; other bits mean the offsets do not fit this build
#define LOAD_ACH 8              // achievements earned in one look: more is a save loaded, not play
#define SETTLE_MS 10000

// The game's achievements, by ID: earned in the last look, seen not earned since the last load (settled), sent.
static uint8_t ach_earned[ACH_IDS + 1], ach_armed[ACH_IDS + 1], ach_fired[ACH_IDS + 1];

static PlatIpc *channel;
static char session[512];
static uint64_t frame;   // the spec's frame: the thread's ticks, only for ordering
static int trace;

static void send_line(const char *line) {
    if (!channel) return;
    size_t n = strlen(line);
    char buf[1100];
    if (n + 2 > sizeof buf) return;
    memcpy(buf, line, n);
    buf[n++] = '\n';
    if (!plat_ipc_write(channel, buf, n)) {
        rt_log("achievements: the launcher closed the channel");
        plat_ipc_close(channel);
        channel = NULL;
    }
}

// JSON string body: the names here are plain ASCII; anything else is dropped rather than escaped wrong.
static void json_text(char *out, size_t cap, const char *s) {
    size_t k = 0;
    for (; *s && k + 2 < cap; s++)
        if ((unsigned char)*s >= 0x20 && *s != '"' && *s != '\\' && (unsigned char)*s < 0x7f) out[k++] = *s;
    out[k] = 0;
}

static void say_hello(void) {
    char version[64], line[256];
    json_text(version, sizeof version, rt_port_version);
    snprintf(line, sizeof line,
             "{\"t\":\"hello\",\"sdk\":\"xcx-recomp-own-1\",\"console\":\"switch\",\"memory\":0,"
             "\"game\":\"xcx-recomp\",\"version\":\"%s\"}", version);
    send_line(line);
}

static void send_achievement(int id) {
    char line[160];
    snprintf(line, sizeof line, "{\"t\":\"event\",\"name\":\"ach_%d\",\"frame\":%" PRIu64 "}", id, frame);
    send_line(line);
    rt_log("achievements: event ach_%d", id);
}

// Where the value of a line's top-level field "key" starts (spaces around the colon allowed), NULL without it. The
// launcher's lines are flat objects for the messages read here, so a plain search for the quoted key is enough.
static const char *field(const char *line, const char *key) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    for (const char *p = strstr(line, pat); p; p = strstr(p + 1, pat)) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '	') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ' || *q == '	') q++;
        return q;
    }
    return NULL;
}

// A number field, -1 when it is not there.
static int64_t field_num(const char *line, const char *key) {
    const char *p = field(line, key);
    return p ? strtoll(p, NULL, 0) : -1;
}

// The message type "t" is this name.
static int is_type(const char *line, const char *name) {
    const char *p = field(line, "t");
    size_t n = strlen(name);
    return p && *p == '"' && !strncmp(p + 1, name, n) && p[1 + n] == '"';
}

// peek: up to 256 bytes of guest memory at address, as hex; unreadable memory answers an empty hex.
static void answer_peek(const char *line) {
    int64_t address = field_num(line, "address"), size = field_num(line, "size"), seq = field_num(line, "seq");
    if (address < 0 || size <= 0) return;
    if (size > 256) size = 256;
    uint8_t mem[256];
    char hex[520] = "", out[700];
    if ((uint64_t)address < RT_ADDRESS_LIMIT && plat_mem_read((uint64_t)address, mem, (size_t)size))
        for (int i = 0; i < size; i++) snprintf(hex + 2 * i, 3, "%02x", mem[i]);
    snprintf(out, sizeof out, "{\"t\":\"mem\",\"seq\":%" PRId64 ",\"hex\":\"%s\"}", seq < 0 ? 0 : seq, hex);
    send_line(out);
}

static void read_channel(void) {
    static char buf[16384];
    static size_t have;
    for (;;) {
        if (!channel) return;
        int64_t got = plat_ipc_read(channel, buf + have, sizeof buf - 1 - have);
        if (got < 0) {
            rt_log("achievements: the launcher closed the channel");
            plat_ipc_close(channel);
            channel = NULL;
            have = 0;
            return;
        }
        if (got == 0) return;
        have += (size_t)got;
        buf[have] = 0;
        char *start = buf, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = 0;
            if (is_type(start, "peek")) answer_peek(start);
            else if (is_type(start, "list")) rt_log("achievements: the launcher sent its list");
            start = nl + 1;
        }
        have = (size_t)(buf + have - start);
        memmove(buf, start, have);
        if (have == sizeof buf - 1) have = 0;   // a line longer than the buffer: dropped
    }
}

static uint16_t last_levels[CHARACTERS], last_story;

// One look at the game: fills ach_earned, and sets *loaded when the achievements, the levels or the scenario changed
// as only a load changes them; 0 when the data does not fit.
static int look(uint64_t data, int *loaded) {
    uint16_t story = 0;
    if (!plat_mem_read(data + STORY, &story, sizeof story) || story > STORY_MAX) return 0;
    int top = 0, changed = 0, down = 0;
    uint16_t now[CHARACTERS];
    for (int i = 0; i < CHARACTERS; i++) {
        uint16_t level = 0;
        if (!plat_mem_read(data + i * (uint64_t)CHARACTER_STRIDE + LEVEL, &level, sizeof level)) return 0;
        if (level > LEVEL_MAX) return 0;
        if (level > top) top = level;
        changed += level != last_levels[i];
        down |= level < last_levels[i];
        now[i] = level;
    }
    // Play moves the scenario one step on, or from a chapter's last step to the next chapter's first: anything else
    // (back, or past a whole chapter) is another save.
    int story_jump = story < last_story || story / 1000 > last_story / 1000 + 1;
    // The game's achievements: the flags word of each record. Play only ever adds a few at once.
    static uint32_t records[(ACH_IDS + 1) * ACH_STRIDE / 4];
    if (!plat_mem_read(data + ACH_RECORDS, records, sizeof records)) return 0;
    int lost = 0, gained = 0, earned_count = 0;
    for (int id = 1; id <= ACH_IDS; id++) {
        uint32_t flags = records[id * ACH_STRIDE / 4];
        if (flags & ~ACH_FLAGS) return 0;
        uint8_t earned = (flags & ACH_EARNED) != 0;
        lost |= ach_earned[id] && !earned;
        gained += earned && !ach_earned[id];
        earned_count += earned;
        ach_earned[id] = earned;
    }
    *loaded = changed >= LOAD_LEVELS || down || story_jump || lost || gained > LOAD_ACH;
    memcpy(last_levels, now, sizeof now);
    last_story = story;
    static int64_t last_top = -1, last_traced_story = -1, last_earned = -1;
    if (trace && (top != last_top || story != last_traced_story || earned_count != last_earned)) {
        rt_log("achievements: top level %d, scenario %u, game achievements earned %d", top, story, earned_count);
        last_top = top, last_traced_story = story, last_earned = earned_count;
    }
    return 1;
}

static void achievements_thread(void *arg) {
    (void)arg;
    uint64_t main_base = ldr_module(0)->base, last_data = 0, next_connect = 0, settled_from = 0;
    int casual_sent = 0, bad_logged = 0, loads = 0;
    for (;;) {
        plat_sleep_ms(PERIOD_MS);
        frame++;
        uint64_t now = plat_now_ms();
        if (!channel && now >= next_connect) {
            next_connect = now + RECONNECT_MS;
            if ((channel = plat_ipc_connect(session))) {
                rt_log("achievements: connected to RecompLauncher");
                say_hello();
                casual_sent = 0;
                memset(ach_fired, 0, sizeof ach_fired);
            }
        }
        read_channel();
        if (!channel) continue;
        if (!casual_sent && cheats_active()) {
            send_line("{\"t\":\"casual\",\"reason\":\"cheats\"}");
            casual_sent = 1;
        }
        uint64_t data = 0;
        if (!plat_mem_read(main_base + GAME_DATA, &data, sizeof data) || !data || data >= RT_ADDRESS_LIMIT) {
            last_data = 0;
            continue;
        }
        int loaded = 0;
        if (!look(data, &loaded)) {
            if (!bad_logged) rt_log("achievements: the game data does not fit this build's offsets; no events");
            bad_logged = 1;
            continue;
        }
        if (data != last_data || loaded) {   // a save loaded: every achievement waits to be seen unearned, settled
            last_data = data;
            settled_from = now + SETTLE_MS;
            memset(ach_armed, 0, sizeof ach_armed);
            if (loads++) {   // the first is the boot's defaults, or the save read at start
                char line[96];
                snprintf(line, sizeof line, "{\"t\":\"save_loaded\",\"frame\":%" PRIu64 "}", frame);
                send_line(line);
            }
            if (trace) rt_log("achievements: load %d", loads);
            continue;
        }
        int settled = now >= settled_from;
        for (int id = 1; id <= ACH_IDS; id++) {
            if (!ach_earned[id]) ach_armed[id] |= settled;
            else if (ach_armed[id] && !ach_fired[id]) {
                ach_fired[id] = 1;
                send_achievement(id);
            }
        }
    }
}

void achievements_start(void) {
    if (!plat_getenv("RL_SESSION", session, sizeof session) || !*session) return;
    char t[8];
    trace = plat_getenv("XCX_ACH_TRACE", t, sizeof t) && t[0] == '1';
    rt_log("achievements: RecompLauncher session channel given");
    if (!plat_thread_spawn(achievements_thread, NULL, "achievements", 128 << 10))
        rt_log("achievements: no thread, no achievements this session");
}
