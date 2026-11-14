// XCX_GRASS_DISTANCE: a multiplier on how far from the camera grass loads and is drawn (1, 1.5, 2 or 3).
//
// Grass comes in layers (15 in Primordia's fields), each with its own range in a 12-byte layer table (+8 the range,
// 10 to 90 m there). Each frame the live grass update (main+0x9fd830) calls main+0x9972c0, which per layer loads the
// grass cells within +-range of the camera on X and Z (main+0x9953a0, or main+0x996150 for the 10 m cells of levels
// 6 and up with a range of 40 or more). Its two range loads are patched (recomp/patches-upd.txt, set "grass"), so the
// cells load further out. The instance builders (main+0x998688 and main+0x99a4c0) then cull each placement against
// three rings derived from a per-type distance R (1.7 R, 2.5 R and 3.3 R); their R loads are patched too, so the rings
// grow with the cells. All of these multiply by the float at patch page +0x100.
//
// The scale alone leaves two pop-ins close to the camera. The fine-cell loader (main+0x996150) keeps a 30 m window of its finest cells around the camera and
// rebuilds them as it moves; its sites read the window from page +0x104 (30 m x scale). And the shortest layer (10 m
// in Primordia, the low fill-in grass) still ends 30 m out at 3x, so the range sites raise every scaled range to at
// least page +0x108 (30 m x scale).
//
// Above 2x the cells no longer fit the grass memory, so set "grasspool" gives the grass system's cell heap 4 MiB more.
// It is always on (the heap is made once, when the grass system starts, so it must already be large when the setting
// is raised later); XCX_GRASS_POOL=0 is only for comparing with the game's size. 4x stops loading grass and then
// crashes in the cell list code (main+0x993ad0) even with a much larger heap, so the setting is clamped to 0.5..3. The
// builders read the scale every frame, so a change applies at once.
#include "cfg.h"
#include "host.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { GRASS_SCALE_PAGE = 0x100, GRASS_WINDOW_PAGE = 0x104, GRASS_MIN_RANGE_PAGE = 0x108 };
_Static_assert(GRASS_MIN_RANGE_PAGE + sizeof(float) <= 0x1000, "the grass floats must fit on the patch page");

static int grass_set = -2;
static unsigned grass_cfg_seen;
static int grass_cfg_seen_valid;
static float grass_logged = -1;

// 1 when this build has the grass patch set (the 2.0.0 update): the menu shows its row only then.
int rt_grass_available(void) {
    if (grass_set == -2) grass_set = rt_patch_set("grass");
    return grass_set >= 0;
}

void rt_grass_update(void) {
    unsigned version = cfg_version();
    if (grass_cfg_seen_valid && version == grass_cfg_seen) return;
    grass_cfg_seen = version;
    grass_cfg_seen_valid = 1;

    uint8_t *page = rt_patch_page();
    if (!rt_grass_available() || !page) return;

    const char *v = cfg_get("XCX_GRASS_DISTANCE");
    float scale = v && *v ? strtof(v, NULL) : 1.0f;
    if (!(scale >= 0.5f)) scale = 1.0f;   // also catches NaN and text that is not a number
    if (scale > 3.0f) scale = 3.0f;
    uint32_t word;
    memcpy(&word, &scale, sizeof word);
    // The window and the minimum range are written before the scale, so a site never sees a new scale with old ones.
    float metres = 30.0f * scale;
    uint32_t mword;
    memcpy(&mword, &metres, sizeof mword);
    __atomic_store_n((uint32_t *)(page + GRASS_WINDOW_PAGE), mword, __ATOMIC_RELEASE);
    __atomic_store_n((uint32_t *)(page + GRASS_MIN_RANGE_PAGE), mword, __ATOMIC_RELEASE);
    __atomic_store_n((uint32_t *)(page + GRASS_SCALE_PAGE), word, __ATOMIC_RELEASE);
    rt_patch_switch(grass_set, scale != 1.0f);
    if (scale != grass_logged) {
        grass_logged = scale;
        rt_log("grass: distance x%.2f (patch page +0x%x holds %#010x), fine window and minimum range %.0f m", scale,
               (unsigned)GRASS_SCALE_PAGE, word, metres);
    }
}

void rt_grass_init(void) {
    grass_cfg_seen_valid = 0;
    rt_grass_update();
}
