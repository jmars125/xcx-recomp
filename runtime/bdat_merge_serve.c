// The game side of the BDAT merge (bdat_merge.h, bdat_merge_serve): the game's own copy of a file read from its
// archive, the merged file kept in <profile>/bdat-merge/, and status lines for the F1 Mods tab.
//
// The archive: sts.arh holds 'arh2', the entry count, the alignment and a 0, then per entry the XXH64 (seed 0) of
// its path ("/bdat/common.bdat"), its stored size and its unpacked size; the entries' data follows each other in
// sts.ard, each starting at a multiple of the alignment. A stored entry that starts with 'xbc1' is packed: type (3 =
// zstd), unpacked size, packed size, 32 more bytes, then the data.
//
// The merged file is named after a SHA-256 of the game's copy and the mods' copies in order, so a set of inputs is
// merged once and a change to any of them (a mod update, another order) makes a new file. Older merges of the same
// game file are deleted. Next to the file, <file>.txt lists the mods, one line per event (bdat_merge.h) and, last,
// the summary, which a later start shows again when it reuses the file. <key with / as _>.quick lets a start whose
// inputs have the same sizes and times find that file without reading them ("the quick check", below).
//
// bdat_merge_prepare makes the merge of a list changed while the game runs, ahead of the next start (the F1 Mods
// tab's Merge now): the same file, report and quick note, so that start finds it, but its status lines and tags go
// to a report of their own, and the files this run serves are kept.
#include "bdat_merge.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include <zstd.h>

#include "fs.h"
#include "host.h"
#include "romfs_mods.h"
#include "sha256.h"
#include "platform/platform.h"

#define VERSION "bdat-merge 6"   // part of the cache name: a change to the merge rules makes new files
#define LOG_NOTES 20             // event lines in the log per merge; the report file has all of them
#define STATUS_CAP 48
#define STATUS_EVENTS 8          // left-out and caveat lines per merge in the Mods tab; the report has all

static PlatMutex lock = PLAT_MUTEX_INIT;     // one merge at a time; guards the archive index and target
static uint8_t *arh;
static uint64_t arh_size;
static int arh_state;                        // 0 not read, 1 read, -1 missing

// ---- XXH64, seed 0 (the archive's path hash), after the xxHash specification
#define P1 0x9E3779B185EBCA87ull
#define P2 0xC2B2AE3D27D4EB4Full
#define P3 0x165667B19E3779F9ull
#define P4 0x85EBCA77C2B2AE63ull
#define P5 0x27D4EB2F165667C5ull
static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t round64(uint64_t acc, uint64_t in) { return rotl(acc + in * P2, 31) * P1; }
static uint64_t merge64(uint64_t h, uint64_t v) { return (h ^ round64(0, v)) * P1 + P4; }

static uint64_t xxh64(const uint8_t *d, size_t n) {
    size_t i = 0;
    uint64_t h;
    if (n >= 32) {
        uint64_t v[4] = {P1 + P2, P2, 0, (uint64_t)0 - P1};
        for (; i + 32 <= n; i += 32)
            for (int k = 0; k < 4; k++) v[k] = round64(v[k], rd64(d + i + 8 * k));
        h = rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18);
        for (int k = 0; k < 4; k++) h = merge64(h, v[k]);
    } else {
        h = P5;
    }
    h += n;
    for (; i + 8 <= n; i += 8) h = rotl(h ^ round64(0, rd64(d + i)), 27) * P1 + P4;
    if (i + 4 <= n) {
        h = rotl(h ^ (uint64_t)rd32(d + i) * P1, 23) * P2 + P3;
        i += 4;
    }
    for (; i < n; i++) h = rotl(h ^ d[i] * P5, 11) * P1;
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    return h ^ (h >> 32);
}

// ---- the game's copy

// n bytes at off of RomFS file name (all of it for n == UINT64_MAX) into a new buffer; NULL on failure.
static uint8_t *rom_read(const char *mount, const char *name, uint64_t off, uint64_t n, uint64_t *got) {
    char path[512];
    uint64_t h;
    snprintf(path, sizeof path, "%s:/%s", mount, name);
    if (romfs_open(&h, path, FS_OPEN_READ)) return NULL;
    int64_t size = 0;
    uint8_t *p = NULL;
    if (!romfs_size(&size, h) && size >= 0 && off <= (uint64_t)size) {
        if (n == UINT64_MAX || n > (uint64_t)size - off) n = (uint64_t)size - off;
        uint64_t done = 0;
        if (n < (1ull << 31) && (p = malloc(n ? n : 1)) && romfs_read(&done, h, (int64_t)off, p, n) == 0 && done == n) {
            *got = n;
        } else {
            free(p);
            p = NULL;
        }
    }
    romfs_close(h);
    return p;
}

// The game's copy as it is stored in sts.ard (packed or not) into a new buffer; NULL with the reason in why.
static uint8_t *game_stored(const char *mount, const char *key, size_t *n, char *why, size_t cap) {
    if (!arh_state) {
        arh = rom_read(mount, "sts.arh", 0, UINT64_MAX, &arh_size);
        arh_state = arh && arh_size >= 16 && !memcmp(arh, "arh2", 4) && 16 + 16ull * rd32(arh + 4) <= arh_size &&
                    rd32(arh + 8) && !(rd32(arh + 8) & (rd32(arh + 8) - 1)) ? 1 : -1;
    }
    if (arh_state < 0) {
        snprintf(why, cap, "the game's archive index sts.arh can't be read");
        return NULL;
    }
    char path[1100];
    int len = snprintf(path, sizeof path, "/%s", key);
    uint64_t want = xxh64((const uint8_t *)path, (size_t)len), off = 0, align = rd32(arh + 8);
    uint32_t count = rd32(arh + 4);
    for (uint32_t k = 0; k < count; k++) {
        const uint8_t *e = arh + 16 + 16ull * k;
        uint32_t stored = rd32(e + 8);
        if (rd64(e) != want) {
            off += (stored + align - 1) & ~(align - 1);
            continue;
        }
        uint64_t got = 0;
        uint8_t *p = rom_read(mount, "sts.ard", off, stored, &got);
        if (!p || got != stored) {
            free(p);
            snprintf(why, cap, "the game's %s can't be read from sts.ard", key);
            return NULL;
        }
        *n = got;
        return p;
    }
    snprintf(why, cap, "the game has no %s", key);
    return NULL;
}

// The game's stored copy p (game_stored, freed here) unpacked into a new buffer; NULL with the reason in why.
static uint8_t *game_unpack(uint8_t *p, size_t got, const char *key, size_t *n, char *why, size_t cap) {
    if (got < 48 || memcmp(p, "xbc1", 4)) {
        *n = got;
        return p;
    }
    uint32_t type = rd32(p + 4), un = rd32(p + 8), packed = rd32(p + 12);
    uint8_t *out = type == 3 && packed <= got - 48 && un < (1u << 30) ? malloc(un ? un : 1) : NULL;
    size_t r = out ? ZSTD_decompress(out, un, p + 48, packed) : 0;
    free(p);
    if (!out || ZSTD_isError(r) || r != un) {
        free(out);
        snprintf(why, cap, "the game's %s can't be unpacked (type %u)", key, type);
        return NULL;
    }
    *n = un;
    return out;
}

// ---- status lines and events

// What each merge said, for the F1 Mods tab: its status lines and, per mod and file, its changes left out, its
// caveats, whether it adds nothing and how many of its tables are alike (the tag on the mod's row). Report 0 is of
// the merges this run serves, report 1 of those made ahead for the next start. Guarded by text_lock, held only to
// copy lines in or out, so the menu never waits for a merge.
typedef struct { char id[65], key[64]; int left, caveats, nothing, alike, changed; } ModTag;
#define TAG_CAP 128
typedef struct {
    char status[STATUS_CAP][320];
    int kind[STATUS_CAP], nstatus;
    ModTag tags[TAG_CAP];
    int ntags;
    char files[8][2500];   // the report files of its merges, for bdat_merge_mod_lines
    int nfiles;
} Report;
static Report reports[2];
static PlatMutex text_lock = PLAT_MUTEX_INIT;
static int target;   // the report of the merge in progress; under lock

// kind 0: a summary line; 1: a change left out; 2: a caveat; 3: a mod that adds nothing; 4: one partly alike. The oldest lines go first when the list is full.
static void add_status(int kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add_status(int kind, const char *fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    plat_mutex_lock(&text_lock);
    Report *r = &reports[target];
    if (r->nstatus == STATUS_CAP) {
        memmove(r->status, r->status + 1, sizeof r->status[0] * (STATUS_CAP - 1));
        memmove(r->kind, r->kind + 1, sizeof r->kind[0] * (STATUS_CAP - 1));
        r->nstatus--;
    }
    r->kind[r->nstatus] = kind;
    memcpy(r->status[r->nstatus++], line, sizeof line);
    plat_mutex_unlock(&text_lock);
}

static int report_status(const Report *r, int i, char *out, size_t cap, int *kind) {
    plat_mutex_lock_shared(&text_lock);
    int ok = i >= 0 && i < r->nstatus;
    if (ok) {
        snprintf(out, cap, "%s", r->status[i]);
        if (kind) *kind = r->kind[i];
    }
    plat_mutex_unlock_shared(&text_lock);
    return ok;
}

int bdat_merge_status(int i, char *out, size_t cap, int *kind) { return report_status(&reports[0], i, out, cap, kind); }
int bdat_merge_next_status(int i, char *out, size_t cap, int *kind) {
    return report_status(&reports[1], i, out, cap, kind);
}

void bdat_merge_next_clear(void) {
    plat_mutex_lock(&text_lock);
    reports[1].nstatus = reports[1].ntags = reports[1].nfiles = 0;
    plat_mutex_unlock(&text_lock);
}

// Under text_lock (exclusive).
static ModTag *tag_of(const char *id, const char *key) {
    Report *r = &reports[target];
    for (int i = 0; i < r->ntags; i++)
        if (!strcmp(r->tags[i].id, id) && !strcmp(r->tags[i].key, key)) return &r->tags[i];
    if (r->ntags == TAG_CAP) return NULL;
    ModTag *t = &r->tags[r->ntags++];
    memset(t, 0, sizeof *t);
    snprintf(t->id, sizeof t->id, "%s", id);
    snprintf(t->key, sizeof t->key, "%s", key);
    return t;
}

// The events of one merge (its report's lines, "<kind>: <mod>: ...") onto the tags of the mods that took part. Mods are
// named by label in events, and a label may hold ": " itself, so the longest label the line starts with is its mod.
static void tag_events(const char *key, const char *text, const BdatModFiles *mods, char (*label)[160], int n) {
    plat_mutex_lock(&text_lock);
    for (int k = 0; k < n; k++) tag_of(mods[k].id, key);
    static const char *const kinds[] = {"left out: ", "caveat: ", "adds nothing: ", "partly alike: "};
    for (const char *p = text; p && *p;) {
        const char *e = strchr(p, '\n');
        for (int x = 0; x < 4; x++) {
            size_t kl = strlen(kinds[x]);
            if (strncmp(p, kinds[x], kl)) continue;
            const char *rest = p + kl;
            int who = -1;
            size_t best = 0;
            for (int k = 0; k < n; k++) {
                size_t ll = strlen(label[k]);
                if (ll > best && !strncmp(rest, label[k], ll) && !strncmp(rest + ll, ": ", 2)) {
                    who = k;
                    best = ll;
                }
            }
            ModTag *t = who >= 0 ? tag_of(mods[who].id, key) : NULL;
            if (!t) break;
            if (x == 0) t->left++;
            else if (x == 1) t->caveats++;
            else if (x == 2) t->nothing = 1;
            else sscanf(rest + best + 2, "%d of the %d", &t->alike, &t->changed);
            break;
        }
        p = e ? e + 1 : NULL;
    }
    plat_mutex_unlock(&text_lock);
}

static int report_tag(const Report *r, const char *id, char *out, size_t cap, int *flags) {
    const ModTag *tags = r->tags;
    plat_mutex_lock_shared(&text_lock);
    int files = 0, nothing = 1, f = 0, ntags = r->ntags;
    size_t w = 0;
    out[0] = 0;
    for (int i = 0; i < ntags; i++)
        if (!strcmp(tags[i].id, id)) {
            files++;
            nothing &= tags[i].nothing;
        }
    if (files && nothing) {
        snprintf(out, cap, "adds nothing");
        f |= BDAT_TAG_ADDS_NOTHING;
    } else {
        int said = 0;   // files with something to say about it: their names go in front once there are several
        for (int i = 0; i < ntags; i++)
            said += !strcmp(tags[i].id, id) && (tags[i].left || tags[i].caveats || tags[i].alike);
        for (int i = 0; i < ntags && w < cap; i++) {
            const ModTag *t = &tags[i];
            if (strcmp(t->id, id) || !(t->left || t->caveats || t->alike)) continue;
            char part[160];
            int pn = 0;
            if (t->left) pn += snprintf(part + pn, sizeof part - (size_t)pn, "%s%d left out", pn ? ", " : "", t->left);
            if (t->caveats)
                pn += snprintf(part + pn, sizeof part - (size_t)pn, "%s%d caveat%s", pn ? ", " : "", t->caveats,
                               t->caveats == 1 ? "" : "s");
            if (t->alike)
                pn += snprintf(part + pn, sizeof part - (size_t)pn, "%s%d/%d tables alike", pn ? ", " : "", t->alike,
                               t->changed);
            const char *base = strrchr(t->key, '/');
            base = base ? base + 1 : t->key;
            int bl = (int)strcspn(base, ".");
            if (said > 1) w += (size_t)snprintf(out + w, cap - w, "%s%.*s: %s", w ? "; " : "", bl, base, part);
            else w += (size_t)snprintf(out + w, cap - w, "%s%s", w ? "; " : "", part);
            f |= (t->left ? BDAT_TAG_LEFT_OUT : 0) | (t->caveats ? BDAT_TAG_CAVEAT : 0);
        }
    }
    plat_mutex_unlock_shared(&text_lock);
    if (flags) *flags = f;
    return out[0] != 0;
}

int bdat_merge_mod_tag(const char *id, char *out, size_t cap, int *flags) {
    return report_tag(&reports[0], id, out, cap, flags);
}
int bdat_merge_next_mod_tag(const char *id, char *out, size_t cap, int *flags) {
    return report_tag(&reports[1], id, out, cap, flags);
}

typedef struct { char *text; size_t n, cap; int lines; const char *key; } Notes;

static void add_line(Notes *s, const char *line) {
    size_t len = strlen(line);
    if (s->n + len + 2 > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 4096;
        while (cap < s->n + len + 2) cap *= 2;
        char *t = realloc(s->text, cap);
        if (!t) return;
        s->text = t;
        s->cap = cap;
    }
    memcpy(s->text + s->n, line, len);
    s->n += len;
    s->text[s->n++] = '\n';
    s->text[s->n] = 0;
}

static void on_note(void *ctx, const char *line) {
    Notes *s = ctx;
    if (s->lines++ < LOG_NOTES) rt_log("bdat merge: %s: %s", s->key, line);
    else if (s->lines == LOG_NOTES + 1) rt_log("bdat merge: %s: more in the report file", s->key);
    add_line(s, line);
}

// The left-out and caveat lines of a report (one event per line), up to STATUS_EVENTS, into the status lines; then
// up to STATUS_EVENTS of its mods that add nothing or are partly alike, so those never push a left-out change out of
// view.
static void status_events(const char *key, const char *text) {
    int more = 0;
    for (int pass = 0; pass < 2; pass++) {
        int shown = 0;
        for (const char *p = text; p && *p;) {
            const char *e = strchr(p, '\n');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            int kind = !strncmp(p, "left out: ", 10) ? 1 : !strncmp(p, "caveat: ", 8) ? 2 :
                       !strncmp(p, "adds nothing: ", 14) ? 3 : !strncmp(p, "partly alike: ", 14) ? 4 : 0;
            if (kind && (kind >= 3) == pass) {
                if (shown < STATUS_EVENTS) {
                    add_status(kind, "%s: %.*s", key, (int)n, p);
                    shown++;
                } else {
                    more++;
                }
            }
            p = e ? e + 1 : NULL;
        }
    }
    if (more) add_status(0, "%s: %d more in the report", key, more);
}

// ---- the cache

typedef struct { const char *suffix; const char *keep; char dir[2100]; } Prune;

// One of this game file's merges: "<32 hex digits>-<flat>" with nothing after it, ".txt" (its report), ".sum" (its
// check) or a temporary file a write left (".<...>.tmp", from a process ended in the middle of a merge); or a
// temporary file of the quick note; or "game-<flat>", the copy of the game's own file earlier builds served when a
// merge of JSON tables failed (now the archive serves it). Only the exact form: another file's key that ends like this
// one never matches.
static int merge_file_of(const char *name, const char *flat) {
    size_t f = strlen(flat);
    if (!strncmp(name, "game-", 5) && !strcmp(name + 5, flat)) return 1;
    if (!strncmp(name, flat, f) && !strncmp(name + f, ".quick.", 7)) return strlen(name) > 4 && !strcmp(name + strlen(name) - 4, ".tmp");
    for (int i = 0; i < 32; i++)
        if (!isxdigit((unsigned char)name[i])) return 0;
    if (name[32] != '-' || strncmp(name + 33, flat, f)) return 0;
    const char *rest = name + 33 + f;
    size_t r = strlen(rest);
    return !*rest || !strcmp(rest, ".txt") || !strcmp(rest, ".sum") ||
           (rest[0] == '.' && r > 4 && !strcmp(rest + r - 4, ".tmp"));
}

static int prune_entry(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    Prune *p = ctx;
    size_t k = strlen(p->keep);
    int kept = !strncmp(name, p->keep, k) && (!name[k] || !strcmp(name + k, ".txt") || !strcmp(name + k, ".sum"));
    if (kind == PLAT_FS_FILE && merge_file_of(name, p->suffix) && !kept) {
        char path[2400];
        snprintf(path, sizeof path, "%s/%s", p->dir, name);
        plat_file_delete(path);   // may fail while the game still has it open; then the next merge deletes it
    }
    return 1;
}

// Deletes the merges of this game file (flat: its key with / as _) in dir other than keep (a file name).
static void prune(const char *dir, const char *flat, const char *keep) {
    Prune p = {flat, keep, ""};
    snprintf(p.dir, sizeof p.dir, "%s", dir);
    plat_fs_list(dir, prune_entry, &p);
}

static void hex(const uint8_t *d, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

// The status line of a merge: what was done and how much didn't go to plan.
static void summary_line(char *out, size_t cap, int n, const BdatSummary *s, char (*label)[160]) {
    if (s->file_mode == BDAT_FILE_COPIED && n == 1 && s->left_out)
        snprintf(out, cap, "%s's tables put in, %d left out (no other mod changes this file)", label[0], s->left_out);
    else if (s->file_mode == BDAT_FILE_COPIED && n == 1)
        snprintf(out, cap, "%s's tables put in (no other mod changes this file)", label[0]);
    else if (s->file_mode == BDAT_FILE_COPIED)
        snprintf(out, cap, "only %s's copy is used (the other mods' copies can't be)", label[s->file_mod]);
    else if (s->file_mode == BDAT_FILE_WHOLE)
        snprintf(out, cap, "%s's whole copy is used (it has another list of tables)", label[s->file_mod]);
    else {
        int w = snprintf(out, cap, "%d mods merged: %d table%s from several mods, %d settled by priority, %d change%s "
                         "left out, %d caveat%s", n, s->tables_merged, s->tables_merged == 1 ? "" : "s", s->priority,
                         s->left_out, s->left_out == 1 ? "" : "s", s->caveats, s->caveats == 1 ? "" : "s");
        if (s->adds_nothing && w > 0 && (size_t)w < cap)
            snprintf(out + w, cap - (size_t)w, ", %d mod%s adding nothing", s->adds_nothing,
                     s->adds_nothing == 1 ? "" : "s");
    }
}

// What a mod is called in events: the name in its library.ini (mods/<id>/library.ini, written by the mod managers;
// for a part of a mod, "<mod>: <part>"), else its id.
static void label_of(const char *id, char *out, size_t cap) {
    snprintf(out, cap, "%s", id);
    const char *dir = romfs_mods_dir();
    char path[4200];
    if (!dir || snprintf(path, sizeof path, "%s/%s/library.ini", dir, id) >= (int)sizeof path) return;
    uint64_t n = 0;
    char *text = (char *)plat_file_read(path, &n);
    for (char *p = text; p && p < text + n;) {
        char *e = memchr(p, '\n', (size_t)(text + n - p)), *end = e ? e : text + n, *q = p;
        while (q < end && (*q == ' ' || *q == '\t')) q++;
        if (end - q > 4 && !strncmp(q, "name", 4)) {
            q += 4;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (q < end && *q == '=') {
                q++;
                while (q < end && (*q == ' ' || *q == '\t')) q++;
                char *v = end;
                while (v > q && (v[-1] == ' ' || v[-1] == '\t' || v[-1] == '\r')) v--;
                if (v > q) snprintf(out, cap, "%.*s", (int)(v - q), q);
                break;
            }
        }
        p = e ? e + 1 : NULL;
    }
    free(text);
}

// The file name of a host path without its folders and .json.
static void stem_of(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    const char *f = slash && (!back || slash > back) ? slash + 1 : back ? back + 1 : path;
    size_t n = strlen(f);
    if (n > 5 && !strcmp(f + n - 5, ".json")) n -= 5;
    if (n >= cap) n = cap - 1;
    memcpy(out, f, n);
    out[n] = 0;
}

// A merge made before (file, its report next to it) served again: its report's last line is the summary, the lines
// before it the events, which go to the status lines and the per-mod tags as when it was made.
// The report file of a merge served (or made ahead) into the report of the merge in progress; under lock.
static void note_report(const char *path) {
    plat_mutex_lock(&text_lock);
    Report *r = &reports[target];
    if (r->nfiles < 8) snprintf(r->files[r->nfiles++], sizeof r->files[0], "%s", path);
    plat_mutex_unlock(&text_lock);
}

static void reuse(const char *key, const char *file, const char *how, const BdatModFiles *mods, char (*label)[160],
                  int n) {
    char report[2500];
    snprintf(report, sizeof report, "%s.txt", file);
    note_report(report);
    uint64_t rn = 0;
    char *text = (char *)plat_file_read(report, &rn);
    char *last = NULL;
    if (text) {
        char *t = realloc(text, rn + 1);
        if (t) {
            text = t;
            text[rn] = 0;
            while (rn && (text[rn - 1] == '\n' || text[rn - 1] == '\r')) text[--rn] = 0;
            last = strrchr(text, '\n');
        }
    }
    rt_log("bdat merge: %s: the merge made before, %s (%s)%s", key, how, file, target ? ", for the next start" : "");
    add_status(0, "%s: %s (as before)", key, last ? last + 1 : "merged");
    if (last) {
        *last = 0;
        status_events(key, text);
        tag_events(key, text, mods, label, n);
    }
    free(text);
}

// ---- the quick check
//
// Finding the merge made before by its name means reading and hashing every mod's copy: at 50 whole-file mods most
// of a start's 270 ms. So <profile>/bdat-merge/<key with / as _>.quick holds a key made only of what can be seen
// without reading them (each mod's id, and each of its files' path, size, last-write time, change time and identity
// on its volume; the game's stored copy, hashed with XXH64 instead of unpacked) and the name of the merged file that
// key gave. A copy tool can carry a file's size and last-write time over to a changed file, but not its change time
// or identity (plat_file_identity), so a changed mod file gives another key. A file on a file system not known to keep
// both (FAT, exFAT, network shares: plat_file_identity lists the ones that do) gives no quick key at all, so its inputs
// are read and hashed every start. The same key and that file still there, and still the
// file that was written (cached_ok): it is served without reading the mods. Anything else (a file replaced, one
// added, another order, a game update): the inputs are read and hashed as before, and the quick file is written again.

static int quick_file(Sha256 *s, const char *path) {
    int64_t v[2];
    uint64_t id[4];
    if (!plat_file_stat(path, &v[0], &v[1]) || !plat_file_identity(path, id)) return 0;
    sha256_update(s, path, strlen(path) + 1);
    sha256_update(s, v, sizeof v);
    sha256_update(s, id, sizeof id);
    return 1;
}

// <merged file>.sum: its size and XXH64, written after it. A merged file found again (by the quick key or by its
// name) is served only when it still has both; a file cut short, changed or left by an older build is merged again.
static int write_sum(const char *file, const uint8_t *data, size_t n) {
    char path[2600], line[64];
    int w = snprintf(line, sizeof line, "%zu %016llx\n", n, (unsigned long long)xxh64(data, n));
    return snprintf(path, sizeof path, "%s.sum", file) < (int)sizeof path && w > 0 &&
           plat_file_write_atomic(path, line, (size_t)w);
}

static int cached_ok(const char *file) {
    char path[2600], line[64] = "";
    if (snprintf(path, sizeof path, "%s.sum", file) >= (int)sizeof path) return 0;
    uint64_t sn = 0, n = 0;
    char *sum = (char *)plat_file_read(path, &sn);
    if (sum && sn < sizeof line) memcpy(line, sum, (size_t)sn);
    free(sum);
    unsigned long long want_n = 0, want_h = 0;
    if (sscanf(line, "%llu %llx", &want_n, &want_h) != 2 || plat_file_size(file) != (int64_t)want_n) return 0;
    uint8_t *data = plat_file_read(file, &n);
    int ok = data && n == want_n && xxh64(data, (size_t)n) == want_h;
    free(data);
    return ok;
}

// The quick key as 32 hex digits in out; 0 when a file can't be seen (then there's no quick check).
static int quick_key(const char *key, const uint8_t *stored, size_t stored_n, const BdatModFiles *mods, int n,
                     char out[33]) {
    Sha256 s;
    uint8_t d[32];
    uint64_t g[2] = {xxh64(stored, stored_n), stored_n};
    sha256_init(&s);
    sha256_update(&s, VERSION, sizeof VERSION);
    sha256_update(&s, key, strlen(key) + 1);
    sha256_update(&s, g, sizeof g);
    for (int k = 0; k < n; k++) {
        sha256_update(&s, mods[k].id, strlen(mods[k].id) + 1);
        if (!mods[k].full) sha256_update(&s, "-", 1);
        else if (!quick_file(&s, mods[k].full)) return 0;
        for (int x = 0; x < mods[k].njson; x++)
            if (!quick_file(&s, mods[k].json[x])) return 0;
    }
    sha256_final(&s, d);
    hex(d, 16, out);
    return 1;
}

// ---- each mod's work at the same time
//
// An uncached merge's time is mostly per mod and each mod's work needs only its own files and the game's copy:
// reading and hashing its copies here, putting in its JSON tables and comparing its tables with the game's in
// bdat_merge. So that runs on up to 8 threads, the calling one among them: one fewer than the logical processors, or
// all of them on a CPU with 4 or fewer (the game waits for this file anyway, and one thread fewer of 2 is none to
// help). A merge made ahead for the next start (bdat_merge_prepare) runs while the game plays, so it takes half the
// logical processors, at most 4, below normal priority: a little slower, out of the game's way. Whatever the order the
// work finishes in, its results and notes are taken in priority order afterwards, so the merged file and the report
// are the same as one after another.

typedef struct {
    void (*fn)(void *ctx, int i);
    void *ctx;
    int n;
    atomic_int next;
} Pool;

static void pool_run(void *arg) {
    Pool *p = arg;
    for (int i; (i = atomic_fetch_add(&p->next, 1)) < p->n;) p->fn(p->ctx, i);
}

static void parallel_for(int n, void (*fn)(void *ctx, int i), void *ctx) {
    uint32_t cpus = plat_cpu_count();
    int want = cpus <= 4 ? (int)cpus : (int)cpus - 1;   // a small CPU: all of it, as the game waits for this file
    if (want > 8) want = 8;
    if (target) want = cpus / 2 > 4 ? 4 : cpus / 2 < 1 ? 1 : (int)(cpus / 2);   // ahead, while the game plays
    if (want > n) want = n;
    Pool p = {fn, ctx, n, 0};
    PlatThread *t[8];
    int nt = 0;
    for (int i = 1; i < want; i++)
        if ((t[nt] = plat_thread_new(pool_run, &p, "bdat merge"))) {
            if (target) plat_thread_set_priority(t[nt], PLAT_PRIO_BELOW_NORMAL);
            nt++;
        }
    pool_run(&p);   // a thread that didn't start leaves its share to the others
    for (int i = 0; i < nt; i++) plat_thread_join(t[i]);
}

// A mod's copies read and hashed: its full copy into data[k] (SHA-256 in d_full[k], of "" when it has none) and its
// readable JSON tables into json[k] (SHA-256 of each in d_json[k]); the ones that can't be read marked to be noted.
typedef struct {
    const BdatModFiles *mods;
    uint8_t **data;
    uint64_t *size;
    BdatJson **json;
    int *njson;
    // per mod, one entry per JSON table it has (read_mod allocates them; any number of tables)
    char (*stems[64])[128];
    uint8_t (*d_json[64])[32];
    uint8_t *json_bad[64];
    uint8_t d_full[64][32];
    uint8_t full_bad[64], oom[64];
} Reads;

static void read_mod(void *ctx, int k) {
    Reads *r = ctx;
    const BdatModFiles *m = &r->mods[k];
    if (m->full && !(r->data[k] = plat_file_read(m->full, &r->size[k]))) {
        r->full_bad[k] = 1;
        return;
    }
    sha256(r->data[k] ? r->data[k] : (const uint8_t *)"", r->data[k] ? r->size[k] : 0, r->d_full[k]);
    if (m->njson && (!(r->json[k] = calloc((size_t)m->njson, sizeof *r->json[k])) ||
                     !(r->stems[k] = calloc((size_t)m->njson, sizeof *r->stems[k])) ||
                     !(r->d_json[k] = calloc((size_t)m->njson, sizeof *r->d_json[k])) ||
                     !(r->json_bad[k] = calloc((size_t)m->njson, 1)))) {
        r->oom[k] = 1;
        return;
    }
    for (int x = 0; x < m->njson; x++) {
        uint64_t sz = 0;
        uint8_t *p = plat_file_read(m->json[x], &sz);
        stem_of(m->json[x], r->stems[k][x], sizeof r->stems[k][x]);
        if (!p) {
            r->json_bad[k][x] = 1;
            continue;
        }
        sha256(p, (size_t)sz, r->d_json[k][r->njson[k]]);
        r->json[k][r->njson[k]++] = (BdatJson){r->stems[k][x], p, (size_t)sz};
    }
}

// bdat_merge_serve, or with next bdat_merge_prepare: into report 1, and the older merges are kept (this run serves one).
static int serve(const char *mount, const char *key, const BdatModFiles *mods, int n, char *out, size_t cap, int next) {
    if (n < 1 || n > 64 || !mount || !key) return 0;
    const RtConfig *cfg = rt_config();
    if (!cfg || !cfg->profile) return 0;
    // lock is held for the whole merge, one at a time. A merge made ahead (bdat_merge_prepare) runs below normal
    // priority, so a game thread opening a data table meanwhile waits on it: a priority inversion, accepted because the
    // game opens its tables only at start-up (before any merge ahead can run) and the Mods tab's next start is the
    // only other caller. A finer lock would be needed if that changed.
    plat_mutex_lock(&lock);
    target = next;
    int ok = 0, full = -1;
    char why[400] = "";
    uint8_t *game = NULL, *merged = NULL, *stored = NULL;
    Reads *reads = NULL;
    uint8_t *data[64] = {0};
    uint64_t size[64] = {0};
    BdatJson *json[64] = {0};
    int njson[64] = {0};
    char label[64][160];
    for (int k = 0; k < n; k++) label_of(mods[k].id, label[k], sizeof label[k]);
    for (int k = 1; k < n; k++)   // two mods of the same name: the later one also named by its id, so lines stay apart
        for (int j = 0; j < k; j++)
            if (!strcmp(label[k], label[j])) {
                char t[160];
                snprintf(t, sizeof t, "%.*s (%s)", (int)(sizeof t - 70), label[k], mods[k].id);
                snprintf(label[k], sizeof label[k], "%s", t);
                break;
            }
    size_t game_n = 0, merged_n = 0, stored_n = 0;
    Notes notes = {.key = key};
    uint64_t t0 = plat_now_ms();
    struct { char dir[2100]; } pr;
    char name[300], flat[256], file[2400], report[2500], quick[2400], qkey[33] = "";
    snprintf(flat, sizeof flat, "%s", key);
    for (char *c = flat; *c; c++)
        if (*c == '/' || *c == '\\') *c = '_';
    snprintf(pr.dir, sizeof pr.dir, "%s/bdat-merge", cfg->profile);
    snprintf(quick, sizeof quick, "%s/%s.quick", pr.dir, flat);
    for (int k = 0; full < 0 && k < n; k++)
        if (mods[k].full) full = k;   // not merged: the caller serves this mod's whole copy
    if (!(stored = game_stored(mount, key, &stored_n, why, sizeof why))) goto done;
    if (quick_key(key, stored, stored_n, mods, n, qkey)) {
        // "<quick key> <merged file name>": the same inputs as then, and its file still there
        uint64_t qn = 0;
        char *q = (char *)plat_file_read(quick, &qn), *nl = q ? strchr(q, '\n') : NULL;
        if (nl) *nl = 0;
        if (q && qn > 34 && !memcmp(q, qkey, 32) && q[32] == ' ' && !strchr(q + 33, '/') && !strchr(q + 33, '\\') &&
            snprintf(file, sizeof file, "%s/%s", pr.dir, q + 33) < (int)cap && cached_ok(file)) {
            snprintf(out, cap, "%s", file);
            ok = 1;
            reuse(key, file, "inputs unchanged", mods, label, n);
            if (!next) prune(pr.dir, flat, q + 33);   // a merge made ahead and then not used
        }
        free(q);
        if (ok) goto done;
    }
    // the first line names every mod for the reader; then one "mod: <id> <label>" line each, by which
    // bdat_merge_mod_lines finds a mod's lines (a name may hold "; ", and two mods may share one)
    size_t hcap = strlen(key) + 64 + (size_t)n * (sizeof label[0] + 2);
    char *head = malloc(hcap);
    if (!head) goto done;
    size_t hn = (size_t)snprintf(head, hcap, "%s merged from, highest priority first:", key);
    for (int k = 0; k < n; k++) hn += (size_t)snprintf(head + hn, hcap - hn, "%s %s", k ? ";" : "", label[k]);
    add_line(&notes, head);
    free(head);
    for (int k = 0; k < n; k++) {
        char line[260];
        snprintf(line, sizeof line, "mod: %s %s", mods[k].id, label[k]);
        add_line(&notes, line);
    }
    // every mod's copies read and hashed (at the same time when there are threads), then taken in priority order as
    // if one after another: a copy that can't be read ends it, and its notes come in order
    if (!(reads = calloc(1, sizeof *reads))) goto done;
    *reads = (Reads){.mods = mods, .data = data, .size = size, .json = json, .njson = njson};
    parallel_for(n, read_mod, reads);
    for (int k = 0; k < n; k++) {
        if (reads->full_bad[k]) {
            snprintf(why, sizeof why, "%s's copy can't be read", label[k]);
            goto done;
        }
        if (reads->oom[k]) goto done;
        for (int x = 0; x < mods[k].njson; x++)
            if (reads->json_bad[k][x]) {
                char line[400];
                snprintf(line, sizeof line, "left out: %s: its table %s.json can't be read", label[k], reads->stems[k][x]);
                on_note(&notes, line);
            }
    }
    game = game_unpack(stored, stored_n, key, &game_n, why, sizeof why);   // takes stored
    stored = NULL;
    if (!game) goto done;

    // the file name: <first 16 bytes of SHA-256(version, game copy, per mod its id, full copy and JSON tables)>-<key
    // with / as _>
    Sha256 s;
    uint8_t d[32];
    sha256_init(&s);
    sha256_update(&s, VERSION, sizeof VERSION);
    sha256(game, game_n, d);
    sha256_update(&s, d, 32);
    for (int k = 0; k < n; k++) {
        sha256_update(&s, mods[k].id, strlen(mods[k].id) + 1);
        sha256_update(&s, data[k] ? "f" : "-", 1);
        sha256_update(&s, reads->d_full[k], 32);   // read_mod: SHA-256 of the full copy ("" when none)
        for (int x = 0; x < njson[k]; x++) {
            sha256_update(&s, json[k][x].stem, strlen(json[k][x].stem) + 1);
            sha256_update(&s, reads->d_json[k][x], 32);
        }
    }
    sha256_final(&s, d);
    hex(d, 16, name);
    name[32] = '-';
    snprintf(name + 33, sizeof name - 33, "%s", flat);
    snprintf(file, sizeof file, "%s/%s", pr.dir, name);
    snprintf(report, sizeof report, "%s.txt", file);
    if (strlen(file) >= cap) goto done;
    if (cached_ok(file)) {
        snprintf(out, cap, "%s", file);
        ok = 1;
        reuse(key, file, "found by its inputs' contents", mods, label, n);
        if (!next) prune(pr.dir, flat, name);
        goto quick_note;
    }

    BdatInput base = {game, game_n, "game", NULL, 0}, in[64];
    for (int k = 0; k < n; k++) in[k] = (BdatInput){data[k], (size_t)size[k], label[k], json[k], njson[k]};
    BdatSummary sum;
    bdat_merge_set_parallel(parallel_for);
    if (!(merged = bdat_merge(&base, in, n, &merged_n, &sum, on_note, &notes, why, sizeof why))) goto done;
    plat_file_mkdir_p(pr.dir);
    if (!plat_file_write_atomic(file, merged, merged_n) || !write_sum(file, merged, merged_n)) {
        snprintf(why, sizeof why, "%s can't be written", file);
        goto done;
    }
    char line[400];
    summary_line(line, sizeof line, n, &sum, label);
    add_line(&notes, line);
    plat_file_write_atomic(report, notes.text, notes.n);
    note_report(report);
    if (!next) prune(pr.dir, flat, name);
    snprintf(out, cap, "%s", file);
    ok = 1;
    rt_log("bdat merge: %s: %s, in %llu ms%s (report %s)", key, line, (unsigned long long)(plat_now_ms() - t0),
           next ? ", for the next start" : "", report);
    add_status(0, "%s: %s", key, line);
    status_events(key, strchr(notes.text, '\n') ? strchr(notes.text, '\n') + 1 : "");
    tag_events(key, strchr(notes.text, '\n') ? strchr(notes.text, '\n') + 1 : "", mods, label, n);
quick_note:
    if (qkey[0]) {   // the next start with these inputs finds this file without reading them
        char line[400];
        int w = snprintf(line, sizeof line, "%s %s\n", qkey, name);
        if (w > 0 && (size_t)w < sizeof line) plat_file_write_atomic(quick, line, (size_t)w);
    }
done:
    if (!ok && full >= 0) {
        rt_log("bdat merge: %s: not merged (%s); %s's whole file is used", key, why[0] ? why : "no room for the path",
               label[full]);
        add_status(1, "%s: not merged (%s); %s's whole file is used", key, why, label[full]);
    } else if (!ok) {
        // only JSON tables, and they can't be put in: nothing served, so romfs_mods.c's romfs_mod_declines doesn't
        // claim the file and the game reads its own copy from the archive
        rt_log("bdat merge: %s: not merged (%s); the game's own copy is used", key,
               why[0] ? why : "no room for the path");
        add_status(1, "%s: not merged (%s); the game's own copy is used", key, why);
    }
    plat_mutex_unlock(&lock);
    for (int k = 0; k < n; k++) {
        free(data[k]);
        for (int x = 0; x < njson[k]; x++) free((void *)json[k][x].data);
        free(json[k]);
        if (reads) {
            free(reads->stems[k]);
            free(reads->d_json[k]);
            free(reads->json_bad[k]);
        }
    }
    free(reads);
    free(stored);
    free(game);
    free(merged);
    free(notes.text);
    return ok;
}

int bdat_merge_serve(const char *mount, const char *key, const BdatModFiles *mods, int n, char *out, size_t cap) {
    return serve(mount, key, mods, n, out, cap, 0);
}

int bdat_merge_prepare(const char *mount, const char *key, const BdatModFiles *mods, int n) {
    char out[4096];
    return serve(mount, key, mods, n, out, sizeof out, 1);
}

// ---- one mod's lines, for the Mods tab's details under its row

int bdat_merge_mod_lines(const char *id, int next, char (*out)[320], int *kinds, int cap) {
    char files[8][2500], mine[160];
    plat_mutex_lock_shared(&text_lock);
    const Report *r = &reports[next ? 1 : 0];
    int nf = r->nfiles;
    memcpy(files, r->files, sizeof files);
    plat_mutex_unlock_shared(&text_lock);
    label_of(id, mine, sizeof mine);
    static const char *const kind_names[] = {"priority: ", "left out: ", "caveat: ", "adds nothing: ", "partly alike: "};
    static const int kind_of[] = {0, 1, 2, 3, 4};
    int got = 0;
    for (int f = 0; f < nf && got < cap; f++) {
        uint64_t n = 0;
        char *text = (char *)plat_file_read(files[f], &n), *t;
        if (!text || !(t = realloc(text, n + 1))) {
            free(text);
            continue;
        }
        text = t;
        text[n] = 0;
        // the first line: "<key> merged from, highest priority first: ..."; then "mod: <id> <label>" per mod (serve)
        char key[256] = "", labels[64][160], ids[64][65];
        int nl = 0;
        char *nl1 = strchr(text, '\n');
        if (nl1) *nl1 = 0;
        const char *first = strstr(text, " merged from, highest priority first: ");
        if (first) snprintf(key, sizeof key, "%.*s", (int)(first - text), text);
        for (const char *p = nl1 ? nl1 + 1 : NULL; p && !strncmp(p, "mod: ", 5) && nl < 64;) {
            const char *e = strchr(p, '\n'), *sp = strchr(p + 5, ' ');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (sp && sp < p + len && (size_t)(sp - p - 5) < sizeof ids[0]) {
                snprintf(ids[nl], sizeof ids[0], "%.*s", (int)(sp - p - 5), p + 5);
                snprintf(labels[nl], sizeof labels[0], "%.*s", (int)strcspn(sp + 1, "\r\n"), sp + 1);
                nl++;
            }
            p = e ? e + 1 : NULL;
        }
        // this mod's label in this merge: by its id where the report names the mods by id (a mod not among them took no
        // part, and another mod of the same name must not lend it its lines); in a report from before the "mod:" lines,
        // by its name, with the labels taken from the first line
        const char *me = NULL;
        for (int l = 0; l < nl; l++)
            if (!strcmp(ids[l], id)) me = labels[l];
        if (!nl && first) {
            for (const char *p = first + strlen(" merged from, highest priority first: "); *p && nl < 64;) {
                const char *e = strstr(p, "; ");
                size_t len = e ? (size_t)(e - p) : strcspn(p, "\r");
                ids[nl][0] = 0;
                snprintf(labels[nl++], sizeof labels[0], "%.*s", (int)len, p);
                if (!e) break;
                p = e + 2;
            }
            me = mine;
        }
        if (!me) {
            free(text);
            continue;
        }
        for (char *p = nl1 ? nl1 + 1 : NULL; p && *p && got < cap;) {
            char *e = strchr(p, '\n');
            if (e) *e = 0;
            if (e && e > p && e[-1] == '\r') e[-1] = 0;
            for (int k = 0; k < 5; k++) {
                size_t kl = strlen(kind_names[k]);
                if (strncmp(p, kind_names[k], kl)) continue;
                // its mod: the longest label the rest starts with, as in tag_events (a label may hold ": ")
                const char *rest = p + kl, *who = NULL;
                size_t best = 0;
                for (int l = 0; l < nl; l++) {
                    size_t ll = strlen(labels[l]);
                    if (ll > best && !strncmp(rest, labels[l], ll) && !strncmp(rest + ll, ": ", 2)) {
                        who = labels[l];
                        best = ll;
                    }
                }
                if (who && !strcmp(who, me)) {
                    snprintf(out[got], 320, "%s: %s", key, p);
                    if (kinds) kinds[got] = kind_of[k];
                    got++;
                }
                break;
            }
            p = e ? e + 1 : NULL;
        }
        free(text);
    }
    return got;
}
