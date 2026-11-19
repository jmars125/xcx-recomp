// Merging the BDAT data tables of several game-file mods (runtime/bdat_merge.c; runtime/romfs_mods.c calls it). The
// rules: docs/bdat-merge.md.
#pragma once
#include <stddef.h>
#include <stdint.h>

// One table of a mod as bdat-toolset writes it with -f json (bdat/<file>/<table>.json in the mod's folder).
typedef struct {
    const char *stem;     // the file name without .json: the table's name, or "<8 hex digits>" (its hash)
    const uint8_t *data;
    size_t size;
} BdatJson;

typedef struct {
    const uint8_t *data;  // the whole file; NULL for a mod that has only JSON tables of it
    size_t size;
    const char *name;     // for events: a mod id, "game"
    const BdatJson *json; // tables in place of the file's (or the game's) own; any order
    int njson;
} BdatInput;

enum { BDAT_FILE_MERGED, BDAT_FILE_COPIED, BDAT_FILE_WHOLE };

typedef struct {
    int tables_changed;   // tables at least one mod changes
    int tables_merged;    // of those, merged from two or more mods
    int tables_whole;     // changed by several but taken whole from the top one (an opaque change)
    int priority;         // events: values or rows two mods changed, settled by priority
    int left_out;         // events: a mod's change not used
    int caveats;          // events: used, but may not behave as its mod expects
    int adds_nothing;     // events: a mod whose every changed table a higher mod has alike
    int partly_alike;     // events: a mod some of whose changed tables a higher mod has alike
    int file_mode;        // BDAT_FILE_*: merged; one mod's file as it is (the others can't be used); the top mod's
                          // whole file (it has another list of tables)
    int file_mod;         // the mod of BDAT_FILE_COPIED / BDAT_FILE_WHOLE, else -1
} BdatSummary;

// One line per event: "priority: ...", "left out: <mod>: ...", "caveat: <mod>: ...", "adds nothing: <mod>: ...",
// "partly alike: <mod>: ...".
typedef void (*BdatNote)(void *ctx, const char *line);

// Runs fn(ctx, i) for every i in [0, n), in any order and possibly at the same time; returns when all have returned.
// bdat_merge runs each mod's own work (its JSON tables put in, its file read, its tables compared with the game's)
// through it. This file has no threads of its own, so a caller with threads gives one (runtime/bdat_merge_serve.c);
// NULL, the default, runs them one after another. The result and the events are the same either way.
typedef void (*BdatParallelFor)(int n, void (*fn)(void *ctx, int i), void *ctx);
void bdat_merge_set_parallel(BdatParallelFor f);

// base: the game's own file. mods: their copies of it, highest priority first (romfs-enabled.txt order), n >= 1.
// Returns the merged file (malloc'd, size in *out_n), or NULL with the reason in why when nothing can be merged (the
// game's copy can't be read, or no mod's can). A mod whose copy can't be used is left out and reported.
uint8_t *bdat_merge(const BdatInput *base, const BdatInput *mods, int n, size_t *out_n, BdatSummary *sum,
                    BdatNote note, void *ctx, char *why, size_t why_cap);

// Every table of data rebuilt by the merger's writer, for the round-trip check (tests/bdat_merge_check.c): the
// count of tables whose rebuilt bytes differ from the file's own, or -1 when data isn't a file it reads.
int bdat_rewrite_check(const uint8_t *data, size_t size);

// One enabled mod's files for a data table: its full copy (host path, or NULL) and its bdat-toolset JSON tables.
typedef struct {
    const char *id;
    const char *full;
    const char *const *json;   // host paths of <table>.json files
    int njson;
} BdatModFiles;

// For runtime/romfs_mods.c: the host path of the merged copy of RomFS file key ("bdat/common.bdat") made from the
// enabled mods' files mods[0..n) (highest priority first), the base read from the game's archive on mount. Made once
// per set of inputs and kept in <profile>/bdat-merge/. When it can't be merged and no mod has a full copy, the game's
// own copy is written there and served. 0 when nothing is served: the caller serves the top mod's full copy.
int bdat_merge_serve(const char *mount, const char *key, const BdatModFiles *mods, int n, char *out, size_t cap);

// The merge bdat_merge_serve would make from mods, made now for the next start (the list changed while the game runs;
// runtime/romfs_mods.c, romfs_mods_premerge): kept in the same place, so that start uses it without merging, and the
// files this run serves are kept. What it said goes to the next-start lines and tags (bdat_merge_next_status,
// bdat_merge_next_mod_tag), which bdat_merge_next_clear empties. 1 when a file was made or found.
int bdat_merge_prepare(const char *mount, const char *key, const BdatModFiles *mods, int n);
void bdat_merge_next_clear(void);

// What this run's merges said about mod id, for its row in the Mods tab: "adds nothing" when it adds nothing to any
// file it is in, else its changes left out, caveats and tables alike ("2 left out, 13/22 tables alike"), per file
// ("common: ...; common_ms: ...") when it has lines in several. 0 when there is nothing to say. *flags: BDAT_TAG_*.
enum { BDAT_TAG_LEFT_OUT = 1, BDAT_TAG_CAVEAT = 2, BDAT_TAG_ADDS_NOTHING = 4 };
int bdat_merge_mod_tag(const char *id, char *out, size_t cap, int *flags);
int bdat_merge_next_mod_tag(const char *id, char *out, size_t cap, int *flags);   // of the merges made for the next start

// The lines of this run's merges (next 0) or of those made for the next start (next 1) that are about mod id, read
// from their report files: values it won or lost by priority, its changes left out, its caveats, and whether it adds
// nothing or is partly alike, each as "<file>: <line>" into out (*kinds: as bdat_merge_status, priority lines 0). The
// count, up to cap. For the Mods tab's details under the mod's row.
int bdat_merge_mod_lines(const char *id, int next, char (*out)[320], int *kinds, int cap);

// Line i (from 0) of what the merges did this run, for the F1 Mods tab; 0 past the last. *kind: 0 a summary line,
// 1 a change left out, 2 a caveat, 3 a mod that adds nothing (a higher one has all its changes), 4 a mod partly alike.
int bdat_merge_status(int i, char *out, size_t cap, int *kind);
int bdat_merge_next_status(int i, char *out, size_t cap, int *kind);   // of the merges made for the next start
