// XCX_ANISO: forced anisotropic filtering. The level is read once before the game starts (the menu row is
// after-restart): the game builds its NVN samplers with its own maximum anisotropy, and NVN queries
// (nvnSamplerGetMaxAnisotropy) still answer that value; only the Vulkan sampler of a mipmapped, linearly minified texture
// takes the forced level instead (vk_texture.c sampler_for). 0 keeps every sampler as the game made it; the menu offers
// 2, 4, 8 and 16.
#include "cfg.h"
#include "host.h"
#include "vk.h"

void rt_aniso_init(void) {
    int level = cfg_int("XCX_ANISO");
    if (level < 2) level = 0;   // "0" and "1" keep the game's own anisotropy
    vk_sampler_aniso((float)level);
    if (level) rt_log("aniso: forcing %dx on mipmapped textures", level);
    else rt_log("aniso: the game's own values");
}
