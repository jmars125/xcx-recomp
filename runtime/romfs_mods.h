// Loose RomFS mod files in place of sts.ard entries, switchable while the game runs (runtime/romfs_mods.c; the game
// side is recomp/romfs_mods.py).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Called by the generated C where the archive device loads its list of declined paths (prefixes, NUL-terminated,
// ended by an empty string) for the path operation on path (a guest string): a list holding just that path when an
// enabled mod has a file for it, so the game opens rom:/<path> instead; else list unchanged.
uint64_t romfs_mod_declines(uint64_t list, uint64_t path);

// The room fs_host.c's open_file gives romfs_mod_file's path (its PATH_CAP): a data table is claimed only when the
// file served for it fits, so a claimed path is always one the open can take.
#define ROMFS_MOD_PATH_CAP 1100

// The host path of an enabled mod's file for a RomFS path ("rom:/chr/..." or "chr/..."), copied to out (may be NULL);
// 0 when none, or when the path doesn't fit in cap.
int romfs_mod_file(const char *path, char *out, size_t cap);

// The mods folder (XCX_MOD_DIR, else "mods" beside xcx.exe); NULL when there is none.
const char *romfs_mods_dir(void);
// Rebuilds the file map at the next lookup, for a mod whose files changed (an update) while its id stayed listed.
void romfs_mods_reload(void);
// Turns a mod on or off; 1 when the list was written. A mod turned off and on again while the game runs goes back
// where it was; any other goes last in romfs-enabled.txt, at the lowest priority. The game picks the change up within
// CHECK_MS of romfs_mods.c.
int romfs_mods_set(const char *id, int on);
// Looks at every mod file of the data tables the game read, once for all of them, for romfs_mods_tables_changed,
// romfs_mod_changed and romfs_mods_premerge_state, which compare what was looked at last. The Mods tab calls it at each
// refresh; otherwise mod files are looked at only at start-up, after a list change or an update, and by a merge made
// ahead, so with the menu closed and nothing changing none is touched.
void romfs_mods_check_files(void);
// 1 when romfs-enabled.txt is no longer as it was when the game read its data tables (romfs_mods_tables_changed can
// also be 1 for an updated mod with the list unchanged).
int romfs_mods_list_changed(void);
// 0 when the game is at the title screen (it loaded the title's scene, at boot or after Return to Title, and no world
// since), else 1: in the world (a save, or a new game) or not known, from the files it asks the archive for
// (romfs_mod_declines). Restart now in the Mods tab asks first unless the game is known to be at the title: if the
// guess from file names is ever wrong, it asks once too often rather than skipping the question in the world.
int romfs_game_in_world(void);
// How mod id's data tables differ from what the game read at start-up: 1 its files were updated since (sizes or
// times, as last looked at: romfs_mods_check_files), 2 it was turned on since, 0 neither (or it has none). For the mark
// on its Mod priority row.
int romfs_mod_changed(const char *id);
// Writes romfs-enabled.txt back as it was when the game read its data tables; 1 when written.
int romfs_mods_restore_list(void);
// 1 when the mods that data tables the game read at start-up would be made from (which mods, in which order, and
// their files' sizes and times, as last looked at: romfs_mods_check_files) are no longer the ones they were made from:
// the game reads those tables only at start-up, so it needs a restart.
int romfs_mods_tables_changed(void);
// Merges the data tables the game read at start-up from the mods as they are now, on a thread of its own, so the next
// start finds them made (bdat_merge_prepare; the F1 Mods tab's Merge now). 1 when it started; 0 when one is running
// or the game has read no data table yet.
int romfs_mods_premerge(void);
// 0: no merge made ahead for the mods as they are now; 1: one running; 2: made for them (what it said:
// bdat_merge_next_status, bdat_merge_next_mod_tag).
int romfs_mods_premerge_state(void);
// The Mods tab's "merge automatically" (xcx.ini bdat_merge_auto): with it on, a list that stays changed for
// AUTO_MS (romfs_mods.c) is merged ahead as by romfs_mods_premerge, without Merge now. Set saves xcx.ini.
int romfs_mods_auto_merge(void);
void romfs_mods_set_auto_merge(int on);
// The ids of romfs-enabled.txt in order, the top (winning) mod first, into ids; the count.
int romfs_mods_priority(char (*ids)[65], int cap);
// Swaps the mod with the one above it (up) or below it in romfs-enabled.txt; 1 when the list was written, 0 when it
// could not be (the log says why), -1 when there was nothing to move (the mod is no longer listed, or no mod is on
// beyond it). Files follow
// the new order the next time the game loads them, as with romfs_mods_set; the game reads its data tables at
// start-up, so a new order for those applies at the next start.
int romfs_mods_move(const char *id, int up);
