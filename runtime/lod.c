// XCX_LOD_DETAIL replaces the float that the patched site (recomp/patches-upd.txt) feeds into the game's object-LOD
// selection, ahead of the draw it belongs to: Original keeps the game's own value, Extended and Maximum substitute
// -8.0 and -16.0, which bring distant geometry and textures in sharper. The game takes this value while it builds
// its render state at startup: a value set on the title screen or while playing changes nothing (measured on the
// 2.0.0 build), one set before the start does. The settings row is therefore after-restart.
#include "cfg.h"
#include "host.h"
#include <stdint.h>
#include <string.h>

enum { LOD_BIAS_PAGE = 0x78 };
_Static_assert(LOD_BIAS_PAGE + sizeof(float) <= 0x1000, "the LOD value must fit on the patch page");

static const struct { const char *name; float bias; } levels[] = {
    {"extended", -8.0f},
    {"maximum", -16.0f},
};

static int lod_set = -2;
static unsigned lod_cfg_seen;
static int lod_cfg_seen_valid;
static int lod_logged_force = -1;   // the state the log last reported
static float lod_logged_bias;

void rt_lod_update(void) {
    unsigned version = cfg_version();
    if (lod_cfg_seen_valid && version == lod_cfg_seen) return;
    lod_cfg_seen = version;
    lod_cfg_seen_valid = 1;

    if (lod_set == -2) lod_set = rt_patch_set("lod");
    uint8_t *page = rt_patch_page();
    if (lod_set < 0 || !page) return;

    const char *level = cfg_get("XCX_LOD_DETAIL");
    float bias = 0;
    const char *mode = "original";
    if (!cfg_is_set("XCX_LOD_DETAIL")) {   // keep the earlier patch switch working until a detail mode is explicitly set
        const char *legacy = cfg_get("XCX_LOD_FORCE");
        if (legacy && *legacy && strcmp(legacy, "0")) bias = -1.0f, mode = "legacy";
    }
    for (size_t i = 0; i < sizeof levels / sizeof *levels; i++)
        if (level && !strcmp(level, levels[i].name)) bias = levels[i].bias, mode = levels[i].name;
    uint32_t word;
    memcpy(&word, &bias, sizeof word);
    __atomic_store_n((uint32_t *)(page + LOD_BIAS_PAGE), word, __ATOMIC_RELEASE);
    int force = bias != 0;
    rt_patch_switch(lod_set, force);
    // The patches line at boot only says whether the set is on; a run that is checked for an effect also needs the
    // value the patched site loads, as the environment of a test can override xcx.ini without saying so.
    if (force != lod_logged_force || bias != lod_logged_bias) {
        lod_logged_force = force;
        lod_logged_bias = bias;
        rt_log("lod: %s, bias %.0f, patch page +0x%x holds %#010x", mode, bias, (unsigned)LOD_BIAS_PAGE, word);
    }
}

void rt_lod_init(void) {
    lod_cfg_seen_valid = 0;
    rt_lod_update();
}
