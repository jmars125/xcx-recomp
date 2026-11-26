// Depth of field in play, the menu's "Depth of field" row (XCX_BOKEH = off | horizon | vista | portrait, live): our own
// depth of field that keeps the distance soft while the player runs around, with disc-shaped bokeh on far highlights.
// The game's own (cutscene) depth of field is left as it is. Off records nothing.
//
// Where it runs: at the point DLSS uses (vk_dlss.c, "where DLSS runs in XCX's frame"). The G-buffer pass gives the
// light target (R11G11B10F) and the scene depth (D32F_S8); the scene's first draws give the camera block (vertex c3,
// rows 3-6 the OpenGL projection); bloom and the tonemap then finish in the light target, and the game's scaling pass
// (FXAA into the output-sized RGBA8 target) reads it. As that pass begins, before DLSS and before FXAA, the light
// target holds the finished, gamma-encoded picture, and the depth still holds the frame's scene:
// 1. shaders/bokeh_down.comp: the picture at half size in linear light, with each pixel's blur radius from its distance
//    (nothing below the start distance, the full radius past the end distance).
// 2. shaders/bokeh_blur.comp: a disc gather at half size that keeps sharp near objects out of the blur behind them.
// 3. shaders/bokeh_comp.comp: back to full size into an RGBA16F image, the game's own value wherever the radius is
//    under half a pixel; a 1:1 blit writes it into the light target (the back end's images are not storage images).
// The sky (where nothing wrote depth: space, the moons, the stars) stays sharp unless XCX_BOKEH_SKY = 1; far land next to
// it still spreads its blur over it, and takes it in as the background at its own edge, so silhouettes stay soft.
// The interface is drawn after this into the output target, so it is never blurred; markers the game draws into the
// scene itself are. Cutscenes get it too, on top of the game's own depth of field there, which is left as it is.
// The light target's values are taken as gamma 2.2: vk_hdr.c decodes the same image with the piecewise sRGB curve. 2.2
// is the approximation chosen here on purpose: only blurred pixels go through it, unblurred ones come back bit for bit.
// Cost, only while on: three compute passes (two at half size) and one copy at the scene size per frame; with the
// clouds kept apart from the sky (Sky or Clouds at Sharp), two more copies of the light target and one render pass
// ended and begun again at the first cloud draw.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bokeh_blur_spv.h"
#include "bokeh_comp_spv.h"
#include "bokeh_down_spv.h"
#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"

#define CAM_SIZE 0x300u   // the scene camera block, vertex stage c3 (vk_dlss.c)
#define SCENE_MIN_W 320u

// The work images: pass 1's half-size picture and pass 2's blur of it, pass 3's full-size result, the light target just
// before and just after the clouds are drawn (its own format, copied), and at half size the cloud mask (pass 1) and the
// soft clouds (pass 2).
enum { IMG_HALF, IMG_BLUR, IMG_FULL, IMG_BEFORE, IMG_AFTER, IMG_MASK, IMG_SOFT, NIMG };

typedef struct { float start, end, size, boost; } Look;   // metres, metres, radius in 1440p pixels, highlight weight

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline down, blur, comp;
    VkSampler lin, near;
    VkImage img[NIMG];
    VmaAllocation mem[NIMG];
    VkImageView view[NIMG];
    uint32_t w, h;           // the scene size they were made for
    int retry;               // the setting changed since a failure: try the failed size again
    int cloud_imgs;          // the cloud images (IMG_BEFORE .. IMG_SOFT) are full size; else 1x1 stand-ins, never used
    VkFormat mask_fmt;       // IMG_MASK: R16F, RGBA16F where R16F is no storage image
    // this frame
    VkExecTarget color, depth;
    int gbuf, post, done, cam_ok;
    int cl;                  // the clouds this frame: 0 not met, 1 copied before them, 2 drawing them, 3 copied after
    float a, b;              // the projection's depth row
    // the setting
    unsigned seen;
    int mode;                // MODE_*
    int sky;                 // XCX_BOKEH_SKY: the sky blurs too
    int clouds;              // XCX_BOKEH_CLOUDS: the clouds blur too
    Look look;
    uint64_t frames, mask_frames;
} K = {.seen = ~0u};

enum { MODE_OFF, MODE_HORIZON, MODE_VISTA, MODE_PORTRAIT };   // the menu's order
static const char *const MODE_NAME[] = {"off", "horizon", "vista", "portrait"};
// Horizon: only the horizon softens. Vista: sharp to a few hundred metres, soft by the horizon. Portrait: sharp to
// about a hundred metres, larger discs. (A stronger step from 12 m to 120 m was too much to play with: dropped.)
static const Look LOOKS[] = {{0, 0, 0, 0}, {800, 3000, 8, 6}, {300, 1500, 10, 6}, {100, 600, 12, 8}};

static int depth32(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }

static void images_free(void);

static void read_setting(void) {
    unsigned v = cfg_version();
    if (v == K.seen) return;
    K.seen = v;
    K.retry = 1;   // a size whose images failed is tried again (run, vk_bokeh_clouds)
    const char *m = cfg_value("XCX_BOKEH");
    int mode = !strcmp(m, "horizon") ? MODE_HORIZON : !strcmp(m, "vista") ? MODE_VISTA
             : !strcmp(m, "portrait") ? MODE_PORTRAIT
                                                      : MODE_OFF;
    int sky = cfg_bool("XCX_BOKEH_SKY");
    if (sky != K.sky) rt_log("bokeh: the sky %s", sky ? "blurs too" : "stays sharp");
    K.sky = sky;
    int clouds = cfg_bool("XCX_BOKEH_CLOUDS");
    if (clouds != K.clouds) rt_log("bokeh: the clouds %s", clouds ? "blur too" : "stay sharp");
    K.clouds = clouds;
    if (mode == K.mode) return;
    const Look *l = &LOOKS[mode];
    if (mode) rt_log("bokeh: %s (start %.0f m, full at %.0f m, radius %.0f px at 1440p)", MODE_NAME[mode], l->start,
                     l->end, l->size);
    else rt_log("bokeh: off");
    if (!mode && K.img[0]) images_free();   // off: the work images (at 1440p about 83 MB, 44 MB without the cloud images) go back, made again when on
    K.mode = mode, K.look = *l;
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
    vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &fp);
    if (!K.push || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        rt_log("bokeh: off for this run (no push descriptors or no blit into the light target's format)");
        return 0;
    }
    vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_R16_SFLOAT, &fp);
    K.mask_fmt = (fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) ==
                         (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                     ? VK_FORMAT_R16_SFLOAT : VK_FORMAT_R16G16B16A16_SFLOAT;
    // 0 the light target, 1 the scene depth, 2 the pass's output, 3 its input from the pass before, 4 and 5 the light
    // target before and after the clouds, 6 a second output (pass 1: the cloud mask, pass 2: the soft clouds), 7 its input
    VkDescriptorSetLayoutBinding b[8];
    for (uint32_t i = 0; i < 8; i++)
        b[i] = (VkDescriptorSetLayoutBinding){i, i == 2 || i == 6 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                                  : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                              1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 8, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 56};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &K.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &K.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &K.layout) != VK_SUCCESS) goto fail;
    if (vkCreateSampler(c->device, &si, NULL, &K.lin) != VK_SUCCESS) goto fail;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(c->device, &si, NULL, &K.near) != VK_SUCCESS) goto fail;
    K.down = make_pipeline(c, bokeh_down_spv, sizeof bokeh_down_spv);
    K.blur = make_pipeline(c, bokeh_blur_spv, sizeof bokeh_blur_spv);
    K.comp = make_pipeline(c, bokeh_comp_spv, sizeof bokeh_comp_spv);
    if (!K.down || !K.blur || !K.comp) goto fail;
    return K.ok = 1;
fail:
    rt_log("bokeh: its passes could not be set up: off for this run");
    if (K.down) vkDestroyPipeline(c->device, K.down, NULL);
    if (K.blur) vkDestroyPipeline(c->device, K.blur, NULL);
    if (K.comp) vkDestroyPipeline(c->device, K.comp, NULL);
    if (K.lin) vkDestroySampler(c->device, K.lin, NULL);
    if (K.near) vkDestroySampler(c->device, K.near, NULL);
    if (K.layout) vkDestroyPipelineLayout(c->device, K.layout, NULL);
    if (K.dsl) vkDestroyDescriptorSetLayout(c->device, K.dsl, NULL);
    K.down = K.blur = K.comp = VK_NULL_HANDLE, K.lin = K.near = VK_NULL_HANDLE;
    K.layout = VK_NULL_HANDLE, K.dsl = VK_NULL_HANDLE;
    return 0;
}

// A new scene size (the menu's render scale, the game's dynamic resolution): the old images may still be read by frames
// in flight, so they go through the back end's deferred destruction instead of a wait for the whole device.
static void images_free(void) {
    for (int i = 0; i < NIMG; i++) {
        if (K.view[i]) vk_defer_view(K.view[i]);
        if (K.img[i]) vk_defer_image(K.img[i], K.mem[i]);
        K.view[i] = VK_NULL_HANDLE, K.img[i] = VK_NULL_HANDLE;
    }
    K.w = K.h = 0;
}

// clouds: the setting keeps the clouds apart (clouds_apart: Sky or Clouds at Sharp): the copies and the mask and
// soft-cloud images at full size. Without, the copies are not made and the mask and soft clouds are 1x1 stand-ins: every
// binding stays valid, and the shaders neither write nor read them while the mask is off (about 39 MB less at 1440p).
// The set follows the setting, not the frame: a frame without clouds keeps it, so it is made once, not every frame.
static int clouds_apart(void) { return !K.sky || !K.clouds; }
static int images_for(VkCommandBuffer cb, uint32_t w, uint32_t h, VkFormat light, int clouds) {
    if (K.w == w && K.h == h && K.img[0] && K.cloud_imgs == clouds) return 1;
    if (K.img[0]) images_free();
    VkCtx *c = vk_ctx();
    uint32_t hw = (w + 1) / 2, hh = (h + 1) / 2;
    VkImageMemoryBarrier2 ib[NIMG];
    uint32_t nb = 0;
    for (int i = 0; i < NIMG; i++) {
        int copy = i == IMG_BEFORE || i == IMG_AFTER, half = i != IMG_FULL && !copy;
        int stand_in = !clouds && (i == IMG_MASK || i == IMG_SOFT);
        if (copy && !clouds) continue;
        VkFormat f = copy ? light : i == IMG_MASK ? K.mask_fmt : VK_FORMAT_R16G16B16A16_SFLOAT;
        uint32_t iw = stand_in ? 1 : half ? hw : w, ih = stand_in ? 1 : half ? hh : h;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = f, .extent = {iw, ih, 1}, .mipLevels = 1,
                                .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                         (copy ? VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                               : VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ii, &ai, &K.img[i], &K.mem[i], NULL) != VK_SUCCESS) goto fail;
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = K.img[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(c->device, &vi, NULL, &K.view[i]) != VK_SUCCESS) goto fail;
        ib[nb++] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = K.img[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    }
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = nb,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    K.w = w, K.h = h, K.cloud_imgs = clouds;
    rt_log("bokeh: work images made (%ux%u, cloud images %s)", w, h, clouds ? "full size" : "1x1");
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

// One pass: v[i] is binding i's view (VK_NULL_HANDLE: not bound; bindings 2 and 6 are storage images, the rest sampled
// through s[i]).
static void dispatch(VkCommandBuffer cb, VkPipeline p, const VkImageView v[8], const VkSampler s[8], const float pc[14],
                     uint32_t w, uint32_t h) {
    VkDescriptorImageInfo ii[8];
    VkWriteDescriptorSet wr[8];
    uint32_t n = 0;
    for (uint32_t i = 0; i < 8; i++) {
        if (!v[i]) continue;
        int storage = i == 2 || i == 6;
        ii[i] = (VkDescriptorImageInfo){storage ? VK_NULL_HANDLE : s[i], v[i], VK_IMAGE_LAYOUT_GENERAL};
        wr[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i,
                                         .descriptorCount = 1,
                                         .descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                                   : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         .pImageInfo = &ii[i]};
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    rec_push(K.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, K.layout, 0, n, wr);
    vkCmdPushConstants(cb, K.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 56, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(cb);
}

// The light target into a copy of it (no pass is open): before and after the clouds.
static void copy_light(VkCommandBuffer cb, VkImage dst) {
    const VkExecTarget *t = &K.color;
    VkImageCopy r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, t->base_layer, 1},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .extent = {t->width, t->height, 1}};
    vkCmdCopyImage(cb, t->image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    full_barrier(cb);
}

static uint32_t failed_w, failed_h;   // a size whose images could not be made: not tried again every frame, only after
                                      // a change of the setting (read_setting)

static void run(VkExec *ex) {
    const VkExecTarget *t = &K.color, *z = &K.depth;
    uint32_t w = t->width, h = t->height, hw = (w + 1) / 2, hh = (h + 1) / 2;
    VkCommandBuffer cb = ex->cb;
    if (!init()) return;
    vk_exec_barrier(ex);   // what drew the picture is done (no pass is open here)
    if (K.retry) failed_w = failed_h = 0, K.retry = 0;
    if (w == failed_w && h == failed_h) return;
    if (!images_for(cb, w, h, t->format, clouds_apart())) {
        rt_log("bokeh: its work images could not be made (%ux%u): skipped at this size", w, h);
        failed_w = w, failed_h = h;
        return;
    }
    VkImageView cv = vk_exec_target_view(t->img, t->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, t->level,
                                         t->base_layer, 1, 0);
    VkImageView zv = vk_exec_target_view(z->img, z->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, z->level,
                                         z->base_layer, 1, 0);
    if (!cv || !zv) return;
    const Look *l = &K.look;
    float radius = l->size * (float)h / 1440.0f / 2.0f;   // half-size pixels, the same look at every resolution
    if (radius > 24.0f) radius = 24.0f;   // half-size pixels; a guard for future looks: the largest today is 6 at 1440p, 9 at 4K
    // the cloud mask needs both copies of this frame (vk_bokeh_clouds and vk_bokeh_pass); without clouds it is 0
    int mask = K.cl == 3 && K.view[IMG_BEFORE] && K.view[IMG_AFTER];
    if (mask && !K.mask_frames++) rt_log("bokeh: the clouds were found: the cloud mask is on");
    float pc[14] = {K.a, K.b, l->start, l->end, radius, l->boost, (float)w, (float)h, (float)hw, (float)hh,
                    (float)K.sky, (float)K.clouds, (float)mask, 0};
    // without this frame's copies the shaders read neither (mask_on = 0), but every binding they declare stays valid
    VkImageView bf = mask ? K.view[IMG_BEFORE] : cv, af = mask ? K.view[IMG_AFTER] : cv;
    const VkSampler s[8] = {K.lin, K.near, VK_NULL_HANDLE, K.lin, K.lin, K.lin, VK_NULL_HANDLE, K.lin};
    const VkImageView v1[8] = {cv, zv, K.view[IMG_HALF], VK_NULL_HANDLE, bf, af, K.view[IMG_MASK], VK_NULL_HANDLE};
    const VkImageView v2[8] = {VK_NULL_HANDLE, VK_NULL_HANDLE, K.view[IMG_BLUR], K.view[IMG_HALF], VK_NULL_HANDLE,
                               VK_NULL_HANDLE, K.view[IMG_SOFT], K.view[IMG_MASK]};
    const VkImageView v3[8] = {cv, zv, K.view[IMG_FULL], K.view[IMG_BLUR], bf, af, VK_NULL_HANDLE, K.view[IMG_SOFT]};
    dispatch(cb, K.down, v1, s, pc, hw, hh);
    dispatch(cb, K.blur, v2, s, pc, hw, hh);
    dispatch(cb, K.comp, v3, s, pc, w, h);
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, t->base_layer, 1},
                     .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
    vkCmdBlitImage(cb, K.img[IMG_FULL], VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    full_barrier(cb);
    ex->recorded += 17;   // 3 x (bind, push, constants, dispatch, barrier) + blit + barrier
    vk_exec_draw_new_cb();   // our compute pipeline and descriptors: the next draw binds everything again
    if (++K.frames == 1 || K.frames % 3600 == 0)
        rt_log("bokeh: %llu frames (scene %ux%u, projection %.4f %.4f, %llu with the cloud mask)",
               (unsigned long long)K.frames, w, h, K.a, K.b, (unsigned long long)K.mask_frames);
}

// Each pass begin, before DLSS's (vk_exec.c vk_exec_pass_begin). The frame's G-buffer, then the first pass into the
// light target alone (the scene is drawn), then the scaling pass: one colour target of the output size, no depth. The
// detection follows vk_dlss.c vk_dlss_pass, which finds the same three points (vk_ao.c vk_ao_pass finds the G-buffer
// the same way): a change to one belongs in the others.
void vk_bokeh_pass(VkExec *ex) {
    read_setting();
    if (!K.mode || K.done || (K.tried && !K.ok)) return;
    if (K.cl == 1) {   // the pass vk_bokeh_clouds ended begins again for the first cloud draw
        K.cl = 2;
        return;
    }
    // the first pass after the clouds (not the cloud pass begun again: the light target alone with the scene depth)
    int cloud_pass = ex->pass_ncolor == 1 && ex->color[0].image == K.color.image && ex->depth.image == K.depth.image;
    if (K.cl == 2 && !cloud_pass) {   // the light target with the clouds (no pass is open here)
        vk_exec_barrier(ex);
        copy_light(ex->cb, K.img[IMG_AFTER]);
        ex->recorded += 2;
        K.cl = 3;
    }
    const VkExecTarget *c = &ex->color[0], *z = &ex->depth;
    if (ex->pass_ncolor >= 5 && z->image && depth32(z->format) && z->width >= SCENE_MIN_W && z->samples <= 1) {
        for (uint32_t i = 0; i < ex->pass_ncolor; i++) {
            const VkExecTarget *t = &ex->color[i];
            if (t->image && t->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && t->width == z->width &&
                t->height == z->height && !t->is3d && t->samples <= 1) {
                if (K.gbuf && K.color.image != t->image) K.cam_ok = 0, K.cl = 0;   // another view: its own camera, no clouds
                K.color = *t, K.depth = *z, K.gbuf = 1, K.post = 0;
                break;
            }
        }
        return;
    }
    if (!K.gbuf) return;
    if (!K.post) {
        if (ex->pass_ncolor == 1 && !z->image && c->image == K.color.image) K.post = 1;
        return;
    }
    unsigned o[2], s[2];
    res_sizes(o, s);
    if (ex->pass_ncolor == 1 && c->image && !z->image && c->image != K.color.image && c->width == o[0] &&
        c->height == o[1] && c->samples <= 1 && !c->is3d && !(c->img && (c->img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))) {
        K.done = 1;
        if (K.cam_ok) run(ex);
    }
}

// Each draw: the projection from the scene's first draw with the scene depth and the camera block (vk_dlss.c reads the
// same block for its view-projection).
void vk_bokeh_draw(VkExec *ex) {
    if (!K.mode || !K.gbuf || K.post || K.cam_ok || !ex->depth.image || ex->depth.image != K.depth.image) return;
    const p_bind_buf *b = &ex->ubo[0][0];
    if (b->size != CAM_SIZE || !b->addr) return;
    const float *f = (const float *)vk_usnap_bytes(b->addr, CAM_SIZE);
    if (!f) f = (const float *)(uintptr_t)b->addr;
    const float *zr = f + 5 * 4, *wr = f + 6 * 4;
    if (!(wr[0] == 0 && wr[1] == 0 && wr[2] == -1.0f && wr[3] == 0 && zr[0] == 0 && zr[1] == 0 && zr[3] != 0)) return;
    // The shaders read the depth as OpenGL clip depth (-w..w, stored as z_ndc * 0.5 + 0.5), which is what the game sets
    // (DeviceSetDepthMode NEAR_IS_MINUS_W). Any other mode would give wrong distances: no blur rather than a wrong one.
    if (!nvn_depth_minus_w()) {
        static int told;
        if (!told++) rt_log("bokeh: the game's depth mode is not near = -w: skipped");
        return;
    }
    K.a = zr[2], K.b = zr[3], K.cam_ok = 1;
}

// The clouds (vk_exec_draw.c recognises their program): drawn into the light target after the sky, the moons and the
// stars, depth tested but writing no depth, so the depth buffer can't tell them from the sky. As their first draw comes,
// the open pass ends and the light target is copied as it is without them; vk_bokeh_pass copies it again as the next
// pass begins, and pass 1 turns the difference into a cloud mask. Both copies are of the game's picture before its
// tonemap, so the mask is a coverage estimate, not the clouds' colour. Not needed when the sky and the clouds both blur.
int vk_bokeh_wants_clouds(void) { return K.mode && K.gbuf && !K.post && !K.cl && (!K.sky || !K.clouds) && K.ok; }

void vk_bokeh_clouds(VkExec *ex) {
    const VkExecTarget *t = &K.color;
    if (ex->color[0].image != t->image) return;   // the same program also draws a small copy of the clouds elsewhere
    if (K.retry) failed_w = failed_h = 0, K.retry = 0;
    if (t->width == failed_w && t->height == failed_h) return;
    if (ex->in_pass) vk_exec_pass_end(ex);   // before images_for, which may record layout barriers
    if (!images_for(ex->cb, t->width, t->height, t->format, 1)) {   // only called while the clouds are kept apart
        rt_log("bokeh: its work images with the cloud images could not be made (%ux%u): skipped at this size", t->width,
               t->height);
        failed_w = t->width, failed_h = t->height;
        return;
    }
    vk_exec_barrier(ex);
    copy_light(ex->cb, K.img[IMG_BEFORE]);
    ex->recorded += 2;
    K.cl = 1;
}

void vk_bokeh_frame(void) { K.gbuf = K.post = K.done = K.cam_ok = K.cl = 0; }
