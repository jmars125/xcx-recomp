#ifndef XCX_SHADER_PREPARE_H
#define XCX_SHADER_PREPARE_H
// The preparation of the setup's shader seed while the game runs, and clearing the shader caches (runtime/gfx_shader.c,
// section "the seed, prepared while the game runs"). Plain C declarations, also for the menu (runtime/ui/menu.cpp).
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start-up, after the prewarm: when a seed is there but shader-prepared.bin is not current (the setup did not prepare
// it, or another translator or graphics card), translates it on a background thread while the game runs
// (XCX_SHADER_PREPARE, on by default).
void gfx_shader_prepare_start(void);
// While that runs: 1 and the records done and in all so far; else 0.
int gfx_shader_prepare_progress(int64_t *done, int64_t *total);
// The menu's "Prepare shaders now": the same pass, also with shader_prepare = 0. Returns one of these (NO_SEED: the
// setup left no seed; UNAVAILABLE: no recording, as with XCX_PIPE_SYNC, no free translator process, or no thread).
enum { GFX_PREPARE_STARTED, GFX_PREPARE_RUNNING, GFX_PREPARE_DONE, GFX_PREPARE_NO_SEED, GFX_PREPARE_UNAVAILABLE };
int gfx_shader_prepare_now(void);
// The folder of the setup's seed and the prepared list: beside xcx.exe. 0 when the path does not fit cap.
int gfx_shader_seed_dir(char *out, size_t cap);
// 1 when dir's shader-prepared.bin was made under the translator identity in use and its translations are there
// (xcx.exe --warm-shaders checks it: a list for another card or translator needs --prepare-shaders).
int gfx_shader_prepared_current(const char *dir);
// The menu's "Clear shader cache": on 1 marks the caches to be deleted at the next start (0 takes the mark back); 1 on
// success. _pending: the mark is there. _at_start: rt_boot before anything reads the caches, and xcx.exe
// --prepare-shaders / --warm-shaders (vk_pipeline.c), so a setup run after a press prepares into the cleared caches.
int gfx_shader_cache_clear_request(int on);
int gfx_shader_cache_clear_pending(void);
void gfx_shader_cache_clear_at_start(void);

#ifdef __cplusplus
}
#endif
#endif
