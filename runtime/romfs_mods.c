// Loose RomFS mod files in place of sts.ard entries, switchable while the game runs.
//
// Layout, under the mods folder of runtime/mods.c (XCX_MOD_DIR, else "mods" beside xcx.exe):
//   romfs-enabled.txt      one mod id per line, '#' starts a comment; earlier lines win when two mods have a file
//   <id>/romfs/<path>      a file that replaces (or adds) the game file <path>, e.g. <id>/romfs/chr/np/np009001.wimdo
//
// The archive device (sts.arh/sts.ard) keeps a list of paths it declines, which the game then opens as loose
// rom:/<path> files (recomp/romfs_mods.py). Where the archive reads that list for a path, romfs_mod_declines hands it
// a list holding just that path when an enabled mod has it; the game's open of rom:/<path> then reaches fs_host.c,
// which asks romfs_mod_file for the mod's copy.
//
// The list file is read again at most twice a second, and the file map is rebuilt when its bytes change, so a mod
// manager switches mods by rewriting romfs-enabled.txt. A changed mod applies to files the game opens after that;
// what it has already loaded stays until it loads the file again.
//
// Data tables (bdat/*.bdat) are merged instead (runtime/bdat_merge.c, docs/bdat-merge.md) when two or more enabled
// mods have the file, or when any has some of its tables as bdat-toolset JSON (bdat/<file>/<table>.json, as
// bdat/common/ITM_BeaconList.json for bdat/common.bdat). The map keeps every enabled mod's copy of such files in
// priority order, and the first open of the data table after a rebuild asks bdat_merge_serve for the merged file,
// which then stands in for the top mod's copy. When it can't be merged, the top mod's copy is served as before (or,
// with only JSON tables, the game's own copy). The game reads them only at start-up, so a list changed later can be
// merged ahead for the next start (romfs_mods_premerge).
#include "romfs_mods.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bdat_merge.h"
#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "platform/platform.h"

#define LIST_CAP 4096          // bytes of romfs-enabled.txt
#define MOD_CAP 32             // enabled mods
#define CHECK_MS 500           // how often the list file is read again
#define SIG_CAP (MOD_CAP * 76)  // table_inputs: per mod "<id>#<8 hex>,"
#define LINE_CAP 128           // lines of romfs-enabled.txt the menu's switches keep
#define LINE_LEN 1024          // and the longest one, comments included (longer: the list isn't rewritten)
#define AUTO_MS 1500           // with bdat_merge_auto, how long a changed list stays put before it is merged ahead

// key: lowercase game path without a leading '/'. host: the top mod's copy. For a data table or a JSON table that
// several mods have: every copy and its mod's id (alts[0] == host).
typedef struct {
    char *key, *host;
    char **alts, **ids;
    int nalts;
} ModFile;

// A data table merged since the last rebuild: its key and the file served for it (NULL: none).
typedef struct { char *key, *path; } Merged;
#define MERGED_CAP 16
#define STARTED_CAP 64         // data table files the game reads at start-up, with or without a mod's copy

static PlatMutex lock = PLAT_MUTEX_INIT;   // guards files / nfiles (shared for lookups)
static ModFile *files;
static size_t nfiles;
static uint64_t files_gen;                 // counts rebuilds, so a merge made for an older map is not kept
static char order[MOD_CAP][65];            // the enabled mods with a romfs folder, highest priority first
static int norder;
static Merged merged[MERGED_CAP];
static int nmerged;
// The data tables the game read at start-up and the mods each was made from, in priority order ("a,b,"), kept across
// rebuilds: the game doesn't read them again, so a later list that would make them otherwise needs a restart.
static Merged started[STARTED_CAP];
static int nstarted;
static char start_mount[64];               // the mount the game read them from
// The merges made ahead (romfs_mods_premerge): each data table of started and its mods then, as in started.
static Merged ahead[STARTED_CAP];
static int nahead, ahead_busy;
static uint64_t changed_at;                // when the list or a mod's files last changed; under check_lock (atomic
                                           // all the same: cheap, and safe if a reader without it is ever added)
static char cur_list[LIST_CAP + 1];        // the list text the map was built from; under lock
static char *start_list;                   // the list text when the game read its first data table; under lock
static int stale_now(void);
static int ahead_state(void);
static void refresh_sigs(void);
static PlatMutex check_lock = PLAT_MUTEX_INIT;
static char root[4096];
static int root_state;                     // 0 not looked up, 1 found, -1 none
static char list_seen[LIST_CAP + 1];
static int64_t list_len_seen = -2;         // -2: never read
static uint64_t next_check_ms;
static int claim_logs;
static int in_world = -1;                  // romfs_game_in_world: -1 not known yet, 0 at the title, 1 in the world

static int mods_root(void) {
    if (root_state) return root_state > 0;
    root_state = -1;
    const char *configured = getenv("XCX_MOD_DIR");
    if (configured && *configured) {
        if (strlen(configured) >= sizeof root - 64) return 0;
        strcpy(root, configured);
    } else {
        if (!plat_exe_path(root, sizeof root - 64)) return 0;
        char *slash = strrchr(root, '/'), *back = strrchr(root, '\\');
        if (back && (!slash || back > slash)) slash = back;
        if (!slash) return 0;
        strcpy(slash + 1, "mods");
    }
    root_state = 1;
    return 1;
}

typedef struct {
    ModFile *v;
    size_t n, cap;
    char host[4096];
    size_t hlen;
    char key[1024];
    size_t klen;
    const char *id;                        // the mod being walked
} Walk;

static int ends(const char *s, const char *tail) {
    size_t n = strlen(s), t = strlen(tail);
    return n > t && !strcmp(s + n - t, tail);
}

// A data table (bdat/....bdat) or a table of one as bdat-toolset JSON (bdat/.../<table>.json).
static int mergeable(const char *key) {
    return !strncmp(key, "bdat/", 5) && strlen(key) > 10 && (ends(key, ".bdat") || ends(key, ".json"));
}

// The mod id of a host path <root>/<id>/romfs/..., into id.
static void id_of(const char *host, char *id, size_t cap) {
    size_t r = strlen(root);
    const char *p = strlen(host) > r ? host + r + 1 : host, *e = strchr(p, '/');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n >= cap) n = cap - 1;
    memcpy(id, p, n);
    id[n] = 0;
}

// Another mod's copy of a data table the map already has, after the earlier mods' copies.
static int add_alt(ModFile *f, const char *host, const char *id) {
    char **a = realloc(f->alts, (size_t)(f->nalts + 2) * sizeof *a), **d;
    if (!a) return 0;
    f->alts = a;
    if (!(d = realloc(f->ids, (size_t)(f->nalts + 2) * sizeof *d))) return 0;
    f->ids = d;
    if (!f->nalts) {
        char first[80];
        id_of(f->host, first, sizeof first);
        a[0] = f->host;
        d[0] = strdup(first);
        f->nalts = 1;
    }
    a[f->nalts] = strdup(host);
    d[f->nalts] = strdup(id);
    f->nalts++;
    return 1;
}

static void free_file(ModFile *f) {
    for (int i = 1; i < f->nalts; i++) free(f->alts[i]);
    for (int i = 0; i < f->nalts; i++) free(f->ids[i]);
    free(f->alts);
    free(f->ids);
    free(f->key);
    free(f->host);
}

static int add_file(Walk *w) {
    for (size_t i = 0; i < w->n; i++)   // an earlier mod's file wins; lists are small enough for a scan at rebuild
        if (!strcmp(w->v[i].key, w->key)) return mergeable(w->key) ? add_alt(&w->v[i], w->host, w->id) : 1;
    if (w->n == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 256;
        ModFile *v = realloc(w->v, cap * sizeof *v);
        if (!v) return 0;
        w->v = v;
        w->cap = cap;
    }
    memset(&w->v[w->n], 0, sizeof w->v[w->n]);
    w->v[w->n].key = strdup(w->key);
    w->v[w->n].host = strdup(w->host);
    if (!w->v[w->n].key || !w->v[w->n].host) return 0;
    w->n++;
    return 1;
}

static int walk_entry(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    Walk *w = ctx;
    size_t n = strlen(name), hl = w->hlen, kl = w->klen;
    if (!n || !strcmp(name, ".") || !strcmp(name, "..") || hl + n + 2 >= sizeof w->host ||
        kl + n + 2 >= sizeof w->key)
        return 1;   // skipped; plat_fs_list goes on while the callback returns nonzero
    w->host[hl] = '/';
    memcpy(w->host + hl + 1, name, n + 1);
    if (kl) w->key[kl] = '/';
    char *k = w->key + (kl ? kl + 1 : 0);
    for (size_t i = 0; i <= n; i++) k[i] = (char)tolower((unsigned char)name[i]);
    w->hlen = hl + 1 + n;
    w->klen = (kl ? kl + 1 : 0) + n;
    if (kind == PLAT_FS_DIR) plat_fs_list(w->host, walk_entry, w);
    else if (kind == PLAT_FS_FILE) add_file(w);
    w->hlen = hl;
    w->klen = kl;
    w->host[hl] = 0;
    w->key[kl] = 0;
    return 1;
}

static int by_key(const void *a, const void *b) { return strcmp(((const ModFile *)a)->key, ((const ModFile *)b)->key); }

// The Lua mods' id rule: a-z first, then a-z 0-9 . _ -, at most 64 characters.
static int valid_id(const char *id, size_t n) {
    if (!n || n > 64 || !islower((unsigned char)id[0])) return 0;
    for (size_t i = 0; i < n; i++)
        if (!islower((unsigned char)id[i]) && !isdigit((unsigned char)id[i]) && !strchr("._-", id[i])) return 0;
    return 1;
}

// Rebuilds the file map from the list file's text (NUL-terminated).
static void rebuild(const char *list) {
    Walk w = {0};
    int mods = 0;
    char line[256], ids[MOD_CAP][65];
    for (const char *p = list; *p && mods < MOD_CAP;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && p[n - 1] == '\r') n--;
        if (n && n < sizeof line && p[0] != '#') {
            memcpy(line, p, n);
            line[n] = 0;
            if (valid_id(line, n)) {
                int len = snprintf(w.host, sizeof w.host, "%s/%s/romfs", root, line);
                if (len > 0 && (size_t)len < sizeof w.host && plat_fs_kind(w.host) == PLAT_FS_DIR) {
                    w.hlen = (size_t)len;
                    w.klen = 0;
                    w.key[0] = 0;
                    w.id = line;
                    plat_fs_list(w.host, walk_entry, &w);
                    memcpy(ids[mods], line, n + 1);
                    mods++;
                } else {
                    rt_log("romfs mods: %s has no romfs folder", line);
                }
            } else if (n) {
                rt_log("romfs mods: romfs-enabled.txt has an invalid id");
            }
        }
        if (!e) break;
        p = e + 1;
    }
    if (w.n) qsort(w.v, w.n, sizeof *w.v, by_key);
    plat_mutex_lock(&lock);
    snprintf(cur_list, sizeof cur_list, "%s", list);
    ModFile *old = files;
    size_t nold = nfiles;
    files = w.v;
    nfiles = w.n;
    files_gen++;
    memcpy(order, ids, sizeof order);
    norder = mods;
    Merged gone[MERGED_CAP];
    int ngone = nmerged;
    memcpy(gone, merged, sizeof gone);
    nmerged = 0;
    plat_mutex_unlock(&lock);
    for (int i = 0; i < ngone; i++) {
        free(gone[i].key);
        free(gone[i].path);
    }
    for (size_t i = 0; i < nold; i++) free_file(&old[i]);
    free(old);
    rt_log("romfs mods: %d mod%s, %zu file%s", mods, mods == 1 ? "" : "s", w.n, w.n == 1 ? "" : "s");
}

// Reads romfs-enabled.txt again when CHECK_MS passed, and rebuilds when it changed. One thread checks at a time;
// the others go on with the current map.
static void maybe_reload(void) {
    uint64_t now = plat_now_ms();
    if (now < __atomic_load_n(&next_check_ms, __ATOMIC_RELAXED) || !plat_mutex_trylock(&check_lock)) return;
    __atomic_store_n(&next_check_ms, now + CHECK_MS, __ATOMIC_RELAXED);
    if (mods_root()) {
        char path[4200], buf[LIST_CAP + 1];
        snprintf(path, sizeof path, "%s/romfs-enabled.txt", root);
        int64_t len = -1;
        PlatFile *f = plat_file_open(path);
        if (f) {
            len = plat_file_pread(f, buf, LIST_CAP, 0);
            plat_file_close(f);
        }
        if (len < 0) len = -1;
        buf[len > 0 ? len : 0] = 0;
        if (len != list_len_seen || (len > 0 && memcmp(buf, list_seen, (size_t)len))) {
            memcpy(list_seen, buf, (size_t)(len > 0 ? len : 0) + 1);
            list_len_seen = len;
            if (len >= 0 || nfiles) rebuild(buf);
            __atomic_store_n(&changed_at, now, __ATOMIC_RELAXED);
            refresh_sigs();   // the files of the list as it is now (a list change is rare: this is no timer)
        }
    }
    // bdat_merge_auto: once the mods have stayed put a moment (a few Up/Down clicks are one change, a file being copied
    // is still changing), the data tables are merged ahead whenever the game read them otherwise than they'd be made
    // now (another list, or a mod's files updated) and no merge for this list is made or running. stale_now and
    // ahead_state compare what was last looked at (refresh_sigs): no file is touched here.
    if (now - __atomic_load_n(&changed_at, __ATOMIC_RELAXED) >= AUTO_MS &&
        !__atomic_load_n(&ahead_busy, __ATOMIC_ACQUIRE) &&
        cfg_bool("XCX_BDAT_MERGE_AUTO") && stale_now() && ahead_state() == 0 && romfs_mods_premerge())
        rt_log("romfs mods: the mods changed; merging their data tables ahead for the next start");
    plat_mutex_unlock(&check_lock);
}

// The entry for key, or NULL; under lock.
static ModFile *find(const char *key) {
    size_t lo = 0, hi = nfiles;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int d = strcmp(files[mid].key, key);
        if (!d) return &files[mid];
        if (d < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

// The entries of key's JSON tables (bdat/common.bdat: bdat/common/<table>.json), as [*lo, *hi); under lock.
static void json_range(const char *key, size_t *lo, size_t *hi) {
    char prefix[1100];
    size_t n = strlen(key) - 5;   // without ".bdat"
    *lo = *hi = 0;
    if (n + 2 > sizeof prefix) return;
    memcpy(prefix, key, n);
    prefix[n++] = '/';
    prefix[n] = 0;
    size_t a = 0, b = nfiles;
    while (a < b) {
        size_t mid = (a + b) / 2;
        if (strcmp(files[mid].key, prefix) < 0) a = mid + 1;
        else b = mid;
    }
    *lo = a;
    while (a < nfiles && !strncmp(files[a].key, prefix, n)) a++;
    *hi = a;
}

static int is_json_of(const char *key, size_t prefix) {   // directly in the table's folder, a .json
    return !strchr(key + prefix, '/') && ends(key, ".json");
}

// Mod id's copy among an entry's copies, or NULL.
static const char *copy_of(const ModFile *f, const char *id) {
    char top[80];
    if (f->nalts < 2) {
        id_of(f->host, top, sizeof top);
        return strcmp(top, id) ? NULL : f->host;
    }
    for (int i = 0; i < f->nalts; i++)
        if (!strcmp(f->ids[i], id)) return f->alts[i];
    return NULL;
}

static uint32_t fnv(uint32_t h, const void *p, size_t n) {
    for (const uint8_t *b = p; n--; b++) h = (h ^ *b) * 16777619u;
    return h;
}

// A file's path, size and last-write time into h: an updated copy (same mod, same place) changes it. Less than the
// start-up check uses (bdat_merge_serve.c's quick key adds the change time and the file's identity, which takes a
// handle per file): a copy tool that keeps a file's size and dates gives no restart line here, and the next start still
// merges it again. One cheap stat per file is what the Mods tab can afford once a second.
static uint32_t stamp(uint32_t h, const char *path) {
    int64_t v[2] = {0, 0};
    plat_file_stat(path, &v[0], &v[1]);
    return fnv(fnv(h, path, strlen(path)), v, sizeof v);
}

static int gather(const char *key, BdatModFiles *in, int *ok, int *merge);
static void free_gathered(BdatModFiles *in, int nin);

// The mods of a table as gather gave them, each with a stamp of its files ("a#1f2e3d4c,b#...,") into sig. Reads only
// the copies of the paths, so it runs without the map's lock.
static void sig_of(const BdatModFiles *in, int nin, char *sig, size_t cap) {
    size_t n = 0;
    sig[0] = 0;
    for (int m = 0; m < nin; m++) {
        uint32_t h = 2166136261u;
        if (!in[m].id) continue;   // gather ran out of memory for it (its *ok 0): left out, so it reads as a change
        if (in[m].full) h = stamp(h, in[m].full);
        for (int x = 0; x < in[m].njson; x++)
            if (in[m].json[x]) h = stamp(h, in[m].json[x]);
        if (n + strlen(in[m].id) + 11 <= cap) n += (size_t)snprintf(sig + n, cap - n, "%s#%08x,", in[m].id, h);
    }
}

// The enabled mods that have a copy of data table key or JSON tables of it, highest priority first, each with a stamp
// of those files: what the table would be made from now. Under the map's lock (it looks at every file: boot, and the
// merge made ahead; the menu's checks go through refresh_sigs, which looks at them without the lock).
static void table_inputs(const char *key, char *sig, size_t cap) {
    BdatModFiles in[MOD_CAP];
    int ok = 1, merge = 0;
    int nin = gather(key, in, &ok, &merge);
    sig_of(in, nin, sig, cap);
    free_gathered(in, nin);
}

// What each data table the game read would be made from now, as last looked at: indexed as started[], NULL while
// nothing was looked at since start-up (then: as at start-up). Mod files are looked at only after the list changes
// (maybe_reload's rebuild), after an update (romfs_mods_reload, which rebuilds too) and when the Mods tab asks
// (romfs_mods_check_files), never on a timer: with the menu closed, nothing here touches a mod file.
static char *cur_sig[STARTED_CAP];
static uint64_t sig_at[STARTED_CAP];           // when that look began: an older one never replaces a newer one
static __thread int looked_here;               // refresh_sigs ran on this thread (romfs_mods_check_files clears it)
static PlatMutex sig_lock = PLAT_MUTEX_INIT;   // guards cur_sig and sig_at

// Table i's signature as last looked at into out; under the map's lock (for started[]).
static void sig_now(int i, char *out, size_t cap) {
    plat_mutex_lock(&sig_lock);
    snprintf(out, cap, "%s", cur_sig[i] ? cur_sig[i] : started[i].path ? started[i].path : "");
    plat_mutex_unlock(&sig_lock);
}

// Sets table key's signature from a look that began at ns (plat_now_ns; a merge made ahead is one look too); 1 when it
// differs from before. A look that began before the stored one is dropped: the Mods tab's look and a merge made ahead
// can finish in either order. Under the map's lock.
static int set_sig(const char *key, const char *sig, uint64_t ns) {
    for (int i = 0; i < nstarted; i++)
        if (!strcmp(started[i].key, key)) {
            char *copy = strdup(sig);
            if (!copy) return 0;
            plat_mutex_lock(&sig_lock);
            int differs = 0;
            if (ns >= sig_at[i]) {
                const char *was = cur_sig[i] ? cur_sig[i] : started[i].path ? started[i].path : "";
                differs = strcmp(was, sig) != 0;
                free(cur_sig[i]);
                cur_sig[i] = copy;
                sig_at[i] = ns;
                copy = NULL;
            }
            plat_mutex_unlock(&sig_lock);
            free(copy);
            return differs;
        }
    return 0;
}

// Looks at every mod file of every data table the game read, once for all of them: the paths copied under the map's
// lock, the files looked at after it is released, so a slow disk never holds up the game's lookups. A change from
// what was seen last restarts bdat_merge_auto's wait, so a file still being written isn't merged half done.
static void refresh_sigs(void) {
    plat_mutex_lock_shared(&lock);
    int n = nstarted;
    plat_mutex_unlock_shared(&lock);
    int changed = 0;
    for (int i = 0; i < n; i++) {
        BdatModFiles in[MOD_CAP];
        int ok = 1, merge = 0;
        uint64_t t = plat_now_ns();
        plat_mutex_lock_shared(&lock);
        const char *key = started[i].key;   // started[] entries stay for the run
        int nin = gather(key, in, &ok, &merge);
        plat_mutex_unlock_shared(&lock);
        char sig[SIG_CAP];
        sig_of(in, nin, sig, sizeof sig);
        free_gathered(in, nin);
        plat_mutex_lock_shared(&lock);
        changed |= set_sig(key, sig, t);
        plat_mutex_unlock_shared(&lock);
    }
    if (changed) __atomic_store_n(&changed_at, plat_now_ms(), __ATOMIC_RELAXED);
    looked_here = 1;
}

// Per enabled mod with a copy of data table key or JSON tables of it, in priority order: those files as in[] (the
// count), with copies of the paths (in in[] itself: each mod's id, full copy and JSON list) for the caller to free with free_gathered; any number of JSON tables. *merge: 1
// when they are to be merged (JSON tables, or two or more full copies). Under the map's lock.
static int gather(const char *key, BdatModFiles *in, int *ok, int *merge) {
    ModFile *f = find(key);
    size_t lo = 0, hi = 0, pre = strlen(key) - 4;
    int nin = 0, full = 0, json_any = 0;
    json_range(key, &lo, &hi);
    *ok = 1;
    for (int m = 0; m < norder; m++) {
        const char *copy_full = f ? copy_of(f, order[m]) : NULL;
        const char **json = NULL;
        int njson = 0, cap = 0;
        for (size_t i = lo; i < hi; i++) {
            const char *p = is_json_of(files[i].key, pre) ? copy_of(&files[i], order[m]) : NULL;
            if (!p) continue;
            if (njson == cap) {
                const char **g = realloc(json, (size_t)(cap = cap ? cap * 2 : 16) * sizeof *json);
                if (!g) {
                    *ok = 0;
                    break;
                }
                json = g;
            }
            if (!(json[njson++] = strdup(p))) *ok = 0;
        }
        if (!copy_full && !njson) {
            free(json);
            continue;
        }
        char *id = strdup(order[m]), *copy = copy_full ? strdup(copy_full) : NULL;
        if (!id || (copy_full && !copy)) *ok = 0;
        in[nin++] = (BdatModFiles){id, copy, json, njson};
        full += copy_full != NULL;
        json_any |= njson > 0;
    }
    *merge = json_any || full >= 2;
    return nin;
}

static void free_gathered(BdatModFiles *in, int nin) {
    for (int i = 0; i < nin; i++) {
        for (int x = 0; x < in[i].njson; x++) free((void *)in[i].json[x]);
        free((void *)in[i].json);
        free((void *)in[i].id);
        free((void *)in[i].full);
    }
}

// A data table file the game reads (it reads them only at start-up), kept in started with the mods it would be made
// from now: none when the game reads its own copy from the archive, so a mod for it turned on later still reads as a
// change. mount: the RomFS mount, for merges made ahead. Without the lock.
static void note_table(const char *key, const char *mount) {
    plat_mutex_lock_shared(&lock);
    int known = 0;
    for (int i = 0; i < nstarted; i++) known |= !strcmp(started[i].key, key);
    int need = !known || (!start_mount[0] && mount && *mount);
    plat_mutex_unlock_shared(&lock);
    if (!need) return;
    plat_mutex_lock(&lock);
    if (!start_mount[0] && mount && strlen(mount) < sizeof start_mount) snprintf(start_mount, sizeof start_mount, "%s", mount);
    known = 0;
    for (int i = 0; i < nstarted; i++) known |= !strcmp(started[i].key, key);
    if (!start_list) start_list = strdup(cur_list);   // the list the game started with, for romfs_mods_restore_list
    if (!known && nstarted < STARTED_CAP && (started[nstarted].key = strdup(key))) {
        char sig[SIG_CAP];
        table_inputs(key, sig, sizeof sig);
        started[nstarted].path = strdup(sig);
        nstarted++;
    } else if (!known && nstarted == STARTED_CAP) {
        rt_log("romfs mods: more than %d data table files read; %s isn't followed for the restart line", STARTED_CAP, key);
    }
    plat_mutex_unlock(&lock);
}

static int is_table(const char *key) { return !strncmp(key, "bdat/", 5) && ends(key, ".bdat"); }

// The host path of the mod file for key (lowercase, no leading '/'), copied to out; 0 when no enabled mod has it.
// mount (the RomFS mount name; NULL when only asking whether a mod has the file) lets a data table be merged.
static int lookup(const char *key, char *out, size_t cap, const char *mount) {
    if (mount && out && is_table(key)) note_table(key, mount);   // the game reading a data table
    plat_mutex_lock_shared(&lock);
    ModFile *f = find(key);
    size_t lo = 0, hi = 0, nj = 0;
    int table = !strncmp(key, "bdat/", 5) && ends(key, ".bdat");
    if (table) {
        json_range(key, &lo, &hi);
        for (size_t i = lo; i < hi; i++) nj += (size_t)is_json_of(files[i].key, strlen(key) - 4);
    }
    if (!f && !nj) {
        plat_mutex_unlock_shared(&lock);
        return 0;
    }
    int merge = table && (nj || f->nalts >= 2);
    if (!merge || !mount || !out) {
        // A table to merge, opened before the RomFS is mounted (no game copy to merge onto): the top mod's whole copy,
        // or the archive's when only JSON mods change it. Said once, since the merge waits for the mount.
        static int premount_logged;
        if (merge && out && !mount && !__atomic_exchange_n(&premount_logged, 1, __ATOMIC_RELAXED))
            rt_log("romfs mods: %s opened before the RomFS is mounted; %s", key,
                   f ? "the top mod's whole copy is used, unmerged" : "the game's own copy is used");
        // asked only whether a mod has the file: yes. Asked for its path: only when it fits, so a caller never opens a
        // buffer nothing was copied into
        int ok = !out || (f && strlen(f->host) < cap);
        if (out && ok) strcpy(out, f->host);
        plat_mutex_unlock_shared(&lock);
        return ok;
    }
    for (int i = 0; i < nmerged; i++)
        if (!strcmp(merged[i].key, key)) {
            int ok = merged[i].path && strlen(merged[i].path) < cap;
            if (ok) strcpy(out, merged[i].path);
            plat_mutex_unlock_shared(&lock);
            return ok;
        }
    // the first open since the rebuild: merged outside the map's lock, from copies of the paths, per mod in priority
    // order its full copy (if any) and its JSON tables
    uint64_t gen = files_gen;
    BdatModFiles in[MOD_CAP];
    int ok = 1, many = 0;
    int nin = gather(key, in, &ok, &many);
    (void)many;   // decided above, from the map as the game reads it
    plat_mutex_unlock_shared(&lock);
    // the merge gets the caller's room for the path: one that doesn't fit is "not merged (no room for the path)" in
    // serve, which then leaves the top mod's whole copy below, instead of a merge made and then not served
    char path[ROMFS_MOD_PATH_CAP];
    ok = ok && bdat_merge_serve(mount, key, in, nin, path, cap < sizeof path ? cap : sizeof path);
    const char *served = ok ? path : NULL;
    for (int i = 0; !served && i < nin; i++) served = in[i].full;   // not merged: the top mod's whole copy
    if (served && strlen(served) < cap) strcpy(out, served);
    else served = NULL;
    plat_mutex_lock(&lock);
    int known = 0;
    for (int i = 0; i < nmerged; i++) known |= !strcmp(merged[i].key, key);
    if (files_gen == gen && !known && nmerged < MERGED_CAP) {
        merged[nmerged].key = strdup(key);
        merged[nmerged].path = served ? strdup(served) : NULL;
        if (merged[nmerged].key) nmerged++;
    }
    plat_mutex_unlock(&lock);
    free_gathered(in, nin);
    return served != NULL;
}

// key from a game path: drops a "<mount>:" prefix and leading slashes, lowercases.
static int make_key(const char *path, char *key, size_t cap) {
    const char *colon = strchr(path, ':');
    if (colon) path = colon + 1;
    while (*path == '/' || *path == '\\') path++;
    size_t n = strlen(path);
    if (!n || n >= cap) return 0;
    for (size_t i = 0; i <= n; i++) key[i] = path[i] == '\\' ? '/' : (char)tolower((unsigned char)path[i]);
    return 1;
}

// A printable guest C string at p (at least 2 characters), copied to out; 0 when there is none.
static int guest_string(uint64_t p, char *out, size_t cap) {
    if (!p || !plat_mem_readable(p, 1)) return 0;
    size_t n = 0;
    for (; n < cap - 1; n++) {
        if (n && ((p + n) & 0xfff) == 0 && !plat_mem_readable(p + n, 1)) return 0;
        unsigned char ch = *(const unsigned char *)(uintptr_t)(p + n);
        if (!ch) break;
        if (ch < 32 || ch >= 127) return 0;
        out[n] = (char)ch;
    }
    out[n] = 0;
    return n >= 2 && n < cap - 1;
}

uint64_t romfs_mod_declines(uint64_t list, uint64_t path) {
    maybe_reload();
    // One entry, then the empty string that ends the list. Thread-local: the archive walks it right away on this thread.
    static __thread char one[1026];
    char raw[1024];
    if (!guest_string(path, raw, sizeof raw) || !make_key(raw, one, sizeof one - 1)) return list;
    // A data table is noted even when the game reads its own copy (no mod has it, or no mod is on): the restart line
    // and a merge made ahead then know the game read it.
    if (is_table(one)) note_table(one, romfs_mount_name());
    // In the world or at the title screen, from what the game loads (a file trace of boot, a save loaded, Return to
    // Title): the world's map information and collision come only once a save is loaded or a new game started, and
    // the title's scene (ev/title/) at boot and at each return to the title.
    if (!strncmp(one, "ev/title/", 9)) __atomic_store_n(&in_world, 0, __ATOMIC_RELAXED);
    else if (!strcmp(one, "gimmick/world_info.bin") || !strncmp(one, "map/mapcoli/", 12))
        __atomic_store_n(&in_world, 1, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&nfiles, __ATOMIC_RELAXED)) return list;
    if (is_table(one)) {
        // A data table is claimed only once there is a file to serve for it: the merge (or the reuse of one made
        // before) runs here, before the game is told to open its rom:/ path. When nothing can be served (the profile
        // can't be written, a path too long, out of memory, JSON tables before the RomFS is mounted), the game reads
        // its own copy from the archive instead of finding no file at all.
        // the same room the open takes the path in (fs_host.c's open_file, PATH_CAP): a path that fits here but not
        // there would be claimed and then not opened
        char host[ROMFS_MOD_PATH_CAP];
        const char *mount = romfs_mount_name();
        if (!lookup(one, host, sizeof host, mount && *mount ? mount : NULL)) return list;
    } else if (!lookup(one, NULL, 0, NULL)) {
        return list;
    }
    one[strlen(one) + 1] = 0;
    if (__atomic_fetch_add(&claim_logs, 1, __ATOMIC_RELAXED) < 64) rt_log("romfs mods: %s from a mod", one);
    return (uint64_t)(uintptr_t)one;
}

int romfs_mod_file(const char *path, char *out, size_t cap) {
    maybe_reload();
    if (!__atomic_load_n(&nfiles, __ATOMIC_RELAXED) || !path) return 0;
    char key[1024], mount[64] = "";
    const char *colon = strchr(path, ':');
    if (colon && (size_t)(colon - path) < sizeof mount) {
        memcpy(mount, path, (size_t)(colon - path));
        mount[colon - path] = 0;
    }
    return make_key(path, key, sizeof key) && lookup(key, out, cap, *mount ? mount : NULL);
}

// ---- switching, for the F1 menu (runtime/mod_library.c) ------------------------------------------------------------------------------------------

// The ids of romfs-enabled.txt, in order, into ids (each 65 bytes); the count.
static int read_enabled(char (*ids)[65], int cap) {
    char path[4200];
    snprintf(path, sizeof path, "%s/romfs-enabled.txt", root);
    uint64_t size = 0;
    char *text = (char *)plat_file_read(path, &size);
    int n = 0;
    for (char *p = text; p && *p && n < cap;) {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        while (len && (p[len - 1] == '\r' || p[len - 1] == ' ')) len--;
        if (p[0] != '#' && valid_id(p, len)) {
            int dup = 0;
            for (int i = 0; i < n; i++) dup |= strlen(ids[i]) == len && !memcmp(ids[i], p, len);
            if (!dup) {
                memcpy(ids[n], p, len);
                ids[n++][len] = 0;
            }
        }
        if (!e) break;
        p = e + 1;
    }
    free(text);
    return n;
}

const char *romfs_mods_dir(void) { return mods_root() ? root : NULL; }

void romfs_mods_reload(void) {
    plat_mutex_lock(&check_lock);
    list_len_seen = -2;   // rebuilt from the list at the next check even when its bytes are the same
    plat_mutex_unlock(&check_lock);
    __atomic_store_n(&next_check_ms, 0, __ATOMIC_RELAXED);
}

// ---- the list file as lines, for the menu's switches
//
// A mod turned off from the menu while the game runs is remembered with the mods that were on above it, so turning it
// on again in the same run puts it back where it was. Any other mod turned on (new, on before a restart, or never on)
// goes last, at the lowest priority: it shouldn't win over everything at once, and the player moves it up if it
// should. Comment lines stay as they are.

static int read_lines(char (*lines)[LINE_LEN], int cap) {
    char path[4200];
    snprintf(path, sizeof path, "%s/romfs-enabled.txt", root);
    uint64_t size = 0;
    char *text = (char *)plat_file_read(path, &size);
    int n = 0;
    for (char *p = text; p && *p;) {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        while (len && (p[len - 1] == '\r' || p[len - 1] == ' ')) len--;
        if (len >= LINE_LEN || (len && n == cap)) {   // a line or a list it can't keep whole: not rewritten at all
            rt_log("romfs mods: romfs-enabled.txt has %s; the menu leaves it as it is",
                   len >= LINE_LEN ? "a line too long to keep" : "more lines than it keeps");
            free(text);
            return -1;
        }
        if (len) {
            memcpy(lines[n], p, len);
            lines[n++][len] = 0;
        }
        if (!e) break;
        p = e + 1;
    }
    free(text);
    return n;
}

static int write_lines(char (*lines)[LINE_LEN], int n) {
    char text[LIST_CAP], path[4200];
    size_t len = 0;
    // the whole list or nothing: a list that doesn't fit is left as it is, as read_lines does
    for (int i = 0; i < n; i++) {
        size_t l = strlen(lines[i]);
        if (len + l + 1 > sizeof text) {
            rt_log("romfs mods: romfs-enabled.txt would be longer than %d bytes; the menu leaves it as it is",
                   LIST_CAP);
            return 0;
        }
        memcpy(text + len, lines[i], l);
        text[len + l] = '\n';
        len += l + 1;
    }
    plat_file_mkdir_p(root);
    snprintf(path, sizeof path, "%s/romfs-enabled.txt", root);
    int ok = plat_file_write_atomic(path, text, len);
    __atomic_store_n(&next_check_ms, 0, __ATOMIC_RELAXED);   // the next file lookup reads the new list
    return ok;
}

// Mods turned off from the menu this run: each with the mods that were on above it then, nearest first.
typedef struct { char id[65]; char above[MOD_CAP][65]; int nabove; } OffPlace;
static OffPlace places[MOD_CAP];
static int nplaces;
static PlatMutex place_lock = PLAT_MUTEX_INIT;

static int is_mod_line(const char *line) { return line[0] != '#' && valid_id(line, strlen(line)); }

int romfs_mods_set(const char *id, int on) {
    if (!mods_root() || !id || !valid_id(id, strlen(id))) return 0;
    char lines[LINE_CAP + 1][LINE_LEN];
    int n = read_lines(lines, LINE_CAP), at = -1, k = 0;
    if (n < 0) return 0;
    for (int i = 0; i < n; i++) {   // its own line out (where it was kept in at)
        if (!strcmp(lines[i], id)) {
            if (at < 0) at = k;
        } else {
            if (k != i) memcpy(lines[k], lines[i], sizeof lines[k]);
            k++;
        }
    }
    n = k;
    plat_mutex_lock(&place_lock);
    int p = 0;
    while (p < nplaces && strcmp(places[p].id, id)) p++;
    if (!on && at >= 0) {   // remember the mods on above it, for turning it on again this run
        if (p == nplaces && nplaces < MOD_CAP) nplaces++;
        if (p < nplaces) {
            OffPlace *o = &places[p];
            snprintf(o->id, sizeof o->id, "%s", id);
            o->nabove = 0;
            for (int i = at - 1; i >= 0 && o->nabove < MOD_CAP; i--)
                if (is_mod_line(lines[i])) snprintf(o->above[o->nabove++], 65, "%s", lines[i]);
        }
    } else if (on && at < 0) {
        at = n;   // not remembered: last, the lowest priority
        if (p < nplaces) {   // remembered: right below the nearest mod that was above it and is still on, else first
            at = 0;
            while (at < n && !is_mod_line(lines[at])) at++;   // first: above the first mod, below leading comments
            int found = 0;
            for (int a = 0; a < places[p].nabove && !found; a++)
                for (int i = 0; i < n; i++)
                    if (!strcmp(lines[i], places[p].above[a])) {
                        at = i + 1;
                        found = 1;
                        break;
                    }
            places[p] = places[--nplaces];
        }
    }
    plat_mutex_unlock(&place_lock);
    if (on) {   // back in (at its old line when it was on already)
        memmove(lines[at + 1], lines[at], sizeof lines[0] * (size_t)(n - at));
        snprintf(lines[at], sizeof lines[at], "%s", id);
        n++;
    }
    int ok = write_lines(lines, n);
    rt_log("romfs mods: %s turned %s from the menu%s", id, on ? "on" : "off", ok ? "" : " (the list could not be written)");
    return ok;
}

// The "<id>#<stamp>," entry of id in a table_inputs signature into out ("" when id isn't in it).
static void sig_entry(const char *sig, const char *id, char *out, size_t cap) {
    size_t il = strlen(id);
    out[0] = 0;
    for (const char *p = sig; p && *p;) {
        const char *e = strchr(p, ',');
        if (!strncmp(p, id, il) && p[il] == '#') {
            snprintf(out, cap, "%.*s", (int)(e ? e - p : (long)strlen(p)), p);
            return;
        }
        p = e ? e + 1 : NULL;
    }
}

int romfs_mod_changed(const char *id) {
    maybe_reload();
    int how = 0;
    plat_mutex_lock_shared(&lock);
    for (int i = 0; i < nstarted && how != 1; i++) {
        char now[SIG_CAP], a[96], b[96];
        sig_now(i, now, sizeof now);
        sig_entry(started[i].path ? started[i].path : "", id, a, sizeof a);
        sig_entry(now, id, b, sizeof b);
        if (b[0] && !a[0]) how = 2;
        else if (a[0] && b[0] && strcmp(a, b)) how = 1;
    }
    plat_mutex_unlock_shared(&lock);
    return how;
}

int romfs_game_in_world(void) { return __atomic_load_n(&in_world, __ATOMIC_RELAXED) != 0; }   // unknown counts as in

int romfs_mods_list_changed(void) {
    maybe_reload();
    plat_mutex_lock_shared(&lock);
    int changed = start_list && strcmp(start_list, cur_list) != 0;
    plat_mutex_unlock_shared(&lock);
    return changed;
}

int romfs_mods_restore_list(void) {
    if (!mods_root()) return 0;
    plat_mutex_lock_shared(&lock);
    char *text = start_list ? strdup(start_list) : NULL;
    plat_mutex_unlock_shared(&lock);
    if (!text) return 0;
    char path[4200];
    snprintf(path, sizeof path, "%s/romfs-enabled.txt", root);
    int ok = plat_file_write_atomic(path, text, strlen(text));
    free(text);
    rt_log("romfs mods: the list put back as the game started with it from the menu%s",
           ok ? "" : " (the list could not be written)");
    __atomic_store_n(&next_check_ms, 0, __ATOMIC_RELAXED);
    return ok;
}

int romfs_mods_tables_changed(void) {
    maybe_reload();
    return stale_now();
}

void romfs_mods_check_files(void) {
    if (!mods_root()) return;
    looked_here = 0;
    maybe_reload();   // after a list change it rebuilds and looks itself: then that look is this refresh's
    if (looked_here) return;
    // one look at a time, and not inside a rebuild: a game thread holding check_lock is rebuilding or looking right
    // now, so the menu keeps what it has rather than wait for it (the next refresh looks)
    if (!plat_mutex_trylock(&check_lock)) return;
    refresh_sigs();
    plat_mutex_unlock(&check_lock);
}

static int stale_now(void) {
    plat_mutex_lock_shared(&lock);
    int changed = 0;
    for (int i = 0; !changed && i < nstarted; i++) {
        char sig[SIG_CAP];
        sig_now(i, sig, sizeof sig);
        changed = !started[i].path || strcmp(sig, started[i].path) != 0;
    }
    plat_mutex_unlock_shared(&lock);
    return changed;
}

// ---- merging ahead for the next start

static void premerge_run(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);   // the game plays meanwhile; bdat_merge_serve's helpers match it
    maybe_reload();
    char keys[STARTED_CAP][1024], mount[64];
    plat_mutex_lock_shared(&lock);
    int nk = 0;
    for (int i = 0; i < nstarted; i++)
        if (strlen(started[i].key) < sizeof keys[0]) snprintf(keys[nk++], sizeof keys[0], "%s", started[i].key);
    snprintf(mount, sizeof mount, "%s", start_mount);
    plat_mutex_unlock_shared(&lock);
    bdat_merge_next_clear();
    Merged done[STARTED_CAP];
    int nd = 0;
    uint64_t t0 = plat_now_ms();
    for (int k = 0; k < nk; k++) {
        BdatModFiles in[MOD_CAP];
        char sig[SIG_CAP];
        int ok = 1, merge = 0;
        uint64_t t = plat_now_ns();
        plat_mutex_lock_shared(&lock);
        int nin = gather(keys[k], in, &ok, &merge);
        plat_mutex_unlock_shared(&lock);
        // what this merge is made from is a look at the files too, taken from the copies of the paths without the
        // map's lock (this thread runs below normal priority); ahead_state compares it with the newest look, and a
        // look that began earlier doesn't replace it (set_sig), so the merge can't seem out of date and bdat_merge_auto
        // merge again and again
        sig_of(in, nin, sig, sizeof sig);
        plat_mutex_lock_shared(&lock);
        set_sig(keys[k], sig, t);
        plat_mutex_unlock_shared(&lock);
        // as the next start would: merged where there is something to merge; else one mod's copy or the game's own
        if (ok && merge && mount[0]) bdat_merge_prepare(mount, keys[k], in, nin);
        free_gathered(in, nin);
        done[nd].key = strdup(keys[k]);
        done[nd].path = strdup(sig);
        nd++;
    }
    rt_log("romfs mods: %d data table%s merged ahead for the next start, in %llu ms", nk, nk == 1 ? "" : "s",
           (unsigned long long)(plat_now_ms() - t0));
    plat_mutex_lock(&lock);
    Merged gone[STARTED_CAP];
    int ngone = nahead;
    memcpy(gone, ahead, sizeof gone);
    memcpy(ahead, done, sizeof done);
    nahead = nd;
    plat_mutex_unlock(&lock);
    for (int i = 0; i < ngone; i++) {
        free(gone[i].key);
        free(gone[i].path);
    }
    __atomic_store_n(&ahead_busy, 0, __ATOMIC_RELEASE);
}

int romfs_mods_premerge(void) {
    plat_mutex_lock_shared(&lock);
    int any = nstarted > 0 && start_mount[0];
    plat_mutex_unlock_shared(&lock);
    int idle = 0;
    if (!any || !__atomic_compare_exchange_n(&ahead_busy, &idle, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return 0;
    // bdat_merge_serve keeps several hundred KB of paths on its stack
    if (!plat_thread_spawn(premerge_run, NULL, "bdat premerge", 8u << 20)) {
        __atomic_store_n(&ahead_busy, 0, __ATOMIC_RELEASE);
        return 0;
    }
    return 1;
}

int romfs_mods_premerge_state(void) {
    if (__atomic_load_n(&ahead_busy, __ATOMIC_ACQUIRE)) return 1;
    maybe_reload();
    return ahead_state();
}

int romfs_mods_auto_merge(void) { return cfg_bool("XCX_BDAT_MERGE_AUTO"); }

void romfs_mods_set_auto_merge(int on) {
    cfg_set("XCX_BDAT_MERGE_AUTO", on ? "1" : "0");
    cfg_save();
}

static int ahead_state(void) {
    if (__atomic_load_n(&ahead_busy, __ATOMIC_ACQUIRE)) return 1;
    plat_mutex_lock_shared(&lock);
    int same = nahead > 0 && nahead >= nstarted;
    for (int i = 0; same && i < nahead; i++) {
        char sig[SIG_CAP] = "";
        for (int j = 0; j < nstarted; j++)
            if (ahead[i].key && !strcmp(started[j].key, ahead[i].key)) sig_now(j, sig, sizeof sig);
        same = ahead[i].path && !strcmp(sig, ahead[i].path);
    }
    plat_mutex_unlock_shared(&lock);
    return same ? 2 : 0;
}

int romfs_mods_priority(char (*ids)[65], int cap) {
    if (!mods_root() || cap <= 0) return 0;
    return read_enabled(ids, cap < MOD_CAP ? cap : MOD_CAP);
}

int romfs_mods_move(const char *id, int up) {
    if (!mods_root() || !id) return 0;
    char lines[LINE_CAP][LINE_LEN], tmp[LINE_LEN];
    int n = read_lines(lines, LINE_CAP), i = 0;
    if (n < 0) return 0;
    while (i < n && strcmp(lines[i], id)) i++;
    int j = i;   // the next mod that is on above or below; comment lines (and turned-off places) stay where they are
    do j += up ? -1 : 1; while (j >= 0 && j < n && !is_mod_line(lines[j]));
    if (i == n || j < 0 || j >= n) return 0;
    memcpy(tmp, lines[i], sizeof tmp);
    memcpy(lines[i], lines[j], sizeof tmp);
    memcpy(lines[j], tmp, sizeof tmp);
    int ok = write_lines(lines, n);
    rt_log("romfs mods: %s moved %s in the priority list from the menu%s", id, up ? "up" : "down",
           ok ? "" : " (the list could not be written)");
    __atomic_store_n(&next_check_ms, 0, __ATOMIC_RELAXED);
    return ok;
}
