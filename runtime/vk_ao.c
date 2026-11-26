// Ambient occlusion, the menu's "Ambient occlusion" row (XCX_AO = original | light | medium | strong, live). The
// levels say how strong the shading is, not a quality: each is our own, sharper occlusion. There is no Off: the game
// caps its own occlusion at about half of the ambient light, so without it the picture looked the same as Original
// (the user, 2026-11-25); an old "off" reads as Original.
//
// The game's own: after the G-buffer it draws the occlusion at half size into an RG8 target (r = occlusion, 1 = open):
// six depth taps in three pairs around each pixel, rotated by a 4x4 tile, then two plain four-tap blurs (across, then
// down) that do not look at depth, and a small box draw near the player on top. The lighting pass samples that target
// (fragment slot 7, normalised coordinates, linear filter) and lets it darken the ambient light. Few taps and a blur
// that crosses edges give the soft dark halos around characters and grass and the shimmer at a distance.
//
// Light, Medium and Strong put our own occlusion in its place. The game's occlusion draw and its two blurs are skipped
// then (vk_exec_draw.c recognises them by their guest code), and the box draw still runs on top:
// 1. shaders/ao_main.comp: 16 taps on a spiral disc from the scene depth, normal-oriented, a radius in metres (Light: a
//    short one, creases; Medium: the odd taps wide, the even ones short, broad shade and creases both; Strong: wide).
// 2. shaders/ao_blur.comp twice: a depth-aware blur across and down, so occlusion does not bleed over edges.
// Light and Medium work at the game's half size and write the result into the game's target (a blit), where the
// lighting reads it as before. Strong works at the scene size; the result is blitted into the game's target too, and the lighting
// pass's descriptor for that target is pointed at the full-size image instead (vk_ao_swap_image), so the lighting
// reads one occlusion value per pixel.
// The game lets occlusion take at most about half of the ambient light, so on its own a better occlusion barely
// shows. Light, Medium and Strong also halve that floor in the lighting programs (a copy of their constants, vk_ao_floor),
// which is what makes bushes, plants and corners read darker; in direct sunlight the change stays small.
// Original records nothing: no pass, no copy, no skipped draw. The projection comes from the game's occlusion draw
// itself (its fragment c3: the projection in rows 0-3), the scene depth from the G-buffer pass.
// Cost, only while on: three compute passes and a blit at half size (Light, Medium) or at the scene size (Strong), minus the
// game's three skipped draws.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ao_blur_spv.h"
#include "ao_main_spv.h"
#include "ao_merge_spv.h"
#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"

#define SCENE_MIN_W 320u

enum { IMG_A, IMG_B, IMG_F, NIMG };   // occlusion + distance (two of them for the blur), the final RGBA8
enum { MODE_ORIGINAL, MODE_LIGHT, MODE_MEDIUM, MODE_STRONG };   // the menu's order
static const char *const MODE_NAME[] = {"original", "light", "medium", "strong"};

// radius: metres (the even taps), radius2: the odd taps' radius (0 = the same), max_px: the disc radius cap (1440p
// scene pixels). The strength was set by pictures in New LA at night (2026-11-22): at 7 the facade darkens as much as
// with the game's, at 10 visibly more around the kiosk and the planters; 8 is a little stronger than the game's
// without darkening grass-covered ground much.
// The radii come from pictures of ten atlas views in four regions at noon and night (2026-11-23, eight looks side by
// side): a 4 m radius lets shade pool between grass clumps, under bushes and in dense foliage, about 2.4 times what a
// 1 m radius showed where occlusion reads at all. The user ordered the levels by how dark they read: Light the 1 m disc
// (creases only), Medium 4 m on half its taps and 1 m on the rest (broad shade, creases kept), Strong the 4 m disc on
// every tap at full size (the deepest shade).
typedef struct { float radius, radius2, strength, max_px; } Look;
static const Look LOOK_LIGHT = {1.0f, 0.0f, 8.0f, 96.0f};
static const Look LOOK_MEDIUM = {1.0f, 4.0f, 8.0f, 256.0f};
static const Look LOOK_STRONG = {4.0f, 0.0f, 8.0f, 256.0f};
#define FADE_START 800.0f    // metres: the occlusion starts to fade out (the game's ends at about 1.2 km)
#define FADE_END 1200.0f
#define ANGLE_BIAS 0.25f   // the cosine a tap must rise over the surface: flat ground and grass do not occlude
#define BLUR_SHARP 24.0f     // a tap 4% further away than the pixel counts e^-1 as much
// The darkest our occlusion gets (ao_main.comp remaps to lo + (1 - lo) * ao): at full strength, with the lighting's
// floor lowered, dense bushes went black at night and lost their leaves at noon. Picked by pictures of the five
// snapshot spots at noon and 21:00 (2026-11-22): 0.3 kept the leaves but gave back most of the gain, 0.15 keeps both.
#define AO_LO 0.15f

VkImage vk_ao_swap_image;   // Strong, from the run until the frame ends: the game's target the lighting reads

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline main, blur, merge;
    VkSampler near, lin;
    VkImage img[NIMG];
    VmaAllocation mem[NIMG];
    VkImageView view[NIMG];
    uint32_t w, h;          // the size they were made for
    VkFormat work;          // IMG_A and IMG_B: RG16F (r occlusion, g distance), RGBA16F where RG16F is no storage image
    // this frame
    VkExecTarget depth, game;   // the scene depth; the game's occlusion target (Strong's merge)
    int gbuf, ran, merged;
    int other;              // a second occlusion chain this frame (another view): the game's own for the rest of it
    int retry;              // the setting changed since a failure: try the failed size again
    // the setting
    unsigned seen;
    int mode;
    uint64_t frames;
} K = {.seen = ~0u};

static int depth32(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
static void images_free(void);

static void read_setting(void) {
    unsigned v = cfg_version();
    if (v == K.seen) return;
    K.seen = v;
    const char *m = cfg_value("XCX_AO");
    int mode = !strcmp(m, "light")    ? MODE_LIGHT
               : !strcmp(m, "medium") ? MODE_MEDIUM
               : !strcmp(m, "strong") ? MODE_STRONG
                                      : MODE_ORIGINAL;
    if (mode == K.mode) return;
    K.retry = 1;
    rt_log("ambient occlusion: %s", MODE_NAME[mode]);
    if (K.img[0] && mode == MODE_ORIGINAL) images_free();   // between levels images_for keeps them, or remakes them at a new size
    K.mode = mode, K.frames = 0;   // the first frame of each mode is logged
}

int vk_ao_active(void) {
    read_setting();
    return K.mode != MODE_ORIGINAL && !(K.tried && !K.ok);
}

// The lighting's floor: the lighting programs apply the occlusion as k + (1 - k) * ao with k = (a per-pixel term) *
// c3[7].y (0.5 in every area checked), so the game lets occlusion take at most about half of the ambient light. Light,
// Medium and Strong bind a copy of that block with [7].y times this (vk_exec_draw.c light_copy), so our occlusion can darken
// more of it; 1 = the game's floor (Original, or before our occlusion ran this frame). Half: removing the floor
// altogether (pictures, 2026-11-22) added little more, and the per-pixel term it scales is not known everywhere.
#define FLOOR_SCALE 0.5f
float vk_ao_floor(void) {
    if (!vk_ao_active() || !K.ran || K.other) return 1;
    return FLOOR_SCALE;
}

static VkPipeline make_pipeline(VkCtx *c, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = K.layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(c->device, m, NULL);
    return p;
}

static int init(void) {
    if (K.tried) return K.ok;
    K.tried = 1;
    VkCtx *c = vk_ctx();
    K.push = vk_pipeline_push_fn();
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_R8G8_UNORM, &fp);
    if (!K.push || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        rt_log("ambient occlusion: the setting is off for this run (no push descriptors or no blit into RG8)");
        return 0;
    }
    vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_R16G16_SFLOAT, &fp);
    K.work = (fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) ==
                     (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
                 ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_R16G16B16A16_SFLOAT;
    // 0 the scene depth, 2 a pass's output, 3 its input, 6 the final image
    static const uint32_t bind[4] = {0, 2, 3, 6};
    VkDescriptorSetLayoutBinding b[4];
    for (uint32_t i = 0; i < 4; i++)
        b[i] = (VkDescriptorSetLayoutBinding){bind[i], bind[i] == 2 || bind[i] == 6 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                                                    : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                              1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &K.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &K.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &K.layout) != VK_SUCCESS) goto fail;
    if (vkCreateSampler(c->device, &si, NULL, &K.near) != VK_SUCCESS) goto fail;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    if (vkCreateSampler(c->device, &si, NULL, &K.lin) != VK_SUCCESS) goto fail;
    K.main = make_pipeline(c, ao_main_spv, sizeof ao_main_spv);
    K.blur = make_pipeline(c, ao_blur_spv, sizeof ao_blur_spv);
    K.merge = make_pipeline(c, ao_merge_spv, sizeof ao_merge_spv);
    if (!K.main || !K.blur || !K.merge) goto fail;
    return K.ok = 1;
fail:
    rt_log("ambient occlusion: its passes could not be set up: the setting is off for this run");
    if (K.main) vkDestroyPipeline(c->device, K.main, NULL);
    if (K.blur) vkDestroyPipeline(c->device, K.blur, NULL);
    if (K.merge) vkDestroyPipeline(c->device, K.merge, NULL);
    if (K.near) vkDestroySampler(c->device, K.near, NULL);
    if (K.lin) vkDestroySampler(c->device, K.lin, NULL);
    if (K.layout) vkDestroyPipelineLayout(c->device, K.layout, NULL);
    if (K.dsl) vkDestroyDescriptorSetLayout(c->device, K.dsl, NULL);
    K.main = K.blur = K.merge = VK_NULL_HANDLE, K.near = K.lin = VK_NULL_HANDLE, K.layout = VK_NULL_HANDLE, K.dsl = VK_NULL_HANDLE;
    return 0;
}

// Frames in flight may still read the old images: they go through the back end's deferred destruction.
static void images_free(void) {
    for (int i = 0; i < NIMG; i++) {
        if (K.view[i]) vk_defer_view(K.view[i]);
        if (K.img[i]) vk_defer_image(K.img[i], K.mem[i]);
        K.view[i] = VK_NULL_HANDLE, K.img[i] = VK_NULL_HANDLE;
    }
    K.w = K.h = 0;
}

static int images_for(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (K.w == w && K.h == h && K.img[0]) return 1;
    if (K.img[0]) images_free();
    VkCtx *c = vk_ctx();
    VkImageMemoryBarrier2 ib[NIMG];
    for (int i = 0; i < NIMG; i++) {
        VkFormat f = i == IMG_F ? VK_FORMAT_R8G8B8A8_UNORM : K.work;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = f, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ii, &ai, &K.img[i], &K.mem[i], NULL) != VK_SUCCESS) goto fail;
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = K.img[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(c->device, &vi, NULL, &K.view[i]) != VK_SUCCESS) goto fail;
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = K.img[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    }
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = NIMG,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    K.w = w, K.h = h;
    rt_log("ambient occlusion: work images made (%ux%u)", w, h);
    return 1;
fail:
    images_free();
    return 0;
}

static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// One pass: the depth view, an output (binding 2), an input (binding 3) and the final image (binding 6); every
// binding the shaders declare is pushed, also the ones this pass does not touch.
static void dispatch(VkCommandBuffer cb, VkPipeline p, VkImageView depth, VkImageView out, VkImageView in,
                     VkSampler in_samp, const float pc[16]) {
    const uint32_t bind[4] = {0, 2, 3, 6};
    const VkImageView v[4] = {depth, out, in, K.view[IMG_F]};
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet wr[4];
    for (uint32_t i = 0; i < 4; i++) {
        int storage = bind[i] == 2 || bind[i] == 6;
        ii[i] = (VkDescriptorImageInfo){storage ? VK_NULL_HANDLE : bind[i] == 3 ? in_samp : K.near, v[i],
                                        VK_IMAGE_LAYOUT_GENERAL};
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = bind[i],
                                       .descriptorCount = 1,
                                       .descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                                 : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .pImageInfo = &ii[i]};
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    rec_push(K.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, K.layout, 0, 4, wr);
    vkCmdPushConstants(cb, K.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 64, pc);
    vkCmdDispatch(cb, (K.w + 7) / 8, (K.h + 7) / 8, 1);
    full_barrier(cb);
}

static uint32_t failed_w, failed_h;   // a size whose images could not be made: not tried again every frame

// Ours in place of the game's, at its occlusion draw (no pass is open here). 1: done, the game's draws are skipped.
static int run(VkExec *ex, const VkExecTarget *t, const float proj[4]) {
    VkCommandBuffer cb = ex->cb;
    vk_exec_barrier(ex);   // the G-buffer and what came before are done
    const VkExecTarget *z = &K.depth;
    uint32_t w = K.mode == MODE_STRONG ? z->width : t->width, h = K.mode == MODE_STRONG ? z->height : t->height;
    if (K.retry) failed_w = failed_h = 0, K.retry = 0;
    if (w == failed_w && h == failed_h) return 0;
    if (!images_for(cb, w, h)) {
        rt_log("ambient occlusion: its work images could not be made (%ux%u): the game's at this size", w, h);
        failed_w = w, failed_h = h;
        return 0;
    }
    VkImageView zv = vk_exec_target_view(z->img, z->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, z->level,
                                         z->base_layer, 1, 0);
    if (!zv) return 0;
    float scale = (float)w / (float)z->width;   // output pixels per scene pixel
    const Look *L = K.mode == MODE_STRONG ? &LOOK_STRONG : K.mode == MODE_MEDIUM ? &LOOK_MEDIUM : &LOOK_LIGHT;
    float pc[16] = {proj[0], proj[1], proj[2], proj[3], L->radius, L->strength, FADE_START, FADE_END,
                    (float)w, (float)h, L->max_px * (float)z->height / 1440.0f * scale, ANGLE_BIAS,
                    0, 0, 0, BLUR_SHARP};
    pc[12] = AO_LO, pc[13] = L->radius2;
    dispatch(cb, K.main, zv, K.view[IMG_A], K.view[IMG_B], K.near, pc);
    pc[12] = 1, pc[13] = 0, pc[14] = 0;
    dispatch(cb, K.blur, zv, K.view[IMG_B], K.view[IMG_A], K.near, pc);
    pc[12] = 0, pc[13] = 1, pc[14] = 1;
    dispatch(cb, K.blur, zv, K.view[IMG_A], K.view[IMG_B], K.near, pc);
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, t->base_layer, 1},
                     .dstOffsets = {{0, 0, 0}, {(int32_t)t->width, (int32_t)t->height, 1}}};
    vkCmdBlitImage(cb, K.img[IMG_F], VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                   w == t->width ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    full_barrier(cb);
    ex->recorded += 17;   // 3 x (bind, push, constants, dispatch, barrier) + blit + barrier
    vk_exec_draw_new_cb();   // our compute pipeline and descriptors: the next draw binds everything again
    if (++K.frames == 1 || K.frames % 3600 == 0)
        rt_log("ambient occlusion: %llu frames (%s, %ux%u, scene %ux%u, projection %.4f %.4f %.4f %.4f)",
               (unsigned long long)K.frames, MODE_NAME[K.mode], w, h, z->width, z->height, proj[0], proj[1], proj[2],
               proj[3]);
    return 1;
}

// Strong: the first pass begin after the game's occlusion chain (no target of the occlusion's format and size; the box
// draw that writes the mask in g came before), so before the lighting pass. The game's g into the full-size image, then
// the lighting's descriptor for the game's target points at it (vk_exec_draw.c, vk_ao_swap).
static void merge(VkExec *ex) {
    for (uint32_t i = 0; i < ex->pass_ncolor; i++)
        if (ex->color[i].image && ex->color[i].format == K.game.format && ex->color[i].width == K.game.width &&
            ex->color[i].height == K.game.height)
            return;   // still the chain (the blur passes, the box draw)
    K.merged = 1;
    const VkExecTarget *t = &K.game;
    VkImageView gv = vk_exec_target_view(t->img, t->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, t->level,
                                         t->base_layer, 1, 0);
    if (!gv) return;   // no swap: the lighting reads the game's target, which holds our occlusion at half size
    vk_exec_barrier(ex);
    float pc[16] = {0, 0, 0, 0, 0, 0, 0, 0, (float)K.w, (float)K.h};
    dispatch(ex->cb, K.merge, gv, K.view[IMG_A], gv, K.lin, pc);
    ex->recorded += 5;
    vk_exec_draw_new_cb();
    vk_ao_swap_image = t->image;
}

// Each pass begin (vk_exec.c vk_exec_pass_begin): the G-buffer, found as vk_dlss.c vk_dlss_pass and vk_bokeh.c
// vk_bokeh_pass find it (a change to one belongs in the others). The G-buffer pass gives the scene depth (five or more colour targets
// with a 32-bit depth of the scene size, as vk_bokeh.c and vk_dlss.c find it).
void vk_ao_pass(VkExec *ex) {
    if (!vk_ao_active()) return;
    if (K.ran && K.mode == MODE_STRONG && !K.merged) merge(ex);
    const VkExecTarget *z = &ex->depth;
    if (ex->pass_ncolor >= 5 && z->image && depth32(z->format) && z->width >= SCENE_MIN_W && z->samples <= 1)
        K.depth = *z, K.gbuf = 1;
}

// A draw of the game's occlusion chain (vk_exec_draw.c, after its pass began): kind 1 the occlusion, 2 and 3 its
// blurs. proj: the occlusion draw's projection (a, b of the depth column, x and y scale). 1: skip the draw.
int vk_ao_draw(VkExec *ex, int kind, const float proj[4]) {
    if (!vk_ao_active()) return 0;
    const VkExecTarget *t = &ex->color[0];
    if (kind != 1)   // a blur: skipped once ours ran, when it draws into a target like the game's occlusion target
        return K.ran && !K.other && ex->pass_ncolor == 1 && t->format == K.game.format && t->width == K.game.width &&
               t->height == K.game.height;
    if (K.ran) {   // a second occlusion this frame (another view): it keeps the game's own blurs and floor
        K.other = 1;
        return 0;
    }
    if (ex->pass_ncolor != 1 || !t->image || t->format != VK_FORMAT_R8G8_UNORM || t->is3d) return 0;
    if (!K.gbuf || !init() || !(proj[1] < 0) || !(proj[2] > 0) || !(proj[3] > 0)) return 0;
    int was_in_pass = ex->in_pass;
    if (was_in_pass) vk_exec_pass_end(ex);
    K.game = *t;
    K.ran = run(ex, t, proj);
    if (!K.ran && was_in_pass) {   // ours could not run: the game's draw goes on, in its pass as prepare_draw opened it
        ex->dirty |= VK_EXEC_DIRTY_VIEWPORT | VK_EXEC_DIRTY_SCISSOR;
        vk_exec_pass_begin(ex);
    }
    return K.ran;
}

// The lighting pass's descriptor for the game's occlusion target, while Strong ran this frame: our full-size image.
void vk_ao_swap(VkDescriptorImageInfo *d) {
    if (K.view[IMG_F]) d->imageView = K.view[IMG_F], d->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
}

void vk_ao_frame(void) { K.gbuf = K.ran = K.merged = K.other = 0, vk_ao_swap_image = VK_NULL_HANDLE; }
