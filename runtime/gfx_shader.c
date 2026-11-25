// Shader service client: turns an NVN program (control section, code, c1 constants and the
// translator's answers) into SPIR-V plus reflection through `xcx_xlate.exe --serve` children, started lazily with
// pipes and kept for the life of the process (a pool: XCX_XLATE_PROCS at run time, more for the boot prewarm).
// Results are cached in memory and on disk (shader-cache/<key>.spv and <key>.res.json next to xcx.exe), so a shader
// is translated once per machine and translator.
//
// Keys. The code hash is sha256 over stage (u32), the 0x880-byte control section, code_size (u64), the code, the c1
// length (u32) and bytes. Per code hash and translator the cache holds a use mask (<sha256("TKSU", xlate id, code
// hash)>.use): the texture/image slots the translator queried (.res.json tex_queried, img_queried), the vertex
// attributes a VS declares and the colour targets an FS declares (SPIR-V Locations). The input id is sha256 over "TKSI",
// IN_VERSION, the code hash and the answers with everything outside the mask zeroed (fixed little-endian layout), so
// stale bindings in slots the shader never reads do not make new ids; it does not depend on the translator. The cache
// key is sha256 over "TKSK", KEY_VERSION, the translator identity and the input id. A code hash without a known mask is
// translated under a provisional key ("TKSP", full answers); its files are renamed to the cache key and the mask stored.
//
// Translator identity: build/xlate/xcx_xlate.id, sha256 hex that tools/build_xlate.py writes over the sources it
// builds from (Program.cs, the csproj, the patches, the vendor tree); without it the sha256 of the exe. XCX_XLATE_ID
// (64 hex digits) overrides it. A translator change therefore changes every cache key without a hand-made KEY_VERSION
// bump; KEY_VERSION is for changes on this side (key layout, how the output is used).
//
// Stored inputs: every translation also writes <cache>/in/<input id>.in (magic "TKSI"; stage, the full answers, the
// control section, the code up to its length, c1), so a pipeline key record (vk_pipeline.c), which names its shaders by
// input id, can be translated again under a new translator without guest memory (gfx_shader_load_input,
// gfx_shader_prewarm).
//
// Reflection: every shader that enters the cache has its .res.json parsed into GfxShaderRes arrays and its SPIR-V
// scanned for interface Locations and the workgroup size (gfx.h accessors at the end of the file).
//
// Provenance: the c2 handle ranges of handle_slot are those of MacNativeGames' totk_xlate Service.cs (MIT).
// The code layout the translator sees (graphics: code[0x30..], compute: code[0..]; c1 = code[c1..length]
// with c1 and length at control+0x700) follows tools/shader_census.py, itself MacNativeGames' xcx shader tooling (MIT).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"

#define CONTROL_SIZE 0x880
#define CODE_SKIP 0x30                                     // graphics code starts after the first 0x30 bytes of the program
#define BUCKETS 4096
#define LINE_MAX_LEN 4096
#define KEY_VERSION 4                                      // in every cache key; bump when the key layout or the use of the
                                                           // output changes here (4: translator identity, input ids);
                                                           // translator changes are covered by its identity
#define IN_VERSION 1                                       // in every input id and .in file: bump when the answers layout
                                                           // or the program hash changes (drops every stored input)
#define USE_MAGIC 0x4d554b54u                              // "TKUM", .use files
#define USE_VERSION 1
#define IN_MAGIC 0x49534b54u                               // "TKSI", .in files
#define MAX_SVC 32                                         // translator processes

typedef struct {                                           // which answers a shader's translation depends on
    uint32_t slots;                                        // tex_type, img_type, img_format of these c2 slots
    uint32_t attrs;                                        // attr_type (VS input Locations)
    uint32_t fouts;                                        // frag_out_type (FS output Locations)
} UseMask;

typedef struct UseEnt {                                    // code hash -> use mask; known 0 caches a disk miss
    struct UseEnt *next;
    uint8_t code_hash[32];
    int known;
    UseMask m;
} UseEnt;

struct GfxShader {
    struct GfxShader *next;
    uint8_t key[32];
    uint8_t iid[32];                                       // input id (see the keys at the top)
    uint32_t *spirv;
    uint64_t words;
    char *reflection;                                      // the service's .res.json text, parsed into the fields below
    uint64_t reflection_size;
    // Reflection (reflect(), once per shader before it enters the cache)
    int stage;                                             // GFX_STAGE_*
    GfxShaderRes *res[GFX_RES_KINDS];
    int nres[GFX_RES_KINDS];
    uint32_t in_mask, out_mask;                            // user Locations of Input / Output variables
    GfxShaderIo in[32], out[32];
    uint32_t local[3];
    uint32_t mvprev_banks;                                 // .res.json "mvprev_banks" (patch 0012), 0 none
};

static PlatMutex cache_lock = PLAT_MUTEX_INIT;                  // memory cache
static GfxShader *buckets[BUCKETS];
static UseEnt *use_buckets[BUCKETS];                       // under cache_lock
static volatile int64_t n_requests, n_hits, n_failures, n_keys, n_xlated;   // n_keys: memory cache entries

typedef struct {                                           // one translator child
    PlatChild *child;                                      // NULL: not started
    int busy;                                              // a request owns it (under S.lock)
    int low;                                               // the background preparation's (tl_private) only: lowered
    size_t in_len;                                         // on Linux (plat_child_lower), never used for the game's
    char inbuf[LINE_MAX_LEN * 2];
} Svc;

// The background preparation's translations stay on disk only: with tl_private set, translate_one hands back the
// shader it made or loaded without entering it in the memory cache, and sets tl_owned (the caller frees it). Kept in
// memory, the ~25,000 seed variants would stay resident for the whole run though play draws few of them. Its requests
// also go to translator children of their own (svc_get), lowered on Linux, where the threads stay at nice 0.
static _Thread_local int tl_private, tl_owned;

static struct {
    PlatMutex lock;
    PlatCond freed;
    int limit;                                             // children in use at most (0: svc_runtime_limit)
    Svc s[MAX_SVC];
} S = {PLAT_MUTEX_INIT, PLAT_COND_INIT};

static struct {                                            // translations in flight: cache key, or code hash while the
    PlatMutex lock;                                          // use mask is unknown; one translation per id at a time
    PlatCond done;
    uint8_t id[2 * MAX_SVC][32];
    int n;
} F = {PLAT_MUTEX_INIT, PLAT_COND_INIT};

#define PATH_CAP 1024                                  // bytes of a UTF-8 path
static char cache_dir[PATH_CAP];
static char xlate_exe[PATH_CAP];
static uint8_t xlate_id[32];                               // translator identity (see the keys at the top)
static int host_f64 = 1, host_set;                         // gfx_shader_set_host
static uint32_t host_sbuf, host_sg[3] = {32, 32, 32}, host_gather;
static PlatOnce paths_once = PLAT_ONCE_INIT;

// ---- small helpers

#define file_exists plat_file_exists
#define mkdir_p plat_file_mkdir_p
#define read_file plat_file_read
#define write_file plat_file_write

// Appends a UTF-8 string as a JSON string body: ASCII as is, quotes and backslashes escaped, the rest as \uXXXX
// (UTF-16 units), so the service's stdin encoding does not matter.
static void json_esc(char **o, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p++;
        if (c >= 0x80) {
            int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
            c &= 0x3f >> more;
            while (more-- && (*p & 0xc0) == 0x80) c = c << 6 | (*p++ & 0x3f);
        }
        if (c == '"' || c == '\\') *(*o)++ = '\\', *(*o)++ = (char)c;
        else if (c >= 0x20 && c < 0x7f) *(*o)++ = (char)c;
        else if (c >= 0x10000) {
            c -= 0x10000;
            *o += sprintf(*o, "\\u%04x\\u%04x", 0xd800 + (c >> 10), 0xdc00 + (c & 0x3ff));
        } else *o += sprintf(*o, "\\u%04x", c);
    }
}

// ---- paths and the translator identity

static int hex32(const char *h, uint8_t out[32]) {        // 64 hex digits, then nothing but white space
    for (int i = 0; i < 64; i++) {
        char c = h[i] >= 'A' && h[i] <= 'F' ? (char)(h[i] | 32) : h[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return 0;
        out[i / 2] = (uint8_t)(i & 1 ? out[i / 2] | v : v << 4);
    }
    for (const char *p = h + 64; *p; p++)
        if (*p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') return 0;
    return 1;
}

// XCX_XLATE_ID (64 hex digits, or any other text, which is hashed), else <translator>.id next to the exe
// (tools/build_xlate.py), else the sha256 of the exe.
static void xlate_identity(void) {
    char text[256] = "";
    const char *how = "XCX_XLATE_ID";
    const char *id_env = cfg_get("XCX_XLATE_ID");
    if (id_env && *id_env && strlen(id_env) < sizeof text) {
        strcpy(text, id_env);
        if (!hex32(text, xlate_id)) rt_sha256(text, strlen(text), xlate_id);
    } else {
        char p[PATH_CAP + 8];
        strcpy(p, xlate_exe);
        char *dot = strrchr(p, '.'), *sep = plat_last_sep(p);
        if (dot && dot > sep) *dot = 0;
        strcat(p, ".id");
        uint64_t size = 0;
        char *f = (char *)read_file(p, &size);
        how = "xcx_xlate.id";
        if (!f || !hex32(f, xlate_id)) {
            uint8_t *exe = read_file(xlate_exe, &size);
            how = exe ? "sha256 of the exe; tools/build_xlate.py writes xcx_xlate.id" : "none: no translator";
            if (exe) rt_sha256(exe, (size_t)size, xlate_id);
            free(exe);
        }
        free(f);
    }
    rt_log("gfx_shader: translator identity %02x%02x%02x%02x%02x%02x%02x%02x (%s)", xlate_id[0], xlate_id[1], xlate_id[2],
           xlate_id[3], xlate_id[4], xlate_id[5], xlate_id[6], xlate_id[7], how);
}

// The repository root is the nearest parent of xcx.exe that holds build/xlate/xcx_xlate.exe. The shader cache lives
// next to xcx.exe, so every build dir has its own (as the pipeline cache, vk_pipeline.c cache_path_init).
static void use_preload_start(void);

// The shader cache folder: XCX_SHADER_CACHE (a relative path from the exe's folder, as the setup reads it:
// installer/shaders.py shader_cache), else <exe folder>/shader-cache; "" when it does not fit PATH_CAP. One rule for
// init_paths and the clear (gfx_shader_cache_clear_at_start), so the clear empties the folder the game uses.
static void cache_folder(const char *exe_dir, char out[PATH_CAP]) {
    const char *cfg = cfg_get("XCX_SHADER_CACHE");
    out[0] = 0;
    if (cfg && *cfg && strlen(cfg) < PATH_CAP) {
        int absolute = cfg[0] == '/' || cfg[0] == '\\' || (cfg[0] && cfg[1] == ':');
        if (absolute || !exe_dir[0]) strcpy(out, cfg);
        else if (snprintf(out, PATH_CAP, "%s/%s", exe_dir, cfg) >= PATH_CAP) out[0] = 0;
    } else if (exe_dir[0] && snprintf(out, PATH_CAP, "%s/shader-cache", exe_dir) >= PATH_CAP) {
        out[0] = 0;
    }
}

// The folder of the setup's seed and of the prepared list made from it: beside xcx.exe, where installer/steps.py
// Prepare writes the seed (also when XCX_PIPELINE_CACHE puts the pipeline cache elsewhere). 0 when the path does not
// fit.
int gfx_shader_seed_dir(char *out, size_t cap) {
    int n = plat_exe_path(out, cap);
    if (!n || (size_t)n >= cap) return 0;
    char *cut = plat_last_sep(out);
    if (!cut) return 0;
    *cut = 0;
    return 1;
}

static void init_paths(void *param) {
    (void)param;
    char exe[PATH_CAP], root[PATH_CAP] = "", exe_dir[PATH_CAP] = "";
    if (plat_exe_path(exe, sizeof exe)) {
        char *last = plat_last_sep(exe);
        if (last) snprintf(exe_dir, sizeof exe_dir, "%.*s", (int)(last - exe), exe);
        for (char *cut = plat_last_sep(exe); cut; cut = plat_last_sep(exe)) {
            *cut = 0;
            char probe[PATH_CAP + 32];
            snprintf(probe, sizeof probe, "%s/build/xlate/xcx_xlate" PLAT_EXE_SUFFIX, exe);
            if (file_exists(probe)) {
                strcpy(root, exe);
                break;
            }
        }
    }
    const char *xlate_cfg = cfg_get("XCX_XLATE");
    char beside[PATH_CAP + 32];   // a packaged folder (tools/package_build.py) keeps the translator next to xcx.exe
    if (exe_dir[0]) snprintf(beside, sizeof beside, "%s/xcx_xlate" PLAT_EXE_SUFFIX, exe_dir);
    if (xlate_cfg && *xlate_cfg && strlen(xlate_cfg) < sizeof xlate_exe) strcpy(xlate_exe, xlate_cfg);
    else if (exe_dir[0] && file_exists(beside)) strcpy(xlate_exe, beside);
    else if (root[0]) snprintf(xlate_exe, sizeof xlate_exe, "%s/build/xlate/xcx_xlate" PLAT_EXE_SUFFIX, root);
    cache_folder(exe_dir, cache_dir);
    xlate_identity();
    use_preload_start();
    return;
}

static void paths(void) { plat_once(&paths_once, init_paths, NULL); }

// Host answers (vk_caps.c, at device creation, before any translation). Answers other than the defaults (Float64,
// exact storage offsets, 32 lanes, no gather bias) are mixed into the translator identity, so they get their own cache
// keys.
void gfx_shader_set_host(int float64, uint32_t sbuf_align, const uint32_t subgroup[3], uint32_t gather_bits) {
    paths();
    if (host_set++) return;
    host_f64 = float64, host_sbuf = sbuf_align, host_gather = gather_bits;
    memcpy(host_sg, subgroup, sizeof host_sg);
    if (float64 && !sbuf_align && subgroup[0] == 32 && subgroup[1] == 32 && subgroup[2] == 32 && !gather_bits) return;
    uint8_t buf[32 + 96];
    int n = snprintf((char *)buf + 32, 96, "TKHO f64=%d sbuf=%u sg=%u,%u,%u", float64, sbuf_align, subgroup[0], subgroup[1],
                     subgroup[2]);
    if (gather_bits) n += snprintf((char *)buf + 32 + n, (size_t)(96 - n), " gather=%u", gather_bits);
    memcpy(buf, xlate_id, 32);
    rt_sha256(buf, 32 + (size_t)n, xlate_id);
    rt_log("gfx_shader: host options %s: own shader cache keys", (char *)buf + 37);
}

// ---- the translator children

static void child_stop(Svc *v) {
    plat_child_stop(v->child);
    v->child = NULL;
    v->in_len = 0;
}

static int child_start(Svc *v) {
    paths();
    if (!xlate_exe[0] || !file_exists(xlate_exe)) {
        rt_log("gfx_shader: translator not found (%s); set XCX_XLATE", xlate_exe[0] ? xlate_exe : "<none>");
        return 0;
    }
    uint32_t err = 0;
    v->child = plat_child_start(xlate_exe, "--serve", &err);
    if (!v->child) {
        rt_log("gfx_shader: cannot start %s (error %lu)", xlate_exe, (unsigned long)err);
        return 0;
    }
    // for good: an unprivileged process can lower a priority but not raise it again (Linux; nothing on Windows)
    if (v->low) plat_child_lower(v->child);
    v->in_len = 0;
    return 1;
}

// One line out, one line in (no newline in the reply). 0 when the pipe broke.
static int child_exchange_raw(Svc *v, const char *line, char *reply, size_t reply_size) {
    if (!v->child && !child_start(v)) return 0;
    plat_inc64(&n_requests);
    size_t n = strlen(line);
    if (!plat_child_write(v->child, line, n)) return 0;
    if (!plat_child_write(v->child, "\n", 1)) return 0;
    for (;;) {
        char *nl = memchr(v->inbuf, '\n', v->in_len);
        if (nl) {
            size_t len = (size_t)(nl - v->inbuf), keep = len < reply_size - 1 ? len : reply_size - 1;
            memcpy(reply, v->inbuf, keep);
            while (keep && reply[keep - 1] == '\r') keep--;
            reply[keep] = 0;
            memmove(v->inbuf, nl + 1, v->in_len - len - 1);
            v->in_len -= len + 1;
            return 1;
        }
        if (v->in_len == sizeof v->inbuf) v->in_len = 0;  // an overlong line: drop it, wait for its end
        long got = plat_child_read(v->child, v->inbuf + v->in_len, sizeof v->inbuf - v->in_len);
        if (got <= 0) return 0;
        v->in_len += (size_t)got;
    }
}

static int child_exchange(Svc *v, const char *line, char *reply, size_t reply_size) {
    uint64_t p0 = perf_begin();   // XCX_TRACE=perf: render-thread time spent translating
    int ok = child_exchange_raw(v, line, reply, reply_size);
    perf_end(PERF_XLATE, p0);
    return ok;
}

// Children allowed at run time: XCX_XLATE_PROCS, default 2.
static int svc_runtime_limit(void) {
    static volatile int32_t n;
    if (!n) {
        const char *e = cfg_get("XCX_XLATE_PROCS");
        int v = e && *e ? atoi(e) : 2;
        plat_xchg32(&n, v < 1 ? 1 : v > MAX_SVC ? MAX_SVC : v);
    }
    return (int)n;
}

static int svc_used(void) {                                // caller holds S.lock
    int used = 0;
    for (int i = 0; i < MAX_SVC; i++) used += S.s[i].busy || S.s[i].child;
    return used;
}

// A child for one request of the given kind (low: the background preparation's, tl_private): an idle running one of
// that kind, else a new slot while fewer than S.limit are in use, else an idle one of the other kind, stopped and
// started again as this kind, else waits.
static Svc *svc_get(int low) {
    plat_mutex_lock(&S.lock);
    if (!S.limit) S.limit = svc_runtime_limit();
    Svc *v;
    int restart = 0;
    for (;;) {
        Svc *empty = NULL, *other = NULL;
        v = NULL;
        for (int i = 0; i < MAX_SVC && !v; i++) {
            Svc *c = &S.s[i];
            if (!c->busy && c->child && c->low == low) v = c;
            else if (!c->busy && c->child) other = other ? other : c;
            else if (!c->busy && !empty) empty = c;
        }
        if (!v && empty && svc_used() < S.limit) v = empty;
        if (!v && other) v = other, restart = 1;
        if (v) break;
        plat_cond_wait(&S.freed, &S.lock);
    }
    v->busy = 1;
    plat_mutex_unlock(&S.lock);
    if (restart) child_stop(v);
    v->low = low;   // busy: only this request touches it now
    return v;
}

// Returns a child; one over the limit (svc_set_limit lowered it) is stopped.
static void svc_put(Svc *v) {
    plat_mutex_lock(&S.lock);
    int stop = v->child && svc_used() > S.limit;
    if (!stop) v->busy = 0;
    plat_mutex_unlock(&S.lock);
    if (stop) {
        child_stop(v);
        plat_mutex_lock(&S.lock);
        v->busy = 0;
        plat_mutex_unlock(&S.lock);
    }
    plat_cond_broadcast(&S.freed);
}

// Sets the number of children; idle ones over it are stopped now, busy ones when they are returned.
static void svc_set_limit(int n) {
    Svc *stop[MAX_SVC];
    int k = 0;
    plat_mutex_lock(&S.lock);
    S.limit = n < 1 ? 1 : n > MAX_SVC ? MAX_SVC : n;
    for (int i = 0, used = svc_used(); i < MAX_SVC && used > S.limit; i++)
        if (!S.s[i].busy && S.s[i].child) S.s[i].busy = 1, stop[k++] = &S.s[i], used--;
    plat_mutex_unlock(&S.lock);
    for (int i = 0; i < k; i++) child_stop(stop[i]);
    plat_mutex_lock(&S.lock);
    for (int i = 0; i < k; i++) stop[i]->busy = 0;
    plat_mutex_unlock(&S.lock);
    plat_cond_broadcast(&S.freed);
}

// One request on a child; a dead child is restarted once. 0 when the translator is unavailable. The caller has given
// up its virtual core (kern_core_away): the wait for a child and the translation are host time.
static int svc_request(const char *line, char *reply, size_t reply_size) {
    Svc *v = svc_get(tl_private);
    int ok = 0;
    for (int attempt = 0; attempt < 2 && !ok; attempt++)
        if (!(ok = child_exchange(v, line, reply, reply_size))) child_stop(v);
    svc_put(v);
    return ok;
}

// One translation per id at a time (F); F.id full also waits.
static void flight_enter(const uint8_t id[32]) {
    plat_mutex_lock(&F.lock);
    for (;;) {
        int busy = F.n == 2 * MAX_SVC;
        for (int i = 0; i < F.n && !busy; i++) busy = !memcmp(F.id[i], id, 32);
        if (!busy) break;
        plat_cond_wait(&F.done, &F.lock);
    }
    memcpy(F.id[F.n++], id, 32);
    plat_mutex_unlock(&F.lock);
}

static void flight_leave(const uint8_t id[32]) {
    plat_mutex_lock(&F.lock);
    for (int i = 0; i < F.n; i++)
        if (!memcmp(F.id[i], id, 32)) {
            memcpy(F.id[i], F.id[--F.n], 32);
            break;
        }
    plat_mutex_unlock(&F.lock);
    plat_cond_broadcast(&F.done);
}

// ---- cache

static GfxShader *mem_find(const uint8_t key[32]) {
    plat_mutex_lock_shared(&cache_lock);
    GfxShader *s = buckets[(key[0] | key[1] << 8) % BUCKETS];
    while (s && memcmp(s->key, key, 32)) s = s->next;
    plat_mutex_unlock_shared(&cache_lock);
    return s;
}

static void shader_free(GfxShader *s) {
    for (int k = 0; k < GFX_RES_KINDS; k++) free(s->res[k]);
    free(s->spirv), free(s->reflection), free(s);
}

static void reflect(GfxShader *s);

// Inserts s, or frees it and returns the entry another thread inserted first.
static GfxShader *mem_insert(GfxShader *s) {
    plat_mutex_lock(&cache_lock);
    GfxShader **head = &buckets[(s->key[0] | s->key[1] << 8) % BUCKETS], *r = *head;
    while (r && memcmp(r->key, s->key, 32)) r = r->next;
    if (r) {
        shader_free(s);
        s = r;
    } else {
        s->next = *head;
        *head = s;
        n_keys++;
    }
    plat_mutex_unlock(&cache_lock);
    return s;
}

static void key_hex(const uint8_t key[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[i * 2] = hex[key[i] >> 4], out[i * 2 + 1] = hex[key[i] & 15];
    out[64] = 0;
}

// A .res.json the translator wrote: one JSON object that names its stage, no NUL inside. A file an interrupted write
// left zero-filled (3647 of 21924 in one cache were) fails this; reflect() would take it for a vertex shader with
// no resources, and its pipeline layout would lack the shader's buffers and textures.
static int reflection_valid(const char *res, uint64_t size) {
    if (strlen(res) != size) return 0;
    const char *b = res, *e = res + size;
    while (b < e && isspace((unsigned char)*b)) b++;
    while (e > b && isspace((unsigned char)e[-1])) e--;
    if (e - b < 2 || *b != '{' || e[-1] != '}') return 0;
    const char *st = strstr(b, "\"stage\"");
    if (!st) return 0;
    st = strchr(st + 7, '"');
    return st && (!strncmp(st, "\"vs\"", 4) || !strncmp(st, "\"ps\"", 4) || !strncmp(st, "\"cs\"", 4));
}

// Every instruction after the header has a word count and the last one ends at the module's end: a module with
// zero-filled clusters fails.
static int spirv_complete(const uint32_t *w, uint64_t n) {
    uint64_t i = 5;
    while (i < n) {
        uint32_t len = w[i] >> 16;
        if (!len) return 0;
        i += len;
    }
    return i == n;
}

// Reads <key>.spv and <key>.res.json; NULL if either is missing or malformed (the caller translates again and
// overwrites both). iid: the input id, NULL when the caller sets it.
static GfxShader *disk_load(const uint8_t key[32], const uint8_t *iid) {
    static volatile int32_t n_bad;
    char hex[65], p[PATH_CAP + 80];
    key_hex(key, hex);
    uint64_t spv_size = 0, res_size = 0;
    snprintf(p, PATH_CAP + 80, "%s/%s.res.json", cache_dir, hex);
    uint8_t *res = read_file(p, &res_size);
    if (!res) return NULL;
    if (!reflection_valid((const char *)res, res_size)) {
        int32_t n = plat_inc32(&n_bad);
        if (n <= 3 || !(n & (n - 1))) rt_log("gfx_shader: %s.res.json is malformed, translating again (%d so far)", hex, n);
        free(res);
        return NULL;
    }
    snprintf(p, PATH_CAP + 80, "%s/%s.spv", cache_dir, hex);
    uint8_t *spv = read_file(p, &spv_size);
    uint32_t magic = 0;
    if (spv && spv_size >= 20) memcpy(&magic, spv, 4);
    if (!spv || spv_size % 4 || magic != 0x07230203 || !spirv_complete((const uint32_t *)spv, spv_size / 4)) {
        free(res), free(spv);
        return NULL;
    }
    GfxShader *s = calloc(1, sizeof *s);
    memcpy(s->key, key, 32);
    if (iid) memcpy(s->iid, iid, 32);
    s->spirv = (uint32_t *)spv;
    s->words = spv_size / 4;
    s->reflection = (char *)res;
    s->reflection_size = res_size;
    reflect(s);
    if (s->stage == GFX_STAGE_FS) vk_hdr_shader_swap(&s->spirv, &s->words);   // the in-memory copy; the cache keeps its own
    return s;
}

const GfxShader *gfx_shader_own(const uint32_t *spirv, uint64_t words, const char *res_json) {
    if (words < 5 || spirv[0] != 0x07230203 || !spirv_complete(spirv, words) ||
        !reflection_valid(res_json, strlen(res_json)))
        return NULL;
    GfxShader *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    size_t rn = strlen(res_json);
    uint8_t *buf = malloc(words * 4 + rn);
    s->spirv = malloc(words * 4), s->reflection = malloc(rn + 1);
    if (!buf || !s->spirv || !s->reflection) rt_fatal("gfx_shader: out of memory");
    memcpy(buf, spirv, words * 4), memcpy(buf + words * 4, res_json, rn);
    rt_sha256(buf, words * 4 + rn, s->key);
    free(buf);
    memcpy(s->spirv, spirv, words * 4), memcpy(s->reflection, res_json, rn + 1);
    s->words = words, s->reflection_size = rn;
    reflect(s);
    return s;
}

// ---- request building

static const char *const tex_names[] = {NULL, "1d", "1darray", "buffer", "2d", "2darray", "2dms", "2dmsarray", "3d",
                                        "cube", "cubearray"};   // GfxShaderAnswers tex_type / img_type codes
static const char *const fmt_names[] = {   // img_format codes: Ryujinx TextureFormat enum order
    NULL, "R8Unorm", "R8Snorm", "R8Uint", "R8Sint", "R16Float", "R16Unorm", "R16Snorm", "R16Uint", "R16Sint", "R32Float",
    "R32Uint", "R32Sint", "R8G8Unorm", "R8G8Snorm", "R8G8Uint", "R8G8Sint", "R16G16Float", "R16G16Unorm", "R16G16Snorm",
    "R16G16Uint", "R16G16Sint", "R32G32Float", "R32G32Uint", "R32G32Sint", "R8G8B8A8Unorm", "R8G8B8A8Snorm",
    "R8G8B8A8Uint", "R8G8B8A8Sint", "R16G16B16A16Float", "R16G16B16A16Unorm", "R16G16B16A16Snorm", "R16G16B16A16Uint",
    "R16G16B16A16Sint", "R32G32B32A32Float", "R32G32B32A32Uint", "R32G32B32A32Sint", "R10G10B10A2Unorm",
    "R10G10B10A2Uint", "R11G11B10Float"};
#define COUNT(a) ((int)(sizeof a / sizeof *a))

static const char *attr_name(uint8_t t) { return t == 1 ? "sint" : t == 2 ? "uint" : "float"; }

// ---- reflection: the .res.json the translator writes (tools/xlate/Program.cs Translate) and the SPIR-V itself

// `"name":` inside [o, e): the first byte after the colon and its spaces, NULL when absent.
static const char *json_key(const char *o, const char *e, const char *name) {
    char pat[40];
    int n = snprintf(pat, sizeof pat, "\"%s\":", name);
    for (const char *p = o; p + n <= e; p++)
        if (*p == '"' && !memcmp(p, pat, (size_t)n)) {
            p += n;
            while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
            return p;
        }
    return NULL;
}

static long json_int(const char *o, const char *e, const char *name, long def) {
    const char *p = json_key(o, e, name);
    return p ? strtol(p, NULL, 10) : def;
}

// Copies a string value into buf (empty when absent or not a string).
static void json_str(const char *o, const char *e, const char *name, char *buf, size_t n) {
    const char *p = json_key(o, e, name);
    size_t k = 0;
    if (p && *p == '"')
        for (p++; p < e && *p != '"' && k + 1 < n; p++) buf[k++] = *p;
    buf[k] = 0;
}

// The objects of the flat array `"name": [ {...}, ... ]`: calls fn for each object's [start, end). Returns the count.
static int json_objects(const char *js, const char *name, void (*fn)(GfxShader *, int, const char *, const char *),
                        GfxShader *s, int kind) {
    const char *end = js + strlen(js), *p = json_key(js, end, name);
    if (!p || *p != '[') return 0;
    int n = 0;
    for (p++; p < end && *p != ']'; p++) {
        if (*p != '{') continue;
        const char *q = memchr(p, '}', (size_t)(end - p));
        if (!q) break;
        if (fn) fn(s, kind, p, q);
        n++;
        p = q;
    }
    return n;
}

static int fmt_code(const char *name) {
    for (int i = 1; i < COUNT(fmt_names); i++)
        if (!strcmp(fmt_names[i], name)) return i;
    return 0;
}

// c2 slot of a texture or image handle (Service.cs of MNG's xcx_xlate: combined handles at 0x20, 32 slots; images at
// 0x120; separate textures at 0x168), -1 outside those ranges or for a handle in another constant buffer.
static int handle_slot(int kind, int cbuf_slot, int handle, int separate) {
    if (cbuf_slot >= 0 && cbuf_slot != 2) return -1;
    int bytes = (handle & 0x3fff) * 4;
    if (kind == GFX_RES_IMG) return bytes >= 0x120 && bytes < 0x168 ? (bytes - 0x120) / 8 : -1;
    if (separate) return bytes >= 0x168 && bytes < 0x568 ? (bytes - 0x168) / 8 : -1;
    return bytes >= 0x20 && bytes < 0x120 ? (bytes - 0x20) / 8 : -1;
}

static void add_res(GfxShader *s, int kind, const char *o, const char *e) {
    GfxShaderRes *r = &s->res[kind][s->nres[kind]++];
    r->set = 0;
    r->binding = (uint16_t)json_int(o, e, "binding", 0);
    if (kind == GFX_RES_UBO || kind == GFX_RES_SSBO) {
        r->slot = (int16_t)json_int(o, e, "slot", -1);
        r->count = 1;
        if (kind == GFX_RES_SSBO) {
            r->cbuf_slot = (int8_t)json_int(o, e, "cb_slot", -1);
            r->handle = (uint16_t)json_int(o, e, "cb_offset", 0);
            r->flags = (uint8_t)json_int(o, e, "flags", 0);
        }
        return;
    }
    char buf[32];
    json_str(o, e, "format", buf, sizeof buf);
    const char *sep = json_key(o, e, "separate");
    r->type = (uint16_t)json_int(o, e, "type", 0);
    r->format = (uint8_t)fmt_code(buf);
    r->flags = (uint8_t)json_int(o, e, "flags", 0);
    r->cbuf_slot = (int8_t)json_int(o, e, "cbuf_slot", -1);
    int handle = (int)json_int(o, e, "handle", 0);
    r->handle = (uint16_t)handle;
    r->count = (uint16_t)json_int(o, e, "array", 1);
    r->separate = sep && *sep == 't';
    r->slot = (int16_t)handle_slot(kind, r->cbuf_slot, handle, r->separate);
}

// SPIR-V opcodes and enumerants used below (SPIR-V 1.6 specification, section 3).
enum { OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23,
       OP_TYPE_ARRAY = 28, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71,
       DEC_BUILTIN = 11, DEC_LOCATION = 30, DEC_COMPONENT = 31, SC_INPUT = 1, SC_OUTPUT = 3, EM_LOCAL_SIZE = 17 };

// Interface Locations and the compute workgroup size from the module. Ids beyond 2^20 are ignored (Ryujinx modules stay
// far below).
static void reflect_spirv(GfxShader *s) {
    const uint32_t *w = s->spirv;
    uint64_t n = s->words;
    s->local[0] = s->local[1] = s->local[2] = 1;
    if (n < 5) return;
    uint32_t bound = w[3] < (1u << 20) ? w[3] : (1u << 20);
    // per id: location + 1 (0 = none), component flag, scalar/vector description of types, pointee of pointers
    int32_t *loc = calloc(bound, sizeof *loc);
    uint8_t *packed = calloc(bound, 1), *ttype = calloc(bound, 1), *tcomps = calloc(bound, 1);
    uint32_t *pointee = calloc(bound, sizeof *pointee);
    if (!loc || !packed || !ttype || !tcomps || !pointee) goto out;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        const uint32_t *a = w + i + 1;
        uint32_t id = len > 1 ? a[0] : 0;
        if (op == OP_EXECUTION_MODE && len >= 6 && a[1] == EM_LOCAL_SIZE) {
            s->local[0] = a[2], s->local[1] = a[3], s->local[2] = a[4];
        } else if (op == OP_DECORATE && len >= 4 && id < bound) {
            if (a[1] == DEC_LOCATION) loc[id] = (int32_t)a[2] + 1;
            else if (a[1] == DEC_COMPONENT) packed[id] = 1;
        } else if (op == OP_TYPE_INT && len >= 4 && id < bound) {
            ttype[id] = a[2] ? GFX_TYPE_SINT : GFX_TYPE_UINT, tcomps[id] = 1;
        } else if (op == OP_TYPE_FLOAT && len >= 3 && id < bound) {
            ttype[id] = GFX_TYPE_FLOAT, tcomps[id] = 1;
        } else if (op == OP_TYPE_VECTOR && len >= 4 && id < bound && a[1] < bound) {
            ttype[id] = ttype[a[1]], tcomps[id] = (uint8_t)a[2];
        } else if (op == OP_TYPE_ARRAY && len >= 4 && id < bound && a[1] < bound) {
            ttype[id] = ttype[a[1]], tcomps[id] = tcomps[a[1]];   // element type (arrayed interface blocks)
        } else if (op == OP_TYPE_POINTER && len >= 4 && id < bound) {
            pointee[id] = a[2];
        } else if (op == OP_VARIABLE && len >= 4) {
            uint32_t type = a[0], var = a[1], sc = a[2];
            if ((sc == SC_INPUT || sc == SC_OUTPUT) && var < bound && type < bound && loc[var] > 0 && loc[var] <= 32) {
                int l = loc[var] - 1;
                uint32_t t = pointee[type] < bound ? pointee[type] : 0;
                GfxShaderIo io = {ttype[t], tcomps[t] ? tcomps[t] : 4, packed[var]};
                if (sc == SC_INPUT) s->in_mask |= 1u << l, s->in[l] = io;
                else s->out_mask |= 1u << l, s->out[l] = io;
            }
        }
        i += len;
    }
out:
    free(loc), free(packed), free(ttype), free(tcomps), free(pointee);
}

static void reflect(GfxShader *s) {
    static const char *const kinds[GFX_RES_KINDS] = {"cbufs", "sbufs", "textures", "images"};
    const char *js = s->reflection;
    char stage[8];
    json_str(js, js + strlen(js), "stage", stage, sizeof stage);
    s->stage = !strcmp(stage, "cs") ? GFX_STAGE_CS : !strcmp(stage, "ps") ? GFX_STAGE_FS : GFX_STAGE_VS;
    s->mvprev_banks = (uint32_t)json_int(js, js + strlen(js), "mvprev_banks", 0);
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        int count = json_objects(js, kinds[k], NULL, s, k);
        s->res[k] = calloc(count ? count : 1, sizeof *s->res[k]);
        json_objects(js, kinds[k], add_res, s, k);
    }
    reflect_spirv(s);
}

// ---- use masks (see the cache key at the top)

// The slots of the int array `"name": [...]` below 32 as a mask.
static uint32_t json_slot_mask(const char *js, const char *name) {
    const char *end = js + strlen(js), *p = json_key(js, end, name);
    uint32_t m = 0;
    if (!p || *p != '[') return 0;
    for (p++; p < end && *p != ']';) {
        char *q;
        long v = strtol(p, &q, 10);
        if (q == p) {
            p++;
            continue;
        }
        if (v >= 0 && v < 32) m |= 1u << v;
        p = q;
    }
    return m;
}

static UseMask use_of(const GfxShader *s) {
    UseMask m = {json_slot_mask(s->reflection, "tex_queried") | json_slot_mask(s->reflection, "img_queried"), 0, 0};
    if (s->stage == GFX_STAGE_VS) m.attrs = s->in_mask;
    else if (s->stage == GFX_STAGE_FS) m.fouts = s->out_mask & 0xff;
    return m;
}

// <sha256("TKSU", translator identity, code hash)>.use: masks are per translator.
static void use_name(const uint8_t h[32], uint8_t name[32]) {
    uint8_t buf[4 + 32 + 32];
    memcpy(buf, "TKSU", 4), memcpy(buf + 4, xlate_id, 32), memcpy(buf + 36, h, 32);
    rt_sha256(buf, sizeof buf, name);
}

static void use_path_of(const uint8_t name[32], char *p) {
    char hex[65];
    key_hex(name, hex);
    snprintf(p, PATH_CAP + 80, "%s/%s.use", cache_dir, hex);
}

static void use_path(const uint8_t h[32], char *p) {
    uint8_t name[32];
    use_name(h, name);
    use_path_of(name, p);
}

// Every .use file of the cache, read by a background thread when the paths are set (use_preload), by file name: a
// lookup on the render thread then opens no file. Opening one took 0.1 ms alone but 11-40 ms in play, behind the
// translator's and the pipeline cache's writes and flushes on the same volume, and each new shader of a draw opened one.
// The table is complete and read-only once use_files_ready is 1; masks found later are in use_buckets.
#define USE_FILE_BUCKETS 65536
typedef struct UseFile { struct UseFile *next; uint8_t name[32]; UseMask m; } UseFile;
static UseFile *use_files[USE_FILE_BUCKETS];
static volatile int32_t use_files_ready;

static UseFile **use_file_slot(const uint8_t name[32]) {
    UseFile **e = &use_files[(name[0] | name[1] << 8) % USE_FILE_BUCKETS];
    while (*e && memcmp((*e)->name, name, 32)) e = &(*e)->next;
    return e;
}

static int hex_byte(const char *c) {
    int v = 0;
    for (int i = 0; i < 2; i++) {
        int d = isdigit((unsigned char)c[i]) ? c[i] - '0' : c[i] >= 'a' && c[i] <= 'f' ? c[i] - 'a' + 10 : -1;
        if (d < 0) return -1;
        v = v << 4 | d;
    }
    return v;
}

typedef struct { int n, bad; } UsePreload;

// One <64 hex digits>.use file of the cache directory into the table.
static int use_preload_file(const char *name, void *ctx) {
    UsePreload *st = ctx;
    char p[PATH_CAP + 80];
    UseFile e = {0};
    int ok = strlen(name) == 68;
    for (int i = 0; ok && i < 32; i++) {
        int v = hex_byte(name + 2 * i);
        ok = v >= 0, e.name[i] = (uint8_t)v;
    }
    uint64_t len = 0;
    uint32_t *u = NULL;
    if (ok) {
        snprintf(p, sizeof p, "%s/%s", cache_dir, name);
        u = (uint32_t *)read_file(p, &len);
        ok = u && len == 20 && u[0] == USE_MAGIC && u[1] == USE_VERSION;
    }
    UseFile **slot = ok ? use_file_slot(e.name) : NULL, *x;
    if (slot && !*slot && (x = malloc(sizeof *x))) {
        e.m.slots = u[2], e.m.attrs = u[3], e.m.fouts = u[4];
        *x = e, *slot = x, st->n++;
    } else if (!ok) {
        st->bad++;
    }
    free(u);
    return 1;
}

static void use_preload(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    uint64_t t0 = plat_now_ns();
    UsePreload st = {0};
    plat_file_list(cache_dir, ".use", use_preload_file, &st);
    int n = st.n, bad = st.bad;
    plat_xchg32(&use_files_ready, 1);
    rt_log("gfx_shader: %d use masks read in %.0f ms on a background thread (%d unreadable)", n,
           (plat_now_ns() - t0) / 1e6, bad);
    return;
}

static void use_preload_start(void) {
    if (!cache_dir[0]) return;
    plat_thread_spawn(use_preload, NULL, "use preload", 0);
}

static UseEnt **use_slot(const uint8_t h[32]) {         // caller holds cache_lock
    UseEnt **e = &use_buckets[(h[0] | h[1] << 8) % BUCKETS];
    while (*e && memcmp((*e)->code_hash, h, 32)) e = &(*e)->next;
    return e;
}

// Sets or adds the entry for h. Caller holds cache_lock exclusively.
static void use_put(const uint8_t h[32], int known, const UseMask *m) {
    UseEnt **e = use_slot(h);
    if (!*e && !(*e = calloc(1, sizeof **e))) return;
    memcpy((*e)->code_hash, h, 32);
    if (known) (*e)->known = 1, (*e)->m = *m;
}

// 1 with *m when the use mask of h is known (memory, then <hash>.use; a disk miss is remembered).
static int use_find(const uint8_t h[32], UseMask *m) {
    plat_mutex_lock_shared(&cache_lock);
    UseEnt *e = *use_slot(h);
    int seen = e != NULL, known = e && e->known;
    if (known) *m = e->m;
    plat_mutex_unlock_shared(&cache_lock);
    if (seen) return known;
    uint8_t name[32];
    use_name(h, name);
    if (plat_cas32(&use_files_ready, 0, 0)) {   // complete: a name not in it has no file
        UseFile *u = *use_file_slot(name);
        if ((known = u != NULL)) *m = u->m;
    } else {
        char p[PATH_CAP + 80];
        use_path_of(name, p);
        uint64_t n = 0;
        uint32_t *f = (uint32_t *)read_file(p, &n);
        known = f && n == 20 && f[0] == USE_MAGIC && f[1] == USE_VERSION;
        if (known) m->slots = f[2], m->attrs = f[3], m->fouts = f[4];
        free(f);
    }
    plat_mutex_lock(&cache_lock);
    use_put(h, known, m);
    plat_mutex_unlock(&cache_lock);
    return known;
}

static void use_store(const uint8_t h[32], const UseMask *m) {
    char p[PATH_CAP + 80];
    use_path(h, p);
    uint32_t f[5] = {USE_MAGIC, USE_VERSION, m->slots, m->attrs, m->fouts};
    plat_file_write_atomic(p, f, sizeof f);
    plat_mutex_lock(&cache_lock);
    use_put(h, 1, m);
    plat_mutex_unlock(&cache_lock);
}

static void put_u32(uint8_t **p, uint32_t v) { memcpy(*p, &v, 4), *p += 4; }

// Fixed-layout answers for hashing; returns the byte count (at most 256).
static size_t pack_answers(uint8_t *o, const GfxShaderAnswers *a) {
    uint8_t *p = o;
    GfxShaderAnswers zero = {0};
    if (!a) a = &zero;
    memcpy(p, a->tex_type, 32), p += 32;
    memcpy(p, a->img_type, 32), p += 32;
    memcpy(p, a->img_format, 32), p += 32;
    memcpy(p, a->attr_type, 32), p += 32;
    memcpy(p, a->frag_out_type, 8), p += 8;
    for (int i = 0; i < 3; i++) *p++ = (uint8_t)a->local[i], *p++ = (uint8_t)(a->local[i] >> 8);
    put_u32(&p, a->shared_bytes);
    *p++ = (uint8_t)(a->depth_minus_w | (a->mvprev ? 0x80 : 0));
    return (size_t)(p - o);
}

static int build_code_hash(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                           const uint8_t *c1, uint32_t c1_len, uint8_t hash[32]) {
    size_t total = 4 + CONTROL_SIZE + 8 + (size_t)code_size + 4 + c1_len;
    uint8_t *buf = malloc(total), *p = buf;
    if (!buf) return 0;
    put_u32(&p, (uint32_t)stage);
    memcpy(p, control, CONTROL_SIZE), p += CONTROL_SIZE;
    memcpy(p, &code_size, 8), p += 8;
    memcpy(p, code, (size_t)code_size), p += code_size;
    put_u32(&p, c1_len);
    if (c1_len) memcpy(p, c1, c1_len);
    rt_sha256(buf, total, hash);
    free(buf);
    return 1;
}

// The answers with everything outside the mask zeroed; the translator ignores depth_minus_w (Program.cs).
static void mask_answers(const GfxShaderAnswers *a, const UseMask *m, GfxShaderAnswers *o) {
    memset(o, 0, sizeof *o);
    if (!a) return;
    for (int i = 0; i < 32; i++) {
        if (m->slots >> i & 1) o->tex_type[i] = a->tex_type[i], o->img_type[i] = a->img_type[i], o->img_format[i] = a->img_format[i];
        if (m->attrs >> i & 1) o->attr_type[i] = a->attr_type[i];
    }
    for (int i = 0; i < 8; i++)
        if (m->fouts >> i & 1) o->frag_out_type[i] = a->frag_out_type[i];
    memcpy(o->local, a->local, sizeof o->local);
    o->shared_bytes = a->shared_bytes;
    o->mvprev = a->mvprev;   // another translation, not an answer the use mask covers
}

// The cache key of an input id under the current translator.
static void key_of_input(const uint8_t iid[32], uint8_t key[32]) {
    uint8_t buf[8 + 32 + 32], *p = buf;
    memcpy(p, "TKSK", 4), p += 4;
    put_u32(&p, KEY_VERSION);
    memcpy(p, xlate_id, 32), p += 32;
    memcpy(p, iid, 32), p += 32;
    rt_sha256(buf, (size_t)(p - buf), key);
}

// The input id and cache key (m given), or the provisional key (m NULL: full answers; iid untouched).
static void build_key(const uint8_t code_hash[32], const GfxShaderAnswers *a, const UseMask *m, uint8_t key[32],
                      uint8_t iid[32]) {
    GfxShaderAnswers masked;
    uint8_t buf[8 + 32 + 32 + 256], *p = buf;
    if (m) {
        memcpy(p, "TKSI", 4), p += 4;
        put_u32(&p, IN_VERSION);
        memcpy(p, code_hash, 32), p += 32;
        mask_answers(a, m, &masked);
        p += pack_answers(p, &masked);
        rt_sha256(buf, (size_t)(p - buf), iid);
        key_of_input(iid, key);
        return;
    }
    memcpy(p, "TKSP", 4), p += 4;
    put_u32(&p, KEY_VERSION);
    memcpy(p, xlate_id, 32), p += 32;
    memcpy(p, code_hash, 32), p += 32;
    p += pack_answers(p, a);
    rt_sha256(buf, (size_t)(p - buf), key);
}

static size_t build_request(char *o0, const char *hex, const char *stage, const char *code_path, const uint8_t *c1,
                            uint32_t c1_len, const GfxShaderAnswers *a, const uint8_t *control) {
    char *o = o0;
    o += sprintf(o, "{\"job\":{\"key\":\"");
    json_esc(&o, hex);
    o += sprintf(o, "\",\"stage\":\"%s\",\"debug\":false,\"code\":\"", stage);
    json_esc(&o, code_path);
    o += sprintf(o, "\",\"c1\":\"");
    for (uint32_t i = 0; i < c1_len; i++) o += sprintf(o, "%02x", c1[i]);
    o += sprintf(o, "\"");
    if (a) {
        const char *sep = "";
        o += sprintf(o, ",\"tex\":{");
        for (int i = 0; i < 32; i++)
            if (a->tex_type[i] && a->tex_type[i] < COUNT(tex_names)) o += sprintf(o, "%s\"%d\":\"%s\"", sep, i, tex_names[a->tex_type[i]]), sep = ",";
        sep = "";
        o += sprintf(o, "},\"img\":{");
        for (int i = 0; i < 32; i++)
            if (a->img_type[i] && a->img_type[i] < COUNT(tex_names)) {
                const char *f = a->img_format[i] && a->img_format[i] < COUNT(fmt_names) ? fmt_names[a->img_format[i]] : "R8G8B8A8Unorm";
                o += sprintf(o, "%s\"%d\":{\"type\":\"%s\",\"fmt\":\"%s\"}", sep, i, tex_names[a->img_type[i]], f), sep = ",";
            }
        sep = "";
        o += sprintf(o, "},\"attr\":{");
        for (int i = 0; i < 32; i++)
            if (a->attr_type[i]) o += sprintf(o, "%s\"%d\":\"%s\"", sep, i, attr_name(a->attr_type[i])), sep = ",";
        sep = "";
        o += sprintf(o, "},\"fout\":{");
        for (int i = 0; i < 8; i++)
            if (a->frag_out_type[i]) o += sprintf(o, "%s\"%d\":\"%s\"", sep, i, attr_name(a->frag_out_type[i])), sep = ",";
        o += sprintf(o, "},\"shared\":%u", a->shared_bytes);
    }
    if (!strcmp(stage, "cs")) {
        uint32_t l[3] = {1, 1, 1};
        if (a && (a->local[0] | a->local[1] | a->local[2])) l[0] = a->local[0], l[1] = a->local[1], l[2] = a->local[2];
        else memcpy(l, control + 0x748, 12);
        o += sprintf(o, ",\"local\":[%u,%u,%u]", l[0], l[1], l[2]);
    }
    /* Control byte +0x718 of a fragment shader: early fragment tests (the NVN driver's early-Z-force, MacNativeGames
       ae8f4f4). Without it a shader that stores to memory is depth-tested late, and the G-buffer draws' EQUAL test
       after the depth prepass fails along contour lines. Already part of the code hash (the whole control section). */
    if (!strcmp(stage, "ps") && (control[0x718] & 1)) o += sprintf(o, ",\"early_fragment_tests\":true");
    int si = !strcmp(stage, "vs") ? 0 : !strcmp(stage, "cs") ? 2 : 1;   // gfx_shader_set_host answers
    if (!host_f64) o += sprintf(o, ",\"softfloat64\":true");
    if (a && a->mvprev && !strcmp(stage, "vs")) o += sprintf(o, ",\"mvprev\":true");
    if (host_sbuf) o += sprintf(o, ",\"sbuf_align\":%u", host_sbuf);
    if (host_sg[si] != 32) o += sprintf(o, ",\"subgroup\":%u", host_sg[si]);
    if (host_gather) o += sprintf(o, ",\"gather_bias\":%u", host_gather);
    o += sprintf(o, "},\"out\":\"");
    json_esc(&o, cache_dir);
    o += sprintf(o, "\"}");
    return (size_t)(o - o0);
}

// ---- public

// Raw line exchange for the probe: one line to the service, its reply in `reply`. Starts the service if needed.
int gfx_shader_raw_request(const char *line, char *reply, size_t reply_size) {
    int away = kern_core_away();
    Svc *v = svc_get(0);
    int ok = child_exchange(v, line, reply, reply_size);
    if (!ok) child_stop(v);
    svc_put(v);
    kern_core_back(&away);
    return ok;
}

uint64_t gfx_shader_failures(void) { return (uint64_t)n_failures; }
uint64_t gfx_shader_requests(void) { return (uint64_t)n_requests; }   // lines sent to the service
uint64_t gfx_shader_cache_hits(void) { return (uint64_t)n_hits; }     // gets answered from memory or disk

// Drops the memory cache (the probe uses it to prove the disk cache). Pointers handed out before are invalid.
void gfx_shader_drop_memory(void) {
    plat_mutex_lock(&cache_lock);
    for (int i = 0; i < BUCKETS; i++) {
        for (GfxShader *s = buckets[i], *n; s; s = n) {
            n = s->next;
            shader_free(s);
        }
        buckets[i] = NULL;
        n_keys = 0;
        for (UseEnt *e = use_buckets[i], *n; e; e = n) n = e->next, free(e);
        use_buckets[i] = NULL;
    }
    // the preloaded .use table misses the masks this run wrote, which only use_buckets held: back to the disk
    plat_xchg32(&use_files_ready, 0);
    for (int i = 0; i < USE_FILE_BUCKETS; i++) {
        for (UseFile *e = use_files[i], *n; e; e = n) n = e->next, free(e);
        use_files[i] = NULL;
    }
    plat_mutex_unlock(&cache_lock);
}

static const GfxShader *fail(const char *why, const char *hex) {
    plat_inc64(&n_failures);
    rt_log("gfx_shader: translation failed (%s): %s", hex ? hex : "-", why);
    return NULL;
}

// A validated request: the program's code length, its c1 constants and the cache key. 0 (failure counted) when invalid.
typedef struct {
    const char *stage_name;
    uint32_t length, c1_len;
    const uint8_t *c1;
    uint8_t code_hash[32];
    uint8_t key[32];                                       // cache key, or the provisional one with final 0
    uint8_t iid[32];                                       // input id with final 1
    int final;
} Req;

static int req_init(Req *r, int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size, const uint8_t *c1,
                    const GfxShaderAnswers *a) {
    r->stage_name = stage == GFX_STAGE_VS ? "vs" : stage == GFX_STAGE_FS ? "ps" : stage == GFX_STAGE_CS ? "cs" : NULL;
    if (!r->stage_name) return fail("stage is not translated", NULL), 0;
    uint32_t c1_off;
    memcpy(&c1_off, control + 0x700, 4), memcpy(&r->length, control + 0x704, 4);
    uint64_t skip = stage == GFX_STAGE_CS ? 0 : CODE_SKIP;
    if (!code || r->length > code_size || c1_off > r->length || skip >= r->length) return fail("bad control section", NULL), 0;
    r->c1_len = r->length - c1_off;
    r->c1 = c1 ? c1 : code + c1_off;
    if (!build_code_hash(stage, control, code, r->length, r->c1, r->c1_len, r->code_hash))
        return fail("out of memory", NULL), 0;
    paths();
    UseMask m;
    r->final = use_find(r->code_hash, &m);
    build_key(r->code_hash, a, r->final ? &m : NULL, r->key, r->iid);
    return 1;
}

static const GfxShader *translate(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                  const GfxShaderAnswers *a);

static void ans_note(const Req *r, int stage, const uint8_t *control, const uint8_t *code, const GfxShaderAnswers *a,
                     const GfxShader *s);
static const GfxShader *get_async_one(Req r, int stage, const uint8_t *control, const uint8_t *code,
                                      const GfxShaderAnswers *a, int *pending);

static const GfxShader *get_one(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                const GfxShaderAnswers *a) {
    GfxShader *s = mem_find(r->key);
    if (s) return plat_inc64(&n_hits), s;
    paths();
    if (!cache_dir[0]) return fail("no cache directory", NULL);
    if (r->final && (s = disk_load(r->key, r->iid))) return plat_inc64(&n_hits), mem_insert(s);
    return translate(r, stage, control, code, a);
}

const GfxShader *gfx_shader_get(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                const uint8_t *c1, const GfxShaderAnswers *a) {
    Req r;
    if (!req_init(&r, stage, control, code, code_size, c1, a)) return NULL;
    const GfxShader *s = get_one(&r, stage, control, code, a);
    if (s) ans_note(&r, stage, control, code, a, s);
    return s;
}

// One seed variant into the disk cache, for the background preparation: its input id in iid, 1 on success. Unlike
// gfx_shader_get it neither records the pair as met in play (the seed would fill shader-answers.bin and
// shader-programs.bin, which every boot reads whole, and rank first in the boot prebuild's warm list) nor keeps the
// shader in memory.
static void in_store(const uint8_t iid[32], int stage, const uint8_t *control, const uint8_t *code, const Req *r,
                     const GfxShaderAnswers *a);

static int prepare_private(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size, const uint8_t *c1,
                           const GfxShaderAnswers *a, uint8_t iid[32]) {
    Req r;
    if (!req_init(&r, stage, control, code, code_size, c1, a)) return 0;
    const GfxShader *s = mem_find(r.key);
    if (s) return memcpy(iid, s->iid, 32), 1;
    paths();
    if (!cache_dir[0]) return fail("no cache directory", NULL), 0;
    GfxShader *d = r.final ? disk_load(r.key, r.iid) : NULL;   // translated before (an earlier, unfinished run)
    if (d) {   // its stored inputs too, which the prepared list's checks look for (prepared_current, prepared_intact)
        in_store(d->iid, stage, control, code, &r, a);
        return memcpy(iid, d->iid, 32), shader_free(d), 1;
    }
    // normal priority while the translation is in flight: a game draw that needs the same shader waits on this one
    // (translate's flight entry, held for the child's round trip), so it must not wait for CPU behind the game
    plat_thread_priority(PLAT_PRIO_NORMAL);
    tl_private = 1, tl_owned = 0;
    s = translate(&r, stage, control, code, a);
    tl_private = 0;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    if (!s) return 0;
    memcpy(iid, s->iid, 32);
    if (tl_owned) shader_free((GfxShader *)s);
    return 1;
}

// ---- stored inputs (<cache>/in/<input id>.in, see the top)

typedef struct {
    uint32_t magic, version, stage, length, c1_len, answers_size;
} InHeader;                                                // then GfxShaderAnswers, control, code[length], c1[c1_len]

typedef struct {
    uint8_t *buf;                                          // the file; the pointers below point into it
    int stage;
    uint32_t length, c1_len;
    const uint8_t *control, *code, *c1;
    GfxShaderAnswers a;
} Inputs;

static void in_path(const uint8_t iid[32], char *p) {
    char hex[65];
    key_hex(iid, hex);
    snprintf(p, PATH_CAP + 80, "%s/in/%s.in", cache_dir, hex);
}

// Stores the inputs of a translation unless they are stored already; the file appears whole (written, then renamed).
static void in_store(const uint8_t iid[32], int stage, const uint8_t *control, const uint8_t *code, const Req *r,
                     const GfxShaderAnswers *a) {
    char p[PATH_CAP + 80], tmp[PATH_CAP + 120];
    in_path(iid, p);
    if (file_exists(p)) return;
    GfxShaderAnswers zero = {0};
    InHeader h = {IN_MAGIC, IN_VERSION, (uint32_t)stage, r->length, r->c1_len, sizeof(GfxShaderAnswers)};
    size_t n = sizeof h + sizeof zero + CONTROL_SIZE + r->length + r->c1_len;
    uint8_t *buf = malloc(n), *o = buf;
    if (!buf) return;
    memcpy(o, &h, sizeof h), o += sizeof h;
    memcpy(o, a ? a : &zero, sizeof zero), o += sizeof zero;
    memcpy(o, control, CONTROL_SIZE), o += CONTROL_SIZE;
    memcpy(o, code, r->length), o += r->length;
    if (r->c1_len) memcpy(o, r->c1, r->c1_len);
    snprintf(tmp, PATH_CAP + 120, "%s/in", cache_dir);
    mkdir_p(tmp);
    plat_file_write_atomic(p, buf, n);
    free(buf);
}

// Reads stored inputs into *in (free in->buf); 0 when absent or malformed.
static int in_load(const uint8_t iid[32], Inputs *in) {
    char p[PATH_CAP + 80];
    in_path(iid, p);
    uint64_t n = 0;
    uint8_t *f = read_file(p, &n);
    InHeader h;
    if (!f || n < sizeof h) return free(f), 0;
    memcpy(&h, f, sizeof h);
    if (h.magic != IN_MAGIC || h.version != IN_VERSION || h.answers_size != sizeof(GfxShaderAnswers) ||
        n != sizeof h + sizeof(GfxShaderAnswers) + CONTROL_SIZE + (uint64_t)h.length + h.c1_len)
        return free(f), 0;
    const uint8_t *o = f + sizeof h;
    memcpy(&in->a, o, sizeof in->a), o += sizeof in->a;
    in->control = o, o += CONTROL_SIZE;
    in->code = o, o += h.length;
    in->c1 = o;
    in->buf = f, in->stage = (int)h.stage, in->length = h.length, in->c1_len = h.c1_len;
    return 1;
}

// ---- translation

// translate_one's result: into the memory cache, or to the caller of prepare_private (tl_private), who frees it.
static GfxShader *hand_back(GfxShader *s) {
    if (!tl_private) return mem_insert(s);
    tl_owned = 1;
    return s;
}

static const GfxShader *translate_one(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                      const GfxShaderAnswers *a) {
    const uint8_t *key = r->key;
    GfxShader *s;
    UseMask m;
    uint8_t mk[32], miid[32];
    // Another thread may have translated it while this one waited in flight_enter.
    const uint8_t *look = r->final ? key : NULL, *look_iid = r->iid;
    if (!r->final && use_find(r->code_hash, &m)) build_key(r->code_hash, a, &m, mk, miid), look = mk, look_iid = miid;
    if (look && (s = mem_find(look))) return plat_inc64(&n_hits), s;
    if (look && (s = disk_load(look, look_iid))) {
        // translated before (a pass stopped halfway): the preparation's checks look for the stored inputs too
        // (prepared_current, installer/shaders.py prepared_intact); play keeps its old path
        if (tl_private) in_store(s->iid, stage, control, code, r, a);
        return plat_inc64(&n_hits), hand_back(s);
    }

    uint64_t skip = stage == GFX_STAGE_CS ? 0 : CODE_SKIP;
    char hex[65], code_path[PATH_CAP + 80], spv_path[PATH_CAP + 80], res_path[PATH_CAP + 80];
    key_hex(key, hex);
    mkdir_p(cache_dir);
    snprintf(code_path, PATH_CAP + 80, "%s/%s.code", cache_dir, hex);
    snprintf(spv_path, PATH_CAP + 80, "%s/%s.spv", cache_dir, hex);
    snprintf(res_path, PATH_CAP + 80, "%s/%s.res.json", cache_dir, hex);
    if (!write_file(code_path, code + skip, (size_t)(r->length - skip))) return fail("cannot write the code file", hex);
    size_t cap = 2 * PATH_CAP * 6 + 2 * r->c1_len + 8192;
    char *req = malloc(cap), reply[LINE_MAX_LEN];
    if (!req) return plat_file_delete(code_path), fail("out of memory", hex);
    build_request(req, hex, r->stage_name, code_path, r->c1, r->c1_len, a, control);
    int ok = svc_request(req, reply, sizeof reply);
    // XCX_XLATE_KEEP=1: a last-frame variant's job stays as <key>.job (one request line) with its code, to run the
    // translator on by hand (totk_xlate --serve < <key>.job)
    static int keep = -1;
    if (keep < 0) keep = rt_knob("XLATE_KEEP", "0");
    int kept = keep && a && a->mvprev;
    if (kept) {
        char job_path[PATH_CAP + 80];
        snprintf(job_path, sizeof job_path, "%s/%s.job", cache_dir, hex);
        write_file(job_path, req, strlen(req));
    }
    free(req);
    if (!kept) plat_file_delete(code_path);
    if (!ok) return fail("translator unavailable", hex);
    if (strncmp(reply, "ok ", 3)) {
        plat_file_delete(spv_path), plat_file_delete(res_path);
        return fail(reply, hex);
    }
    plat_file_flush(spv_path), plat_file_flush(res_path);   // the translator wrote them; on disk before use or rename
    if (!(s = disk_load(key, r->iid))) return fail("translator output missing or malformed", hex);
    if (!r->final) {                                       // name the files after the cache key, remember the mask
        m = use_of(s);
        build_key(r->code_hash, a, &m, mk, miid);
        char mhex[65], to[PATH_CAP + 80];
        key_hex(mk, mhex);
        snprintf(to, PATH_CAP + 80, "%s/%s.spv", cache_dir, mhex);
        int moved = plat_file_replace(spv_path, to);
        snprintf(to, PATH_CAP + 80, "%s/%s.res.json", cache_dir, mhex);
        moved = moved && plat_file_replace(res_path, to);
        if (moved) {
            use_store(r->code_hash, &m);
        } else {                                           // keep the mask for this run only
            plat_file_delete(spv_path), plat_file_delete(res_path), rt_log("gfx_shader: cannot rename %s", hex);
            plat_mutex_lock(&cache_lock);
            use_put(r->code_hash, 1, &m);
            plat_mutex_unlock(&cache_lock);
        }
        memcpy(s->key, mk, 32), memcpy(s->iid, miid, 32);
    }
    in_store(s->iid, stage, control, code, r, a);
    plat_inc64(&n_xlated);
    return hand_back(s);
}

// The service round trip for a request that is in neither cache. One translation per cache key (per code hash while
// the use mask is unknown) runs at a time; different shaders run in parallel on the children.
static const GfxShader *translate(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                  const GfxShaderAnswers *a) {
    int away = kern_core_away();
    const uint8_t *id = r->final ? r->key : r->code_hash;
    flight_enter(id);
    const GfxShader *s = translate_one(r, stage, control, code, a);
    flight_leave(id);
    kern_core_back(&away);
    return s;
}

// ---- background translation (gfx_shader_get_async)
// svc_runtime_limit() workers, one per translator child. A job owns copies of the control section and the code, so the
// guest may free or rewrite its program while the job waits. A key stays in X.waits while its job is queued or running,
// and for good when the translation failed, so a failing shader is not sent again on every draw.
typedef struct XJob {
    struct XJob *next;
    int stage;
    Req r;                                                 // r.c1 points into code
    uint8_t *control, *code;
    GfxShaderAnswers a;
} XJob;

typedef struct XWait {
    struct XWait *next;
    uint8_t key[32];
    int failed;
} XWait;

static struct {
    PlatMutex lock;
    PlatCond work, done;                         // done: a job finished (gfx_shader_await)
    XJob *head, *tail;
    XWait *waits[256];
    int started;                                           // 1 running, -1 no thread could be created
} X = {PLAT_MUTEX_INIT, PLAT_COND_INIT, PLAT_COND_INIT};

static volatile int64_t n_async;

// Caller holds X.lock.
static XWait **wait_slot(const uint8_t key[32]) {
    XWait **w = &X.waits[key[0]];
    while (*w && memcmp((*w)->key, key, 32)) w = &(*w)->next;
    return w;
}

static void xlate_worker(void *arg) {
    (void)arg;
    for (;;) {
        plat_mutex_lock(&X.lock);
        while (!X.head) plat_cond_wait(&X.work, &X.lock);
        XJob *j = X.head;
        if (!(X.head = j->next)) X.tail = NULL;
        plat_mutex_unlock(&X.lock);
        const GfxShader *s = translate(&j->r, j->stage, j->control, j->code, &j->a);
        plat_mutex_lock(&X.lock);
        XWait **w = wait_slot(j->r.key), *e = *w;
        if (e && s) *w = e->next, free(e);
        else if (e) e->failed = 1;
        plat_mutex_unlock(&X.lock);
        plat_cond_broadcast(&X.done);
        if (s) plat_inc64(&n_async);
        free(j->control), free(j->code), free(j);
    }
    return;
}
const GfxShader *gfx_shader_get_async(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                      const GfxShaderAnswers *a, int *pending) {
    *pending = 0;
    Req r;
    if (!req_init(&r, stage, control, code, code_size, NULL, a)) return NULL;
    const GfxShader *s = get_async_one(r, stage, control, code, a, pending);
    if (s) ans_note(&r, stage, control, code, a, s);
    return s;
}

static const GfxShader *get_async_one(Req r, int stage, const uint8_t *control, const uint8_t *code,
                                      const GfxShaderAnswers *a, int *pending) {
    GfxShader *s = mem_find(r.key);
    if (s) return plat_inc64(&n_hits), s;
    plat_mutex_lock_shared(&X.lock);
    XWait *w = *wait_slot(r.key);
    int failed = w && w->failed;
    plat_mutex_unlock_shared(&X.lock);
    if (w) return *pending = !failed, NULL;
    paths();
    if (!cache_dir[0]) return fail("no cache directory", NULL);
    if (r.final && (s = disk_load(r.key, r.iid))) return plat_inc64(&n_hits), mem_insert(s);

    XJob *j = calloc(1, sizeof *j);
    XWait *e = calloc(1, sizeof *e);
    uint8_t *ctl = malloc(CONTROL_SIZE), *cd = malloc(r.length);
    plat_mutex_lock(&X.lock);
    if (!X.started) {
        int n = 0;
        for (int i = 0; i < svc_runtime_limit(); i++) {
            if (plat_thread_spawn(xlate_worker, NULL, "xlate", 0)) n++;
        }
        X.started = n ? 1 : -1;
        if (n) rt_log("gfx_shader: graphics shaders are translated by %d background workers (XCX_XLATE_PROCS)", n);
        else rt_log("gfx_shader: cannot start the translation workers; translating synchronously");
    }
    XWait **slot = wait_slot(r.key);
    int raced = *slot != NULL;                             // another thread queued it between the checks
    int queued = !raced && X.started == 1 && j && e && ctl && cd;
    if (queued) {
        memcpy(ctl, control, CONTROL_SIZE), memcpy(cd, code, r.length);
        *j = (XJob){.stage = stage, .r = r, .control = ctl, .code = cd, .a = *a};
        j->r.c1 = cd + (r.length - r.c1_len);
        memcpy(e->key, r.key, 32);
        *slot = e;
        if (X.tail) X.tail->next = j;
        else X.head = j;
        X.tail = j;
        plat_cond_signal(&X.work);
    }
    plat_mutex_unlock(&X.lock);
    if (queued) return *pending = 1, NULL;
    free(j), free(e), free(ctl), free(cd);
    if (raced) return *pending = 1, NULL;
    return translate(&r, stage, control, code, a);         // no worker or out of memory
}

const GfxShader *gfx_shader_await(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                  const GfxShaderAnswers *a) {
    int pending;
    const GfxShader *s = gfx_shader_get_async(stage, control, code, code_size, a, &pending);
    Req r;
    if (!pending || !req_init(&r, stage, control, code, code_size, NULL, a)) return s;
    uint64_t p0 = perf_begin();   // the wait counts as the render thread's translation time
    plat_mutex_lock(&X.lock);
    for (XJob *j = X.head, *prev = NULL; j; prev = j, j = j->next)
        if (!memcmp(j->r.key, r.key, 32)) {                // queued: to the front, the worker takes it next
            if (prev) {
                prev->next = j->next;
                if (X.tail == j) X.tail = prev;
                j->next = X.head, X.head = j;
            }
            break;
        }
    XWait *w;
    while ((w = *wait_slot(r.key)) && !w->failed) plat_cond_wait(&X.done, &X.lock);
    plat_mutex_unlock(&X.lock);
    perf_end(PERF_XLATE, p0);
    if (w) return NULL;                                    // failed: logged by the worker
    s = gfx_shader_get_async(stage, control, code, code_size, a, &pending);
    return pending ? NULL : s;
}

uint64_t gfx_shader_async_done(void) { return (uint64_t)n_async; }

void gfx_shader_counts(uint64_t *keys, uint64_t *translated) { *keys = (uint64_t)n_keys, *translated = (uint64_t)n_xlated; }

const uint8_t *gfx_shader_key(const GfxShader *s) { return s->key; }

const uint8_t *gfx_shader_input_id(const GfxShader *s) { return s->iid; }

// By input id: memory, then the disk cache under the current translator, then (xlate 1) a translation from the stored
// inputs. NULL when none has it; a failed translation is counted as a failure.
const GfxShader *gfx_shader_load_input(const uint8_t iid[32], int xlate) {
    paths();
    if (!cache_dir[0]) return NULL;
    uint8_t key[32];
    key_of_input(iid, key);
    GfxShader *s = mem_find(key);
    if (s) return s;
    if ((s = disk_load(key, iid))) return mem_insert(s);
    Inputs in;
    if (!xlate || !in_load(iid, &in)) return NULL;
    const GfxShader *t = gfx_shader_get(in.stage, in.control, in.code, in.length, in.c1, &in.a);
    free(in.buf);
    return t;
}

// From the disk cache only, as a shader the caller owns and releases with gfx_shader_release_private (the boot library
// prebuild warms the driver's pipeline cache with it without keeping 40 KB per shader in memory). When the memory
// cache has it, that entry is returned with *owned 0 and must not be released. NULL when not cached.
const GfxShader *gfx_shader_load_private(const uint8_t iid[32], int *owned) {
    *owned = 0;
    paths();
    if (!cache_dir[0]) return NULL;
    uint8_t key[32];
    key_of_input(iid, key);
    GfxShader *s = mem_find(key);
    if (s) return s;
    if ((s = disk_load(key, iid))) *owned = 1;
    return s;
}

void gfx_shader_release_private(const GfxShader *s) {
    if (s) shader_free((GfxShader *)s);
}

// ---- boot prewarm (gfx_shader_prewarm)

typedef struct {
    const uint8_t (*named)[32];                            // loaded into memory, results in out
    size_t n_named;
    const GfxShader **out;
    uint8_t (*other)[32];                                  // other stored inputs: translated when not cached, not loaded
    size_t n_other;
    volatile int64_t next, done;
    volatile int32_t missing, other_cached;
} Warm;

static void warm_worker(void *arg) {
    Warm *w = arg;
    for (;;) {
        size_t i = (size_t)plat_inc64(&w->next) - 1;
        if (i >= w->n_named + w->n_other) return;
        if (i < w->n_named) {
            if (!(w->out[i] = gfx_shader_load_input(w->named[i], 1))) plat_inc32(&w->missing);
        } else {
            const uint8_t *iid = w->other[i - w->n_named];
            uint8_t key[32];
            char hex[65], p[PATH_CAP + 80];
            key_of_input(iid, key);
            key_hex(key, hex);
            snprintf(p, PATH_CAP + 80, "%s/%s.spv", cache_dir, hex);
            if (file_exists(p)) plat_inc32(&w->other_cached);
            else gfx_shader_load_input(iid, 1);
        }
        plat_inc64(&w->done);
    }
}

static int iid_cmp(const void *x, const void *y) { return memcmp(x, y, 32); }

typedef struct {
    const uint8_t (*named)[32];
    size_t n_named, n, cap;
    uint8_t (*v)[32];
    const uint8_t (*skip)[32];   // sorted: the prepared list's ids (other_inputs)
    size_t n_skip;
} InList;

static int in_collect(const char *file, void *ctx) {
    InList *l = ctx;
    char name[80];
    uint8_t iid[32];
    if (strlen(file) != 67) return 1;                      // 64 hex digits + ".in"
    memcpy(name, file, 64);
    name[64] = 0;
    if (!hex32(name, iid) || (l->n_named && bsearch(iid, l->named, l->n_named, 32, iid_cmp))) return 1;
    if (l->n_skip && bsearch(iid, l->skip, l->n_skip, 32, iid_cmp)) return 1;
    if (l->n == l->cap) {
        size_t c = l->cap ? l->cap * 2 : 1024;
        uint8_t (*t)[32] = realloc(l->v, c * 32);
        if (!t) return 0;
        l->v = t, l->cap = c;
    }
    memcpy(l->v[l->n++], iid, 32);
    return 1;
}

// The stored inputs (ids parsed from the in/ file names) not named and not in the prepared list: the seed's ~25,000 variants are the background pass's,
// which handles a new translator identity privately and off the start; the prewarm, which the guest waits for,
// translates in the foreground and records what it makes as met in play.
static size_t prepared_ids(uint8_t (**out)[32]);   // the prepared list's input ids, sorted (after the list's format)

static size_t other_inputs(const uint8_t (*named)[32], size_t n_named, uint8_t (**out)[32]) {
    char dir[PATH_CAP + 80];
    snprintf(dir, sizeof dir, "%s/in", cache_dir);
    InList l = {.named = named, .n_named = n_named};
    uint8_t (*skip)[32] = NULL;
    l.n_skip = prepared_ids(&skip), l.skip = (const uint8_t (*)[32])skip;
    plat_file_list(dir, ".in", in_collect, &l);
    free(skip);
    return *out = l.v, l.n;
}

void gfx_shader_prewarm(const uint8_t (*iids)[32], size_t n, const GfxShader **out, int threads, int procs) {
    paths();
    double t0 = (double)plat_now_ms();
    memset(out, 0, n * sizeof *out);
    if (!cache_dir[0]) return;
    uint64_t xl0 = (uint64_t)n_xlated, fail0 = (uint64_t)n_failures;
    uint8_t (*sorted)[32] = n ? malloc(n * 32) : NULL;
    if (sorted) memcpy(sorted, iids, n * 32), qsort(sorted, n, 32, iid_cmp);
    Warm w = {.named = iids, .n_named = n, .out = out};
    w.n_other = other_inputs(sorted, sorted ? n : 0, &w.other);
    free(sorted);
    size_t total = w.n_named + w.n_other;
    threads = threads < 1 ? 1 : threads > 64 ? 64 : threads;
    if ((size_t)threads > total) threads = (int)(total ? total : 1);
    svc_set_limit(procs);
    PlatThread *t[64];
    int k = 0;
    for (int i = 0; i < threads; i++)
        if ((t[k] = plat_thread_new(warm_worker, &w, "shader prewarm"))) k++;
    if (!k) warm_worker(&w);
    for (int m = 0; m < k; m++)
        while (!plat_thread_join_ns(t[m], 2000000000u))
            rt_log("gfx_shader: prewarm: %lld of %zu shaders, %llu translated, %.0f ms", (long long)w.done, total,
                   (unsigned long long)((uint64_t)n_xlated - xl0), (double)plat_now_ms() - t0);
    svc_set_limit(svc_runtime_limit());
    rt_log("gfx_shader: prewarm: %zu shaders named by pipeline keys (%ld not available), %zu other stored inputs (%ld "
           "cached); %llu translated, %llu failed, with up to %d translators and %d threads, in %.0f ms", w.n_named,
           (long)w.missing, w.n_other, (long)w.other_cached, (unsigned long long)((uint64_t)n_xlated - xl0),
           (unsigned long long)((uint64_t)n_failures - fail0), procs, k, (double)plat_now_ms() - t0);
    free(w.other);
}
const uint32_t *gfx_shader_spirv(const GfxShader *s, uint64_t *words) {
    if (words) *words = s->words;
    return s->spirv;
}

int gfx_shader_stage(const GfxShader *s) { return s->stage; }

int gfx_shader_resources(const GfxShader *s, int kind, const GfxShaderRes **res) {
    if (kind < 0 || kind >= GFX_RES_KINDS) return 0;
    if (res) *res = s->res[kind];
    return s->nres[kind];
}

// The slots of the use mask: answers outside it select the same translation (mask_answers), so a caller may leave
// their texture and image types zero. Parses the reflection: callers keep the result.
uint32_t gfx_shader_slot_mask(const GfxShader *s) { return use_of(s).slots; }

uint32_t gfx_shader_inputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->in, sizeof s->in);
    return s->in_mask;
}

uint32_t gfx_shader_mvprev_banks(const GfxShader *s) { return s->mvprev_banks; }

uint32_t gfx_shader_outputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->out, sizeof s->out);
    return s->out_mask;
}

void gfx_shader_local_size(const GfxShader *s, uint32_t local[3]) { memcpy(local, s->local, sizeof s->local); }

// ---- answers met in play
// The translated-shader key includes the answers a draw's state implies, so a translation made without play (the
// census) does not match the keys play asks for. Every (program, answers) that a gfx_shader_get* call resolves is
// recorded next to pipeline-keys.bin, so the first-start preparation (Task 2.6, here the XCX_TEST_PREPARE_SHADERS hook)
// translates exactly the variants play needs, from these files alone:
//   shader-answers.bin  "XCXSANS", version, record size; records: stage (u32), code hash, input id, pack_answers bytes
//                       of the full answers, zero padding. One record per input id: answers outside the shader's use
//                       mask do not change its SPIR-V, are out of its input id and cache key (mask_answers) and so do
//                       not make records of their own (a fight run meets 4126 full pairs, 1238 input ids). The full
//                       answers are kept so a translator with a wider mask still gets them. Under a new translator
//                       the masks, hence the input ids, are new, and play appends records for them.
//   shader-programs.bin "XCXSPRG", version, 0; records: stage, length, c1 length (u32 each), code hash, control section,
//                       code[length], c1. One per code hash; written before the answers that name it. Game content:
//                       it stays in the ignored pipeline cache directory.
// A file with another header is rewritten; a torn tail (killed process) is cut off at open.
#define ANS_VERSION 1
#define ANS_PACKED 147                                     // pack_answers' size
#define ANS_REC (4 + 32 + 32 + ANS_PACKED + 1)
#define PRG_VERSION 1
#define PRG_HEAD (12 + 32 + CONTROL_SIZE)
typedef struct { char magic[8]; uint32_t version, rec_size; } RecHeader;
typedef struct { uint64_t *v; uint32_t cap, n; } Set64;    // open addressing, 0 empty
typedef struct { uint8_t *p; size_t n, cap; } Bytes;

static struct {
    PlatMutex lock;
    volatile int32_t on;
    char ans_path[PATH_CAP], prg_path[PATH_CAP];
    Set64 pairs, progs;                                    // input ids, code hashes seen (in the files or this run)
    Bytes ans, prg;                                        // pending
    uint64_t pairs_new;                                    // added this run
} RC = {PLAT_MUTEX_INIT};

static int set_add(Set64 *s, uint64_t h) {                 // 1 if new
    h = h ? h : 1;
    if ((s->n + 1) * 2 > s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 4096;
        uint64_t *t = calloc(cap, sizeof *t);
        if (!t) return 0;
        for (uint32_t i = 0; i < s->cap; i++)
            if (s->v[i]) {
                uint32_t j = (uint32_t)s->v[i] & (cap - 1);
                while (t[j]) j = (j + 1) & (cap - 1);
                t[j] = s->v[i];
            }
        free(s->v), s->v = t, s->cap = cap;
    }
    uint32_t j = (uint32_t)h & (s->cap - 1);
    for (; s->v[j]; j = (j + 1) & (s->cap - 1))
        if (s->v[j] == h) return 0;
    s->v[j] = h, s->n++;
    return 1;
}

static int bytes_put(Bytes *b, const void *p, size_t n) {
    if (!n) return 1;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->n + n) cap *= 2;
        uint8_t *t = realloc(b->p, cap);
        if (!t) return 0;
        b->p = t, b->cap = cap;
    }
    memcpy(b->p + b->n, p, n), b->n += n;
    return 1;
}

static uint64_t hash8(const uint8_t *sha) {
    uint64_t v;
    memcpy(&v, sha, 8);
    return v;
}

static void unpack_answers(const uint8_t *p, GfxShaderAnswers *a) {
    memset(a, 0, sizeof *a);
    memcpy(a->tex_type, p, 32), p += 32;
    memcpy(a->img_type, p, 32), p += 32;
    memcpy(a->img_format, p, 32), p += 32;
    memcpy(a->attr_type, p, 32), p += 32;
    memcpy(a->frag_out_type, p, 8), p += 8;
    for (int i = 0; i < 3; i++) a->local[i] = (uint16_t)(p[0] | p[1] << 8), p += 2;
    memcpy(&a->shared_bytes, p, 4), p += 4;
    a->depth_minus_w = *p & 0x7f, a->mvprev = *p >> 7;
}

// Reads a record file's body (after its header) into *body; 0 when absent, and a file with another header is removed.
// keep: a file this code never writes (the setup's seed): one of another format or version is ignored, not deleted.
static int rec_read_as(const char *path, const char *magic, uint32_t version, uint32_t rec_size, uint8_t **body,
                       size_t *n, int keep) {
    *body = NULL, *n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    RecHeader h;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    int ok = len >= (long)sizeof h && fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, magic, 8) &&
             h.version == version && h.rec_size == rec_size;
    if (ok && len > (long)sizeof h) {
        *n = (size_t)len - sizeof h;
        ok = (*body = malloc(*n)) && fread(*body, 1, *n, f) == *n;
    }
    fclose(f);
    if (!ok) {
        free(*body), *body = NULL, *n = 0;
        if (len > 0 && keep) rt_log("gfx_shader: %s has another format or version; ignored", path);
        else if (len > 0) rt_log("gfx_shader: %s has another format or version; it is rewritten", path), remove(path);
    }
    return ok;
}

static int rec_read(const char *path, const char *magic, uint32_t version, uint32_t rec_size, uint8_t **body, size_t *n) {
    return rec_read_as(path, magic, version, rec_size, body, n, 0);
}

// Walks shader-programs.bin records; returns the byte length of the whole records. fn may be NULL.
static size_t prg_walk(const uint8_t *b, size_t n, void (*fn)(const uint8_t *rec, void *ctx), void *ctx) {
    size_t o = 0;
    while (n - o >= PRG_HEAD) {
        uint32_t len, c1;
        memcpy(&len, b + o + 4, 4), memcpy(&c1, b + o + 8, 4);
        size_t size = PRG_HEAD + (size_t)len + c1;
        if (len > (64u << 20) || c1 > len || size > n - o) break;
        if (fn) fn(b + o, ctx);
        o += size;
    }
    return o;
}

static void prg_seen(const uint8_t *rec, void *ctx) { set_add(ctx, hash8(rec + 12)); }

static void truncate_to(const char *path, size_t size) {
    if (!plat_file_truncate(path, size)) return;
    rt_log("gfx_shader: %s had a torn tail; cut to %zu bytes", path, size);
}

void gfx_shader_record_at(const char *keys_path) {
    if (!keys_path || !keys_path[0] || RC.on) return;
    plat_mutex_lock(&RC.lock);
    const char *slash = strrchr(keys_path, '\\'), *fs = strrchr(keys_path, '/');
    if (fs > slash) slash = fs;
    int dir = slash ? (int)(slash + 1 - keys_path) : 0;
    snprintf(RC.ans_path, sizeof RC.ans_path, "%.*sshader-answers.bin", dir, keys_path);
    snprintf(RC.prg_path, sizeof RC.prg_path, "%.*sshader-programs.bin", dir, keys_path);
    uint8_t *b;
    size_t n;
    if (rec_read(RC.ans_path, "XCXSANS", ANS_VERSION, ANS_REC, &b, &n)) {
        for (size_t o = 0; o + ANS_REC <= n; o += ANS_REC) set_add(&RC.pairs, hash8(b + o + 36));
        if (n % ANS_REC) truncate_to(RC.ans_path, sizeof(RecHeader) + n / ANS_REC * ANS_REC);
    }
    free(b);
    if (rec_read(RC.prg_path, "XCXSPRG", PRG_VERSION, 0, &b, &n)) {
        size_t whole = prg_walk(b, n, prg_seen, &RC.progs);
        if (whole != n) truncate_to(RC.prg_path, sizeof(RecHeader) + whole);
    }
    free(b);
    rt_log("gfx_shader: recording answers met in play to %s (%u records, %u programs recorded before)", RC.ans_path,
           RC.pairs.n, RC.progs.n);
    plat_xchg32(&RC.on, 1);
    plat_mutex_unlock(&RC.lock);
}

// A get resolved (r, a) to s: queue the record when its input id is new, and its program when new. Cost: a set
// lookup under a lock per call; gets happen per (program, answers) change in vk_exec_draw's shader table, not per draw.
static void ans_note(const Req *r, int stage, const uint8_t *control, const uint8_t *code, const GfxShaderAnswers *a,
                     const GfxShader *s) {
    if (!RC.on) return;
    uint32_t st = (uint32_t)stage;
    plat_mutex_lock(&RC.lock);
    if (set_add(&RC.pairs, hash8(s->iid))) {
        uint8_t rec[ANS_REC] = {0};
        memcpy(rec, &st, 4), memcpy(rec + 4, r->code_hash, 32), memcpy(rec + 36, s->iid, 32);
        pack_answers(rec + 68, a);
        if (set_add(&RC.progs, hash8(r->code_hash))) {
            uint32_t head[3] = {st, r->length, r->c1_len};
            bytes_put(&RC.prg, head, 12), bytes_put(&RC.prg, r->code_hash, 32);
            bytes_put(&RC.prg, control, CONTROL_SIZE), bytes_put(&RC.prg, code, r->length);
            bytes_put(&RC.prg, r->c1, r->c1_len);
        }
        bytes_put(&RC.ans, rec, sizeof rec);
        RC.pairs_new++;
    }
    plat_mutex_unlock(&RC.lock);
}

static int rec_append(const char *path, const char *magic, uint32_t version, uint32_t rec_size, const Bytes *b) {
    FILE *f = fopen(path, "ab");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    int ok = 1;
    if (ftell(f) == 0) {
        RecHeader h = {{0}, version, rec_size};
        memcpy(h.magic, magic, 8);
        ok = fwrite(&h, sizeof h, 1, f) == 1;
    }
    ok = ok && fwrite(b->p, 1, b->n, f) == b->n;
    return !fclose(f) && ok;
}

static PlatMutex rec_io = PLAT_MUTEX_INIT;   // the record files: appends in order, no read during an append

void gfx_shader_record_flush(int force) {
    static volatile int64_t last;
    int64_t now = (int64_t)plat_now_ms();
    if (!RC.on || (!force && now - last < 2000)) return;
    last = now;
    // The files are written outside RC.lock, which the render thread takes to record a new answer.
    plat_mutex_lock(&rec_io);
    plat_mutex_lock(&RC.lock);
    Bytes ans = RC.ans, prg = RC.prg;
    RC.ans = (Bytes){0}, RC.prg = (Bytes){0};
    uint64_t total = RC.pairs.n, added = RC.pairs_new;
    plat_mutex_unlock(&RC.lock);
    if (prg.n && !rec_append(RC.prg_path, "XCXSPRG", PRG_VERSION, 0, &prg)) rt_log("gfx_shader: cannot write %s", RC.prg_path);
    if (ans.n && !rec_append(RC.ans_path, "XCXSANS", ANS_VERSION, ANS_REC, &ans)) rt_log("gfx_shader: cannot write %s", RC.ans_path);
    plat_mutex_unlock(&rec_io);
    if (ans.n) rt_log("gfx_shader: answers: %zu new records appended (%llu new this run, %llu recorded)", ans.n / ANS_REC,
                      (unsigned long long)added, (unsigned long long)total);
    free(ans.p), free(prg.p);
}

// ---- preparation from record files: the answers met in play (XCX_TEST_PREPARE_SHADERS=1) or the seed that
// tools/prepare_shaders.py writes from the MacNativeGames shader list (xcx.exe --prepare-shaders). Both use the record
// layouts above; a seed record has a zero input id (the use mask, hence the id, is the translator's to find) and its
// pad byte holds the list priority (0 title, 1 warm-up, 2 rest).
typedef struct { const uint8_t *rec; } PrgIdx;
typedef struct { PrgIdx *idx; size_t n; } PrgList;
typedef struct {
    PrgList progs;
    const uint8_t *ans;
    size_t n_ans;
    uint8_t (*iid)[32];                                    // per answers record: the resolved input id, zero on failure
    volatile int64_t next, done, missing, failed, succeeded;
    volatile int32_t stopped;                              // gave up: PREP_GIVE_UP failures before any success
    int background;                                        // while the game runs (gfx_shader_prepare_start)
} Prep;

// The background preparation's progress for the overlay (gfx_shader_prepare_progress): records done and in all.
static volatile int64_t bg_done, bg_total;
static volatile int32_t bg_running;

static int prg_cmp(const void *x, const void *y) { return memcmp(((const PrgIdx *)x)->rec + 12, ((const PrgIdx *)y)->rec + 12, 32); }

static void prg_collect(const uint8_t *rec, void *ctx) {
    PrgList *l = ctx;
    l->idx[l->n++].rec = rec;
}

enum { PREP_GIVE_UP = 64,     // failures before the first success: no translator (missing, blocked); stop, log once
       PREP_SAMPLE = 64 };     // prepared_current: records sampled for their stored inputs
static volatile int32_t prep_gave_up;   // the last prepare_files stopped that way (prepare_seed's summary line)

static void prep_worker(void *arg) {
    Prep *p = arg;
    // Below the game's threads, not idle: prepare_private goes back to normal priority while a translation is in
    // flight, which a game draw may wait for. Windows only in effect: on Linux plat_thread_priority renices only where
    // the process may raise its nice again (platform/linux.c may_renice), so there the threads stay at nice 0.
    if (p->background) plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    for (;;) {
        size_t i = (size_t)plat_inc64(&p->next) - 1;
        if (i >= p->n_ans) return;
        const uint8_t *r = p->ans + i * ANS_REC;
        uint8_t key_rec[12 + 32];
        memcpy(key_rec + 12, r + 4, 32);
        PrgIdx k = {key_rec}, *e = bsearch(&k, p->progs.idx, p->progs.n, sizeof k, prg_cmp);
        if (!e) {
            plat_inc64(&p->missing), plat_inc64(&p->done);
            if (p->background) plat_inc64(&bg_done);
            continue;
        }
        uint32_t st, len;
        memcpy(&st, e->rec, 4), memcpy(&len, e->rec + 4, 4);
        const uint8_t *control = e->rec + 44, *code = control + CONTROL_SIZE, *c1 = code + len;
        uint32_t c1_rec, c1_off, length;   // req_init reads length - c1_off bytes at c1: the record must hold them
        memcpy(&c1_rec, e->rec + 8, 4), memcpy(&c1_off, control + 0x700, 4), memcpy(&length, control + 0x704, 4);
        if (c1_off > length || length - c1_off != c1_rec) {
            plat_inc64(&p->failed), plat_inc64(&p->done);   // a damaged or hand-edited seed record
            if (p->background) plat_inc64(&bg_done);
            continue;
        }
        GfxShaderAnswers a;
        unpack_answers(r + 68, &a);
        uint8_t iid[32];
        const GfxShader *s = NULL;
        int ok = p->background ? prepare_private((int)st, control, code, len, c1, &a, iid)
                               : (s = gfx_shader_get((int)st, control, code, len, c1, &a)) != NULL;
        if (s) memcpy(iid, s->iid, 32);
        if (!ok) {
            if (plat_inc64(&p->failed) >= PREP_GIVE_UP && !p->succeeded && !plat_xchg32(&p->stopped, 1)) {
                plat_xchg64(&p->next, (int64_t)p->n_ans);   // the other threads stop at their next record
                rt_log("gfx_shader: prepare: gave up after %d failures with none translated: no working translator "
                       "(xcx_xlate missing, or blocked)?", PREP_GIVE_UP);
            }
        } else {
            plat_inc64(&p->succeeded);
            if (p->iid) memcpy(p->iid[i], iid, 32);
        }
        plat_inc64(&p->done);
        if (p->background) plat_inc64(&bg_done);
    }
}

// xcx.exe --prepare-shaders (gfx_shader_prepare_seed): prepare_files also prints "prepare-shaders: progress <done> of
// <total>" once a second on the console, which the setup reads for its progress bar and time left (installer/steps.py
// Prepare.prepare_shaders). The game's own foreground pass (boot.c) prints nothing.
static int prep_console;

// Translates every record of ans_path whose program is in prg_path on `threads` threads with up to `procs` translators.
// background: while the game runs; the translator limit is the run-time one plus `threads` (procs is not used), so the
// game's own translations (gfx_shader_get_async, svc_runtime_limit children) never queue behind the preparation.
// *ans_out, *iid_out (optional; free them): the answers records and the input id each resolved to (zero: failed).
// Returns the record count, -1 without the files.
static long long prepare_files(const char *ans_path, const char *prg_path, int threads, int procs, int background,
                               const char *what, uint8_t **ans_out, uint8_t (**iid_out)[32]) {
    paths();
    double t0 = (double)plat_now_ms();
    uint8_t *ab = NULL, *pb = NULL;
    size_t an = 0, pn = 0;
    // read only: a seed the setup shipped stays even when this build can't read it (the setup writes it again)
    if (!rec_read_as(ans_path, "XCXSANS", ANS_VERSION, ANS_REC, &ab, &an, 1) ||
        !rec_read_as(prg_path, "XCXSPRG", PRG_VERSION, 0, &pb, &pn, 1)) {
        rt_log("gfx_shader: prepare: no %s (%s)", what, ans_path);
        free(ab), free(pb);
        return -1;
    }
    Prep p = {.progs = {calloc(pn / PRG_HEAD + 1, sizeof(PrgIdx)), 0}, .ans = ab, .n_ans = an / ANS_REC,
              .background = background};
    if (iid_out) p.iid = calloc(p.n_ans + 1, 32);
    if (!p.progs.idx || (iid_out && !p.iid)) return free(p.progs.idx), free(p.iid), free(ab), free(pb), -1;
    prg_walk(pb, pn, prg_collect, &p.progs);
    qsort(p.progs.idx, p.progs.n, sizeof *p.progs.idx, prg_cmp);
    uint64_t xl0 = (uint64_t)n_xlated;
    threads = threads < 1 ? 1 : threads > 64 ? 64 : threads;
    if (background) plat_xchg64(&bg_total, (int64_t)p.n_ans);
    svc_set_limit(background ? svc_runtime_limit() + threads : procs);
    PlatThread *t[64];
    int k = 0;
    for (int i = 0; i < threads; i++)
        if ((t[k] = plat_thread_new(prep_worker, &p, "shader prepare"))) k++;
    if (!k) prep_worker(&p);
    if (prep_console) printf("prepare-shaders: progress 0 of %zu\n", p.n_ans), fflush(stdout);
    for (int m = 0, ticks = 0; m < k; m++)
        while (!plat_thread_join_ns(t[m], prep_console ? 1000000000u : 5000000000u)) {
            if (prep_console) printf("prepare-shaders: progress %lld of %zu\n", (long long)p.done, p.n_ans), fflush(stdout);
            if (!prep_console || ++ticks % 5 == 0)
                rt_log("gfx_shader: prepare: %lld of %zu pairs, %llu translated, %.0f ms", (long long)p.done, p.n_ans,
                       (unsigned long long)((uint64_t)n_xlated - xl0), (double)plat_now_ms() - t0);
        }
    if (prep_console) printf("prepare-shaders: progress %zu of %zu\n", p.n_ans, p.n_ans), fflush(stdout);
    svc_set_limit(svc_runtime_limit());
    rt_log("gfx_shader: prepare: %zu %s of %zu programs: %llu translated, %lld failed, %lld without a program, the rest "
           "cached; %.0f ms", p.n_ans, what, p.progs.n, (unsigned long long)((uint64_t)n_xlated - xl0), (long long)p.failed,
           (long long)p.missing, (double)plat_now_ms() - t0);
    free(p.progs.idx), free(pb);
    if (ans_out) *ans_out = ab;
    else free(ab);
    if (iid_out) *iid_out = p.iid;
    plat_xchg32(&prep_gave_up, p.stopped);
    return (long long)p.n_ans;
}

void gfx_shader_prepare_recorded(int threads, int procs) {
    if (!RC.on) {
        rt_log("gfx_shader: prepare: no recorded answers (not recording)");
        return;
    }
    prepare_files(RC.ans_path, RC.prg_path, threads, procs, 0, "recorded pairs", NULL, NULL);
}

// ---- prepared variants (shader-prepared.bin beside xcx.exe, gfx_shader_seed_dir)
// "XCXSPRP", version, record size; records: stage, priority, 2 zero bytes, input id. gfx_shader_prepare_seed writes
// them in priority order, one per input id; gfx_shader_warm_list reads them at boot.
#define PRP_MAGIC "XCXSPRP"   // 8 bytes with its NUL: the writer and the reader use this one spelling
#define PRP_VERSION 1
#define PRP_REC 36

static size_t prepared_ids(uint8_t (**out)[32]) {
    char dir[PATH_CAP], prp[PATH_CAP + 32];
    uint8_t *b = NULL;
    size_t len = 0, n = 0;
    *out = NULL;
    if (!gfx_shader_seed_dir(dir, sizeof dir)) return 0;
    snprintf(prp, sizeof prp, "%s/shader-prepared.bin", dir);
    if (!plat_file_exists(prp) || !rec_read(prp, PRP_MAGIC, PRP_VERSION, PRP_REC, &b, &len)) return 0;
    if ((n = len / PRP_REC) && (*out = malloc(n * 32))) {
        for (size_t i = 0; i < n; i++) memcpy((*out)[i], b + i * PRP_REC + 4, 32);
        qsort(*out, n, 32, iid_cmp);
    } else {
        n = 0;
    }
    free(b);
    return n;
}

static int prepare_seed(const char *dir, int threads, int procs, int background) {
    char ans[PATH_CAP + 32], prg[PATH_CAP + 32], out[PATH_CAP + 32];
    snprintf(ans, sizeof ans, "%s/seed-answers.bin", dir);
    snprintf(prg, sizeof prg, "%s/seed-programs.bin", dir);
    snprintf(out, sizeof out, "%s/shader-prepared.bin", dir);
    double t0 = (double)plat_now_ms();
    uint8_t *ab = NULL, (*iid)[32] = NULL;
    long long n = prepare_files(ans, prg, threads, procs, background, "seed variants", &ab, &iid);
    if (n < 0) return 1;
    uint8_t *recs = calloc((size_t)n + 1, PRP_REC);
    static const uint8_t zero[32];
    size_t u = 0, failed = 0, by[3] = {0}, stages[6] = {0};   // failed: this seed's, not the game's own meanwhile
    Set64 seen = {0};
    for (long long i = 0; recs && i < n; i++) {           // seed records come in priority order: the first id wins
        if (!memcmp(iid[i], zero, 32)) {
            failed++;
            continue;
        }
        if (!set_add(&seen, hash8(iid[i]))) continue;
        uint8_t *r = recs + u * PRP_REC;
        const uint8_t *a = ab + (size_t)i * ANS_REC;
        r[0] = a[0] < 6 ? a[0] : 0, r[1] = a[ANS_REC - 1] > 2 ? 2 : a[ANS_REC - 1];
        memcpy(r + 4, iid[i], 32);
        by[r[1]]++, stages[r[0]]++, u++;
    }
    free(seen.v);
    RecHeader h = {PRP_MAGIC, PRP_VERSION, PRP_REC};
    // the list names what was prepared; the variants that failed are translated in play when met, as without a seed.
    // Writing no list for them would start the pass again at every start for the same failures. Nothing prepared (no
    // working translator, say): no list, so the setup warns and the next start tries again.
    uint8_t *file = recs && u && !prep_gave_up ? malloc(sizeof h + u * PRP_REC + 1) : NULL;
    int ok = file != NULL;
    if (ok) {
        memcpy(file, &h, sizeof h), memcpy(file + sizeof h, recs, u * PRP_REC);
        ok = plat_file_write_atomic(out, file, sizeof h + u * PRP_REC);
    }
    if (ok) {   // the translator identity it was made under (prepared_current)
        char id[PATH_CAP + 32], hex[65];
        snprintf(id, sizeof id, "%s/shader-prepared.id", dir);
        key_hex(xlate_id, hex);
        if (!plat_file_write_atomic(id, hex, 64)) ok = 0, plat_file_delete(out);   // a list without its id: every
    }                                                                                // start would prepare again
    char msg[640];
    snprintf(msg, sizeof msg, "gfx_shader: prepared %zu shader variants (vertex %zu, fragment %zu, compute %zu; title %zu, "
             "warm-up %zu, other %zu) from %lld seed records, %llu translations failed, in %.0f ms; %s %s", u,
             stages[GFX_STAGE_VS], stages[GFX_STAGE_FS], stages[GFX_STAGE_CS], by[0], by[1], by[2], n,
             (unsigned long long)failed, (double)plat_now_ms() - t0,
             ok ? "wrote" : prep_gave_up || !u ? "gave up (no working translator), not writing" : "cannot write", out);
    rt_log("%s", msg);
    if (!background) printf("%s\n", msg);   // xcx.exe --prepare-shaders reports on the console
    free(file), free(recs), free(iid), free(ab);
    return ok ? 0 : 1;
}

int gfx_shader_prepare_seed(const char *dir, int threads, int procs) {
    prep_console = 1;
    int rc = prepare_seed(dir, threads, procs, 0);
    prep_console = 0;
    return rc;
}

// ---- the seed, prepared while the game runs
// The setup writes the seed beside xcx.exe (tools/prepare_shaders.py) and, with the choice "Prepare now", also runs
// xcx.exe --prepare-shaders, which leaves shader-prepared.bin and shader-prepared.id there. Otherwise, or when the id
// names another translator identity, the seed is translated here while the game runs, by a few threads below the
// game's on Windows (prep_worker; at normal priority while a translation is in flight); the translator processes run
// at normal priority, like the game's own. The overlay shows the progress.
// The setup deletes shader-prepared.bin whenever it writes a new seed. Translation goes around the record of answers
// met in play and the memory cache (prepare_private). The thread is not joined: quitting mid-run (accepted) stops it
// with the process, the translator children see their input close, and the translations made so far stay in the
// disk cache, so the next start passes over them quickly; shader-prepared.bin is only written, atomically, at the end.
static char bg_dir[PATH_CAP];
static int bg_threads, bg_asked;   // bg_asked: the pass came from the menu's button

// 1 when prp is there and shader-prepared.id beside it names the translator identity in use. The identity includes the
// host answers (gfx_shader_set_host), so another translator or another graphics card that the setup did not see (a
// dev install, XCX_XLATE, a GPU swap) prepares the seed again instead of leaving it to be translated in play.
static int prepared_current(const char *dir) {
    char prp[PATH_CAP + 32], id[PATH_CAP + 32], hex[65];
    snprintf(prp, sizeof prp, "%s/shader-prepared.bin", dir);
    if (!plat_file_exists(prp)) return 0;
    snprintf(id, sizeof id, "%s/shader-prepared.id", dir);
    uint64_t n = 0;
    uint8_t *f = plat_file_read(id, &n);
    key_hex(xlate_id, hex);
    int same = f && n == 64 && !memcmp(f, hex, 64);
    free(f);
    if (same) {   // the translations too: a shader cache deleted by hand leaves the list behind. Of up to 64 records
        // spread over the list (first and last included), half or more must have their stored inputs, the setup's
        // rule (installer/shaders.py prepared_intact) on a sample: this check runs at every start
        uint8_t *b = NULL;
        size_t len = 0;
        int ok = rec_read(prp, PRP_MAGIC, PRP_VERSION, PRP_REC, &b, &len) && len >= PRP_REC;
        size_t k = len / PRP_REC, picks = k < PREP_SAMPLE ? k : PREP_SAMPLE, have = 0;
        for (size_t i = 0; ok && i < picks; i++) {
            char p[PATH_CAP + 80];
            in_path(b + (picks > 1 ? i * (k - 1) / (picks - 1) : 0) * PRP_REC + 4, p);
            have += file_exists(p);
        }
        same = ok && have * 2 >= picks;
        free(b);
        if (!same) rt_log("gfx_shader: %s names translations the shader cache no longer has; preparing again", prp);
    }
    return same;
}

int gfx_shader_prepared_current(const char *dir) {
    paths();   // the translator identity and the cache folder
    return prepared_current(dir);
}

static void bg_main(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    if (bg_asked) rt_log("gfx_shader: preparing the setup's shader seed in the background on %d thread(s) (asked in the "
                         "menu)", bg_threads);
    else rt_log("gfx_shader: preparing the setup's shader seed in the background on %d thread(s) (shader_prepare = 0 "
                "turns this off)", bg_threads);
    prepare_seed(bg_dir, bg_threads, 0, 1);
    plat_xchg64(&bg_total, 0);   // a later press shows no stale percentage
    plat_xchg32(&bg_running, 0);
}

// Starts the background pass. asked: from the menu's "Prepare shaders now" (shader_prepare = 0 does not stop it, and
// a current prepared list is reported rather than redone). Returns GFX_PREPARE_*.
static int prepare_begin(int asked) {
    if (!RC.on) {   // no recording: the boot's prewarm has not set up the caches (XCX_PIPE_SYNC, a --shot run)
        rt_log("gfx_shader: shader preparation is not available: the pipeline record is off");
        return GFX_PREPARE_UNAVAILABLE;
    }
    if (bg_running) return GFX_PREPARE_RUNNING;
    char dir[PATH_CAP], seed[PATH_CAP + 32];
    if (!gfx_shader_seed_dir(dir, sizeof dir)) {
        rt_log("gfx_shader: shader preparation is not available: xcx.exe's folder does not fit a path");
        return GFX_PREPARE_UNAVAILABLE;
    }
    paths();   // the translator identity prepared_current compares, and the cache folder
    if (!cache_dir[0]) {
        rt_log("gfx_shader: shader preparation is not available: the shader cache folder's path does not fit");
        return GFX_PREPARE_UNAVAILABLE;
    }
    snprintf(seed, sizeof seed, "%s/seed-answers.bin", dir);
    if (!plat_file_exists(seed)) return GFX_PREPARE_NO_SEED;
    if (prepared_current(dir)) return GFX_PREPARE_DONE;
    // a few threads, one translator child each on top of the run-time ones: about 40 ms of a core per shader
    const char *te = cfg_get("XCX_SHADER_PREPARE_THREADS");   // no fixed default: an eighth of the CPUs, 1 to 4
    int cpus = (int)plat_cpu_count(), t = te && *te ? atoi(te) : 0;
    int threads = t > 0 ? (t > 16 ? 16 : t) : cpus / 8 < 1 ? 1 : cpus / 8 > 4 ? 4 : cpus / 8;
    int free_slots = MAX_SVC - svc_runtime_limit();   // each thread's child on top of the run-time ones
    if (threads > free_slots) {
        rt_log("gfx_shader: %d background shader thread(s) asked for, but only %d translator processes are free beside "
               "xlate_procs %d (at most %d in all): using %d", threads, free_slots, svc_runtime_limit(), MAX_SVC,
               free_slots);
        threads = free_slots;
    }
    if (threads < 1) {
        rt_log("gfx_shader: shader preparation is not available: no translator process is free (xlate_procs %d)",
               svc_runtime_limit());
        return GFX_PREPARE_UNAVAILABLE;
    }
    if (plat_xchg32(&bg_running, 1)) return GFX_PREPARE_RUNNING;   // the boot's start and a menu press, at once
    snprintf(bg_dir, sizeof bg_dir, "%s", dir);   // after the claim: one writer
    bg_threads = threads, bg_asked = asked;
    plat_xchg64(&bg_done, 0), plat_xchg64(&bg_total, 0);
    if (!plat_thread_new(bg_main, NULL, "shader prepare bg")) {
        plat_xchg32(&bg_running, 0);
        rt_log("gfx_shader: shader preparation is not available: its thread could not start");
        return GFX_PREPARE_UNAVAILABLE;
    }
    return GFX_PREPARE_STARTED;
}

void gfx_shader_prepare_start(void) {
    // XCX_TEST_PREPARE_SHADERS (boot.c prewarm_main) sets the translator limit for its own run and back after it
    const char *test = cfg_get("XCX_TEST_PREPARE_SHADERS");
    // a --shot image must not depend on background work (rt_boot leaves the pipeline builds synchronous for it too)
    if (!cfg_bool("XCX_SHADER_PREPARE") || (test && !strcmp(test, "1")) || rt_config()->shot) return;
    prepare_begin(0);
}

int gfx_shader_prepare_now(void) { return prepare_begin(1); }

// ---- clearing the caches (the menu's "Clear shader cache"): the files are open and the memory cache is in use while
// the game runs, so the press leaves a mark beside xcx.exe and the next start deletes them before anything reads them
// (rt_boot, before the prewarm). What goes: the shader cache folder, the pipeline cache and keys, the record of what
// play met, and the prepared list with its id. The setup's seed stays, so with shader_prepare on the next start
// prepares the game's shaders again. The GPU driver's own cache is the driver's to keep.
#define CLEAR_MARK "shader-cache.clear"

static int gone(const char *path) {   // deleted, read-only too, or not there
    int rc = plat_fs_unlink(path);
    return rc == PLAT_FS_OK || rc == PLAT_FS_NOT_FOUND;
}

int gfx_shader_cache_clear_request(int on) {
    char dir[PATH_CAP], mark[PATH_CAP + 32];
    if (!gfx_shader_seed_dir(dir, sizeof dir)) return 0;
    snprintf(mark, sizeof mark, "%s/%s", dir, CLEAR_MARK);
    if (!on) return gone(mark);
    return plat_file_write(mark, "1", 1);
}

int gfx_shader_cache_clear_pending(void) {
    char dir[PATH_CAP], mark[PATH_CAP + 32];
    if (!gfx_shader_seed_dir(dir, sizeof dir)) return 0;
    snprintf(mark, sizeof mark, "%s/%s", dir, CLEAR_MARK);
    return plat_file_exists(mark);
}

typedef struct {   // clear_listed's walk
    const char *dir;
    int in;            // the in/ folder: <key>.in
    int failed;
} ClearWalk;

// Only the names the cache writes, so a XCX_SHADER_CACHE that names a shared folder loses nothing else: <64 hex><kind>
// (.spv .res.json .use .code .job; in/: .in), or that name's <name>.<pid>.<tid>.tmp of an interrupted
// plat_file_write_atomic (installer/shaders.py OWN_FILE / OWN_INPUT, the setup's same rule)
static int own_name(const char *name, int in) {
    static const char *const cache[] = {".spv", ".res.json", ".use", ".code", ".job"};
    static const char *const inputs[] = {".in"};
    for (int i = 0; i < 64; i++)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return 0;
    const char *rest = name + 64, *const *kinds = in ? inputs : cache;
    size_t n = in ? 1 : sizeof cache / sizeof *cache;
    for (size_t i = 0; i < n; i++) {
        size_t k = strlen(kinds[i]);
        if (strncmp(rest, kinds[i], k)) continue;
        const char *t = rest + k;
        if (!*t) return 1;
        for (int part = 0; part < 2; part++) {   // .<digits>.<digits>, then exactly .tmp
            if (*t++ != '.') return 0;
            size_t d = strspn(t, "0123456789");
            if (!d) return 0;
            t += d;
        }
        return !strcmp(t, ".tmp");
    }
    return 0;
}

static int clear_listed(const char *name, void *ctx) {
    ClearWalk *w = ctx;
    if (strlen(name) < 64 || !own_name(name, w->in)) return 1;
    char p[PATH_CAP * 2 + 8];
    snprintf(p, sizeof p, "%s/%s", w->dir, name);
    if (!gone(p)) w->failed++;
    return 1;
}

static int clear_folder(const char *dir) {   // the shader cache and its in/; the folders go only when left empty
    char sub[PATH_CAP + 8];
    snprintf(sub, sizeof sub, "%s/in", dir);
    ClearWalk w = {sub, 1, 0};
    plat_file_list(sub, "", clear_listed, &w);
    plat_fs_rmdir(sub);
    w.dir = dir, w.in = 0;
    plat_file_list(dir, "", clear_listed, &w);
    plat_fs_rmdir(dir);
    return w.failed;
}

static int same_folder(const char *a, const char *b) {   // the same text, either separator, no trailing separator
    for (;; a++, b++) {
        int ca = *a == '\\' ? '/' : *a, cb = *b == '\\' ? '/' : *b;
        if (!ca && cb == '/' && !b[1]) return 1;
        if (!cb && ca == '/' && !a[1]) return 1;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

void gfx_shader_cache_clear_at_start(void) {
    char dir[PATH_CAP], mark[PATH_CAP + 32], keys_dir[PLAT_PATH_MAX * 2], shaders[PATH_CAP];
    if (!gfx_shader_seed_dir(dir, sizeof dir)) return;
    snprintf(mark, sizeof mark, "%s/%s", dir, CLEAR_MARK);
    if (!plat_file_exists(mark)) return;
    // The folder of the pipeline cache and the records, by vk_pipeline.c cache_path_init (XCX_PIPELINE_CACHE when it
    // fits, else <exe>/pipeline-cache.bin) and keys_open (the keys and answers beside it; a bare name: the current
    // folder). File names: vk_pipeline.c (pipeline-cache.bin, pipeline-keys.bin), this file (shader-answers.bin,
    // shader-programs.bin, the prepared list and its id; installer/steps.py PREPARED).
    const char *pc = cfg_get("XCX_PIPELINE_CACHE");
    int own_pc = pc && *pc && strlen(pc) < sizeof keys_dir;   // cache_path_init's own limit
    if (own_pc) {
        snprintf(keys_dir, sizeof keys_dir, "%s", pc);
        char *cut = plat_last_sep(keys_dir);
        if (cut) *cut = 0;
        else snprintf(keys_dir, sizeof keys_dir, ".");
    } else {
        snprintf(keys_dir, sizeof keys_dir, "%s", dir);
    }
    int failed = 0;
    static const char *const files[] = {"pipeline-keys.bin", "shader-answers.bin", "shader-programs.bin"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char p[PLAT_PATH_MAX * 2 + 32];
        snprintf(p, sizeof p, "%s/%s", keys_dir, files[i]);
        if (!gone(p)) failed++;
    }
    static const char *const prepared[] = {"shader-prepared.bin", "shader-prepared.id"};   // beside xcx.exe
    for (size_t i = 0; i < 2; i++) {
        char p[PATH_CAP + 32];
        snprintf(p, sizeof p, "%s/%s", dir, prepared[i]);
        if (!gone(p)) failed++;
    }
    char pcache[PLAT_PATH_MAX * 2 + 32];
    if (own_pc) snprintf(pcache, sizeof pcache, "%s", pc);   // the pipeline cache under its own name
    else snprintf(pcache, sizeof pcache, "%s/pipeline-cache.bin", dir);
    if (!gone(pcache)) failed++;
    cache_folder(dir, shaders);
    if (!shaders[0]) rt_log("gfx_shader: clear: the shader cache folder's path does not fit; it is left as it is");
    else if (same_folder(shaders, dir)) rt_log("gfx_shader: clear: XCX_SHADER_CACHE names the game's own folder; it is left as it is");
    else failed += clear_folder(shaders);
    if (!gone(mark))
        rt_log("gfx_shader: clear: %s cannot be deleted; the caches are cleared again at the next start", CLEAR_MARK);
    if (failed) rt_log("gfx_shader: cleared the shader caches (asked in the menu), but %d file(s) could not be deleted "
                       "(in use or not allowed); they are kept and stay valid", failed);
    else rt_log("gfx_shader: cleared the shader cache, the pipeline cache and the record of what play met (asked in the "
                "menu)");
}

int gfx_shader_prepare_progress(int64_t *done, int64_t *total) {
    if (!bg_running) return 0;
    *done = bg_done, *total = bg_total;
    return 1;
}

// The variants the boot prebuild warms, most likely first: every input id play recorded (shader-answers.bin, priority
// 0), then the prepared ones (shader-prepared.bin, priority 1 + their list priority). One entry per input id.
size_t gfx_shader_warm_list(GfxWarmEnt **out) {
    *out = NULL;
    if (!RC.on) return 0;
    char prp[PATH_CAP + 32], dir[PATH_CAP];
    if (!gfx_shader_seed_dir(dir, sizeof dir)) dir[0] = 0;
    snprintf(prp, sizeof prp, "%s%sshader-prepared.bin", dir, dir[0] ? "/" : "");
    uint8_t *ab = NULL, *pb = NULL;
    size_t an = 0, pn = 0;
    plat_mutex_lock(&rec_io);                      // no append meanwhile
    rec_read(RC.ans_path, "XCXSANS", ANS_VERSION, ANS_REC, &ab, &an);
    plat_mutex_unlock(&rec_io);
    rec_read(prp, PRP_MAGIC, PRP_VERSION, PRP_REC, &pb, &pn);
    size_t na = an / ANS_REC, np = pn / PRP_REC, n = 0;
    GfxWarmEnt *e = calloc(na + np + 1, sizeof *e);
    Set64 seen = {0};
    for (size_t i = 0; e && i < na; i++) {
        const uint8_t *r = ab + i * ANS_REC;
        if (!set_add(&seen, hash8(r + 36))) continue;
        memcpy(e[n].iid, r + 36, 32), e[n].stage = r[0], e[n].prio = 0, n++;
    }
    for (size_t i = 0; e && i < np; i++) {
        const uint8_t *r = pb + i * PRP_REC;
        if (!set_add(&seen, hash8(r + 4))) continue;
        memcpy(e[n].iid, r + 4, 32), e[n].stage = r[0], e[n].prio = (uint8_t)(1 + r[1]), n++;
    }
    free(seen.v), free(ab), free(pb);
    rt_log("gfx_shader: warm list: %zu variants (%zu met in play, %zu prepared)", n, na, np);
    *out = e;
    return n;
}
