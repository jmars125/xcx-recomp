// Merging the BDAT data tables of several game-file mods, so that mods which each ship a whole bdat/common.bdat can
// be on together. The rules, and why: docs/bdat-merge.md.
//
// The layout (version 4 with hashed names, as XCX DE 2.0.0 ships it), all little-endian:
//   file   'BDAT', 0x01001004, table count, file size, then one u32 offset per table; tables are 4-byte aligned
//   table  'BDAT', 0x3004, column count, row count, base id, 0, then offsets from the table start of the columns,
//          the hash index, the rows and the string table, the row length and the string table length
//   column 3 bytes: type (1 u8, 2 u16, 3 u32, 4 i8, 5 i16, 6 i32, 7 string, 8 float, 9 hash, 10 u8, 11 string,
//          12 u8, 13 u16) and the offset of the name's hash in the string table (u16)
//   index  one (hash, row) pair per distinct value of column 0 when that is a hash column, sorted, the first row of
//          a value kept
//   strings 0 (names are hashes), the table's name hash, one u32 kept as it is, one u32 per column name, then the
//          strings of the string cells, NUL-terminated, each distinct one once; an empty string points at the 0
// The game's own tables also hold a block of plain-text debug names before the columns. Mod tools drop it, and so
// does write_table, which only runs for tables merged from several mods; every other table keeps its bytes.
//
// Mod files come from anywhere, so every offset is checked before it is read.
#include "bdat_merge.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MODS 64
#define NONE UINT32_MAX

static const uint8_t TSIZE[14] = {0, 1, 2, 4, 1, 2, 4, 4, 4, 4, 1, 4, 1, 2};
#define IS_STR(t) ((t) == 7 || (t) == 11)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

typedef struct {
    const uint8_t *p;           // the table's bytes, up to the next table
    uint32_t len;
    uint32_t cc, rc, base, ro, rl, so, sl, name, extra;
    const uint8_t *cols;        // cc columns of 3 bytes
    uint32_t *off;              // each column's offset in a row
} Tab;

typedef struct {
    uint32_t count;
    Tab *t;
} File;

static void file_free(File *f) {
    for (uint32_t i = 0; f->t && i < f->count; i++) free(f->t[i].off);
    free(f->t);
    f->t = NULL;
}

static const uint8_t *cell(const Tab *t, uint32_t r, uint32_t c) { return t->p + t->ro + (size_t)r * t->rl + t->off[c]; }
static uint8_t ctype(const Tab *t, uint32_t c) { return t->cols[3 * c]; }
static uint32_t cname(const Tab *t, uint32_t c) { return rd32(t->p + t->so + rd16(t->cols + 3 * c + 1)); }
static uint32_t key(const Tab *t, uint32_t r) { return rd32(cell(t, r, 0)); }

// The string a string cell points at (checked when the table was read), its length in *n.
static const char *cstr(const Tab *t, uint32_t r, uint32_t c, size_t *n) {
    const char *s = (const char *)t->p + t->so + rd32(cell(t, r, c));
    *n = strlen(s);
    return s;
}

static int read_table(const uint8_t *p, uint32_t len, Tab *t) {
    memset(t, 0, sizeof *t);
    if (len < 48 || memcmp(p, "BDAT", 4) || rd32(p + 4) != 0x3004 || rd32(p + 20)) return 0;
    t->p = p;
    t->len = len;
    t->cc = rd32(p + 8);
    t->rc = rd32(p + 12);
    t->base = rd32(p + 16);
    uint32_t co = rd32(p + 24);
    t->ro = rd32(p + 32);
    t->rl = rd32(p + 36);
    t->so = rd32(p + 40);
    t->sl = rd32(p + 44);
    if (!t->cc || t->cc > 4096 || (uint64_t)co + 3ull * t->cc > len || (uint64_t)t->ro + (uint64_t)t->rl * t->rc > len ||
        (uint64_t)t->so + t->sl > len || t->sl < 9 || p[t->so])
        return 0;
    t->cols = p + co;
    t->name = rd32(p + t->so + 1);
    t->extra = rd32(p + t->so + 5);
    t->off = malloc(t->cc * sizeof *t->off);
    if (!t->off) return 0;
    uint32_t o = 0;
    for (uint32_t c = 0; c < t->cc; c++) {
        uint8_t ty = ctype(t, c);
        if (!ty || ty > 13 || (uint32_t)rd16(t->cols + 3 * c + 1) + 4 > t->sl) return 0;
        t->off[c] = o;
        o += TSIZE[ty];
    }
    if (o != t->rl) return 0;
    const uint8_t *s = p + t->so;
    for (uint32_t c = 0; c < t->cc; c++)
        if (IS_STR(ctype(t, c)))
            for (uint32_t r = 0; r < t->rc; r++) {
                uint32_t v = rd32(cell(t, r, c));
                if (v >= t->sl || !memchr(s + v, 0, t->sl - v)) return 0;
            }
    return 1;
}

static int read_file(const uint8_t *p, size_t n, File *f, char *why, size_t cap) {
    memset(f, 0, sizeof *f);
    if (n < 16 || n > 0xffffffffu || memcmp(p, "BDAT", 4) || rd32(p + 4) != 0x01001004 || rd32(p + 12) != n) {
        snprintf(why, cap, "not a BDAT file of the 2.0.0 layout");
        return 0;
    }
    f->count = rd32(p + 8);
    if (!f->count || 16 + 4ull * f->count > n || !(f->t = calloc(f->count, sizeof *f->t))) {
        snprintf(why, cap, "damaged table list");
        return 0;
    }
    for (uint32_t i = 0; i < f->count; i++) {
        uint32_t s = rd32(p + 16 + 4 * i), e = i + 1 < f->count ? rd32(p + 20 + 4 * i) : (uint32_t)n;
        if (s < 16 + 4 * f->count || e < s || e > n || !read_table(p + s, e - s, &f->t[i])) {
            snprintf(why, cap, "table %u is damaged", i);
            file_free(f);
            return 0;
        }
    }
    return 1;
}

// Debug string cells (type 11) are always alike: the developers' labels, which the game doesn't use and mod tools
// write back empty, are no change (docs/bdat-merge.md, "Debug names are not changes").
static int same_cell(const Tab *a, uint32_t ra, const Tab *b, uint32_t rb, uint32_t c) {
    if (ctype(a, c) == 11) return 1;
    if (IS_STR(ctype(a, c))) {
        size_t na, nb;
        const char *x = cstr(a, ra, c, &na), *y = cstr(b, rb, c, &nb);
        return na == nb && !memcmp(x, y, na);
    }
    return !memcmp(cell(a, ra, c), cell(b, rb, c), TSIZE[ctype(a, c)]);
}

static int same_row(const Tab *a, uint32_t ra, const Tab *b, uint32_t rb) {
    // Rows of the same bytes: every cell but the strings is alike, since a string cell holds only where its text is.
    // Mod tools keep the game's string order, so most rows of a mod's copy are the game's bytes (98.7% for two real
    // whole-file mods); then only the text cells are compared. Where a column's type differs, the cells are compared
    // as below.
    if (a->rl == b->rl && !memcmp(a->p + a->ro + (size_t)ra * a->rl, b->p + b->ro + (size_t)rb * b->rl, a->rl)) {
        uint32_t c = 0;
        for (; c < a->cc && ctype(a, c) == ctype(b, c); c++)
            if (ctype(a, c) == 7 && !same_cell(a, ra, b, rb, c)) return 0;
        if (c == a->cc) return 1;
    }
    for (uint32_t c = 0; c < a->cc; c++)
        if (ctype(a, c) != ctype(b, c) || !same_cell(a, ra, b, rb, c)) return 0;   // cells of another type differ
    return 1;
}

static int same_cols(const Tab *a, const Tab *b) {
    if (a->cc != b->cc) return 0;
    for (uint32_t c = 0; c < a->cc; c++)
        if (ctype(a, c) != ctype(b, c) || cname(a, c) != cname(b, c)) return 0;
    return 1;
}

// ---- sorted keys

typedef struct { uint32_t key, row; } Pair;

static int by_pair(const void *a, const void *b) {
    const Pair *x = a, *y = b;
    if (x->key != y->key) return (x->key > y->key) - (x->key < y->key);
    return (x->row > y->row) - (x->row < y->row);
}

// The table's (key, row) pairs sorted by key, NULL when out of memory; *unique says whether no key repeats.
static Pair *sorted_keys(const Tab *t, int *unique) {
    Pair *p = malloc((t->rc ? t->rc : 1) * sizeof *p);
    if (!p) return NULL;
    for (uint32_t r = 0; r < t->rc; r++) p[r] = (Pair){key(t, r), r};
    qsort(p, t->rc, sizeof *p, by_pair);
    *unique = 1;
    for (uint32_t r = 1; r < t->rc; r++)
        if (p[r].key == p[r - 1].key) *unique = 0;
    return p;
}

static uint32_t find_key(const Pair *p, uint32_t n, uint32_t k) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (p[mid].key < k) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && p[lo].key == k ? p[lo].row : NONE;
}

// A set of u32 values: open addressing, 0 stored apart.
typedef struct { uint32_t *v; uint32_t cap, n; int zero; } Set;

static int set_add(Set *s, uint32_t x) {
    if (!x) return s->zero = 1;
    if (2 * (s->n + 1) > s->cap) {
        Set t = {calloc(s->cap ? s->cap * 2 : 64, 4), s->cap ? s->cap * 2 : 64, 0, s->zero};
        if (!t.v) return 0;
        for (uint32_t i = 0; i < s->cap; i++)
            if (s->v[i]) set_add(&t, s->v[i]);
        free(s->v);
        *s = t;
    }
    uint32_t i = (x * 2654435761u) & (s->cap - 1);
    while (s->v[i] && s->v[i] != x) i = (i + 1) & (s->cap - 1);
    if (!s->v[i]) {
        s->v[i] = x;
        s->n++;
    }
    return 1;
}

static int set_has(const Set *s, uint32_t x) {
    if (!x) return s->zero;
    if (!s->cap) return 0;
    uint32_t i = (x * 2654435761u) & (s->cap - 1);
    while (s->v[i]) {
        if (s->v[i] == x) return 1;
        i = (i + 1) & (s->cap - 1);
    }
    return 0;
}

// ---- one mod's change to one table

enum { SAME, CHANGES, OPAQUE };

typedef struct {
    uint8_t kind;
    const char *reason;        // OPAQUE
    uint32_t *pos;             // per mod row: the base row it is, or NONE (a row the mod adds)
    uint32_t *mpos;            // per base row: its mod row, or NONE (a row the mod removes)
    uint8_t *edited;           // per base row: some cell differs
    uint32_t ndeleted, ninserts;
    int layout;                // base rows at other numbers or in another order
} Change;

static void change_free(Change *c) {
    free(c->pos);
    free(c->mpos);
    free(c->edited);
    memset(c, 0, sizeof *c);
}

// Half or more of the cells but the key are equal: a row whose key was renamed.
static int similar(const Tab *a, uint32_t ra, const Tab *b, uint32_t rb) {
    uint32_t n = a->cc - 1, eq = 0;
    for (uint32_t c = 1; c < a->cc; c++) eq += (uint32_t)same_cell(a, ra, b, rb, c);
    return n && eq * 2 >= n;
}

static int finish(Change *ch, const Tab *b, const Tab *m) {
    if (!(ch->mpos = malloc((b->rc ? b->rc : 1) * sizeof *ch->mpos)) || !(ch->edited = calloc(b->rc + 1, 1))) return 0;
    for (uint32_t r = 0; r < b->rc; r++) ch->mpos[r] = NONE;
    for (uint32_t i = 0; i < m->rc; i++) {
        uint32_t r = ch->pos[i];
        if (r == NONE) {
            ch->ninserts++;
            continue;
        }
        ch->mpos[r] = i;
        ch->edited[r] = !same_row(b, r, m, i);
        if (i != r) ch->layout = 1;
    }
    int any = ch->ninserts || ch->layout;
    for (uint32_t r = 0; r < b->rc; r++) {
        ch->ndeleted += ch->mpos[r] == NONE;
        any |= ch->edited[r];
    }
    if (!any && !ch->ndeleted) {
        change_free(ch);
        ch->kind = SAME;
    }
    return 1;
}

static int align_keyed(const Tab *b, const Pair *bk, const Tab *m, Change *ch) {
    if (!(ch->pos = malloc((m->rc ? m->rc : 1) * sizeof *ch->pos))) return 0;
    int order = 1;
    uint32_t last = NONE;
    for (uint32_t i = 0; i < m->rc; i++) {
        ch->pos[i] = find_key(bk, b->rc, key(m, i));
        if (ch->pos[i] != NONE) {
            if (last != NONE && ch->pos[i] <= last) order = 0;
            last = ch->pos[i];
        }
    }
    if (order) {
        // rows left over on both sides between the same matched rows: paired in order when similar (renamed keys)
        uint32_t m0 = 0, b0 = 0;   // the first mod and base rows after the last matched pair
        for (uint32_t i = 0; i <= m->rc; i++) {
            if (i < m->rc && ch->pos[i] == NONE) continue;
            uint32_t b1 = i < m->rc ? ch->pos[i] : b->rc, free_row = b0;
            for (uint32_t mi = m0; mi < i; mi++)
                for (uint32_t r = free_row; r < b1; r++)
                    if (similar(m, mi, b, r)) {
                        ch->pos[mi] = r;
                        free_row = r + 1;
                        break;
                    }
            m0 = i + 1;
            b0 = b1 + 1;
        }
    }
    return finish(ch, b, m);
}

static int align_positional(const Tab *b, const Pair *bk, const Tab *m, Change *ch) {
    uint32_t n = b->rc < m->rc ? b->rc : m->rc;
    for (uint32_t i = 0; bk && i < n; i++)
        if (key(m, i) != key(b, i) && find_key(bk, b->rc, key(m, i)) != NONE) {
            ch->kind = OPAQUE;
            ch->reason = "rows moved (it gives a row another row's key)";
            return 1;
        }
    if (!(ch->pos = malloc((m->rc ? m->rc : 1) * sizeof *ch->pos))) return 0;
    for (uint32_t i = 0; i < m->rc; i++) ch->pos[i] = i < n ? i : NONE;
    return finish(ch, b, m);
}

// bk: base's sorted keys (NULL when column 0 isn't a hash), bunique: no key repeats.
// The same columns, first row number and rows: two copies of a table alike.
static int same_table(const Tab *a, const Tab *b) {
    if (!same_cols(a, b) || a->base != b->base || a->rc != b->rc) return 0;
    for (uint32_t r = 0; r < a->rc; r++)
        if (!same_row(a, r, b, r)) return 0;
    return 1;
}

static int classify(const Tab *b, const Pair *bk, int bunique, const Tab *m, Change *ch) {
    memset(ch, 0, sizeof *ch);
    // the same bytes: the same table (a mod's file made from its JSON tables keeps the game's other tables as they are)
    if (b->len == m->len && !memcmp(b->p, m->p, b->len)) return 1;
    if (!same_cols(b, m)) {
        ch->kind = OPAQUE;
        ch->reason = "other columns";
        return 1;
    }
    if (m->base != b->base) {
        ch->kind = OPAQUE;
        ch->reason = "another first row number";
        return 1;
    }
    int same = m->rc == b->rc;
    for (uint32_t r = 0; same && r < b->rc; r++) same = same_row(b, r, m, r);
    if (same) return 1;
    ch->kind = CHANGES;
    if (bk && bunique) {
        int munique;
        Pair *mk = sorted_keys(m, &munique);
        if (!mk) return 0;
        free(mk);
        if (munique) return align_keyed(b, bk, m, ch);
    }
    return align_positional(b, bk, m, ch);
}

// ---- writing a merged table

typedef struct { uint8_t *p; size_t n, cap; } Buf;

static int put(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n) cap *= 2;
        uint8_t *q = realloc(b->p, cap);
        if (!q) return 0;
        b->p = q;
        b->cap = cap;
    }
    if (d) memcpy(b->p + b->n, d, n);
    else memset(b->p + b->n, 0, n);
    b->n += n;
    return 1;
}

// Distinct strings of the string table: open addressing on FNV-1a, offsets into the table being written.
typedef struct { uint32_t *slot; uint32_t cap; } Interns;

static uint32_t fnv(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

// The string's offset in st, added at the end the first time; UINT32_MAX when out of memory or room.
static uint32_t intern(Interns *in, Buf *st, const char *s, size_t n) {
    if (!n) return 0;
    uint32_t i = fnv(s, n) & (in->cap - 1);
    for (; in->slot[i]; i = (i + 1) & (in->cap - 1)) {
        const char *o = (const char *)st->p + in->slot[i];
        if (!strncmp(o, s, n) && !o[n]) return in->slot[i];
    }
    if (st->n + n + 1 > 0xffffffffu) return UINT32_MAX;
    uint32_t off = (uint32_t)st->n;
    if (!put(st, s, n) || !put(st, NULL, 1)) return UINT32_MAX;
    in->slot[i] = off;
    return off;
}

// Where an output cell comes from: src[t] (0 the base table, k + 1 mods[k]'s), at row r.
typedef struct { uint32_t r; uint8_t t; } Src;

// Table b's columns and base id with rows rows; cell (r, c) comes from from[r * cc + c]. extra is the string table's
// kept word.
static int write_table(Buf *out, const Tab *b, const Tab *const *src, const Src *from, uint32_t rows, uint32_t extra) {
    uint32_t cc = b->cc, rl = b->rl;
    Buf st = {0}, rb = {0};
    Interns in = {0};
    Pair *pairs = NULL;
    int ok = 0;
    uint8_t head[9] = {0};
    wr32(head + 1, b->name);
    wr32(head + 5, extra);
    if (!put(&st, head, 9)) goto done;
    for (uint32_t c = 0; c < cc; c++) {
        uint8_t h[4];
        wr32(h, cname(b, c));
        if (!put(&st, h, 4)) goto done;
    }
    if (9 + 4ull * (cc - 1) > 0xffff) goto done;   // column name offsets are u16
    uint64_t strcols = 0;
    for (uint32_t c = 0; c < cc; c++) strcols += IS_STR(ctype(b, c));
    uint64_t want = (uint64_t)rows * strcols * 2 + 16;   // at most half full: every string cell may be distinct
    if (want > (1u << 30)) goto done;
    for (in.cap = 64; in.cap < want; in.cap *= 2) {}
    if (!(in.slot = calloc(in.cap, sizeof *in.slot)) || !put(&rb, NULL, (size_t)rl * rows)) goto done;
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cc; c++) {
            const Src *f = &from[(size_t)r * cc + c];
            const Tab *s = src[f->t];
            uint8_t *d = rb.p + (size_t)r * rl + b->off[c];
            if (IS_STR(ctype(b, c))) {
                size_t n;
                const char *str = cstr(s, f->r, c, &n);
                uint32_t o = intern(&in, &st, str, n);
                if (o == UINT32_MAX) goto done;
                wr32(d, o);
            } else {
                memcpy(d, cell(s, f->r, c), TSIZE[ctype(b, c)]);
            }
        }
    uint32_t keys = 0;
    if (ctype(b, 0) == 9 && rows) {
        if (!(pairs = malloc(rows * sizeof *pairs))) goto done;
        for (uint32_t r = 0; r < rows; r++) pairs[r] = (Pair){rd32(rb.p + (size_t)r * rl + b->off[0]), r};
        qsort(pairs, rows, sizeof *pairs, by_pair);
        for (uint32_t r = 0; r < rows; r++)
            if (!keys || pairs[r].key != pairs[keys - 1].key) pairs[keys++] = pairs[r];
    }
    uint32_t co = 48, ho = co + 3 * cc, ro = ho + 8 * keys;
    uint64_t so = (uint64_t)ro + (uint64_t)rl * rows;
    if (so + st.n > 0xffffffffu) goto done;
    uint8_t h[48];
    memcpy(h, "BDAT", 4);
    uint32_t f[11] = {0x3004, cc, rows, b->base, 0, co, ho, ro, rl, (uint32_t)so, (uint32_t)st.n};
    for (int i = 0; i < 11; i++) wr32(h + 4 + 4 * i, f[i]);
    if (!put(out, h, 48)) goto done;
    for (uint32_t c = 0; c < cc; c++) {
        uint8_t col[3] = {ctype(b, c)};
        wr16(col + 1, (uint16_t)(9 + 4 * c));
        if (!put(out, col, 3)) goto done;
    }
    for (uint32_t k = 0; k < keys; k++) {
        uint8_t e[8];
        wr32(e, pairs[k].key);
        wr32(e + 4, pairs[k].row);
        if (!put(out, e, 8)) goto done;
    }
    ok = put(out, rb.p, rb.n) && put(out, st.p, st.n) && put(out, NULL, (4 - (out->n & 3)) & 3);
done:
    free(st.p);
    free(rb.p);
    free(in.slot);
    free(pairs);
    return ok;
}

// ---- events

typedef struct {
    BdatNote note;
    void *ctx;
    BdatSummary *s;
    const BdatInput *mods;
} Events;

static void event(Events *ev, int kind, int mod, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void event(Events *ev, int kind, int mod, const char *fmt, ...) {
    static const char *const names[] = {"priority", "left out", "caveat", "adds nothing", "partly alike"};
    if (kind == 0) ev->s->priority++;
    else if (kind == 1) ev->s->left_out++;
    else if (kind == 2) ev->s->caveats++;
    else if (kind == 3) ev->s->adds_nothing++;
    else ev->s->partly_alike++;
    if (!ev->note) return;
    char line[700];
    int n = snprintf(line, sizeof line, "%s: ", names[kind]);
    if (mod >= 0) n += snprintf(line + n, sizeof line - (size_t)n, "%s: ", ev->mods[mod].name);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;   // a long mod name: the line is cut, never overrun
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    ev->note(ev->ctx, line);
}
enum { PRIORITY, LEFT_OUT, CAVEAT, ADDS_NOTHING, PARTLY_ALIKE };

static void rowname(const Tab *t, uint32_t r, char *out, size_t cap) {
    if (ctype(t, 0) == 9) snprintf(out, cap, "row %u (key %08X)", t->base + r, key(t, r));
    else snprintf(out, cap, "row %u", t->base + r);
}

// ---- merging one table that several mods change

typedef struct { uint32_t row; uint8_t kind, mod; } Entry;   // kind 0: base row; 1: mods[mod]'s added row

typedef struct {
    const Tab *b;
    const Tab *m[MAX_MODS];        // the mods' copies of the table (NULL: the mod's file is left out)
    Change *ch[MAX_MODS];
    const Set *refs;               // per mod: the hash values its changes put in cells
    int n;
    Events *ev;
} Job;

static int grow(Entry **v, uint32_t *cap, uint32_t need) {
    if (need <= *cap) return 1;
    uint32_t c = *cap ? *cap * 2 : 64;
    while (c < need) c *= 2;
    Entry *q = realloc(*v, c * sizeof *q);
    if (!q) return 0;
    *v = q;
    *cap = c;
    return 1;
}

// Merges into out; 0 when out of memory.
static int merge_table(Job *j, int *P, int np, Buf *out, char *why, size_t why_cap) {
    const Tab *t = j->b;
    Events *ev = j->ev;
    char rn[64];
    int owner = -1;
    for (int w = 0; w < np; w++) {
        Change *c = j->ch[P[w]];
        if (!c->layout && !c->ndeleted) continue;
        if (owner < 0) owner = P[w];
        else if (j->m[P[w]]->rc == j->m[owner]->rc &&
                 !memcmp(c->pos, j->ch[owner]->pos, (size_t)j->m[owner]->rc * sizeof *c->pos))
            continue;   // the same layout as the owner's (docs/bdat-merge.md, "Row layout"): nothing of it is left out
        else
            event(ev, LEFT_OUT, P[w], "table %08X: its removed or reordered rows are not used; %s's row layout is "
                  "(rows it adds are kept, at the end)", t->name, ev->mods[owner].name);
    }
    int keyed = 0;
    if (ctype(t, 0) == 9) {
        Pair *bk = sorted_keys(t, &keyed);
        free(bk);
    }
    uint32_t cap = 0, n = 0;
    Entry *seq = NULL;
    uint8_t *present = calloc(t->rc + 1, 1);
    Src *from = NULL;
    uint8_t *drop = NULL;
    Set added = {0};
    int ok = 0;
    if (!present) goto oom;
    if (owner >= 0) {
        const Change *c = j->ch[owner];
        if (!grow(&seq, &cap, j->m[owner]->rc + 1)) goto oom;
        for (uint32_t i = 0; i < j->m[owner]->rc; i++)
            seq[n++] = c->pos[i] != NONE ? (Entry){c->pos[i], 0, 0} : (Entry){i, 1, (uint8_t)owner};
    } else {
        if (!grow(&seq, &cap, t->rc + 1)) goto oom;
        for (uint32_t r = 0; r < t->rc; r++) seq[n++] = (Entry){r, 0, 0};
    }
    uint32_t prefix = 0;
    while (prefix < n && !seq[prefix].kind && seq[prefix].row == prefix) prefix++;
    for (uint32_t i = 0; i < n; i++)
        if (!seq[i].kind) present[seq[i].row] = 1;
    // rows the layout's mod removes: kept for a higher mod's edits or another mod's reference, where no row moves
    if (owner >= 0)
        for (uint32_t r = 0; r < t->rc; r++) {
            if (j->ch[owner]->mpos[r] != NONE) continue;
            int higher[MAX_MODS], refd[MAX_MODS], nh = 0, nr = 0;
            for (int w = 0; w < np && P[w] < owner; w++) {
                Change *c = j->ch[P[w]];
                if (c->mpos[r] != NONE && c->edited[r]) higher[nh++] = P[w];
            }
            for (int k = 0; keyed && k < j->n; k++)
                if (k != owner && j->m[k] && set_has(&j->refs[k], key(t, r))) refd[nr++] = k;
            if (!nh && !nr) continue;
            rowname(t, r, rn, sizeof rn);
            if (n == r && prefix == r) {
                if (!grow(&seq, &cap, n + 1)) goto oom;
                seq[n++] = (Entry){r, 0, 0};
                present[r] = 1;
                prefix++;
                char who[400] = "";
                size_t wl = 0;
                for (int k = 0; k < j->n && wl < sizeof who - 70; k++) {
                    int in = 0;
                    for (int x = 0; x < nh; x++) in |= higher[x] == k;
                    for (int x = 0; x < nr; x++) in |= refd[x] == k;
                    if (in) wl += (size_t)snprintf(who + wl, sizeof who - wl, "%s%s", wl ? ", " : "", ev->mods[k].name);
                }
                event(ev, CAVEAT, owner, "table %08X: %s is kept, although it removes it, for %s", t->name, rn, who);
            } else {
                for (int x = 0; x < nh; x++)
                    event(ev, LEFT_OUT, higher[x], "table %08X: its edits to %s: %s removes the row, and putting it "
                          "back would renumber the rows after it", t->name, rn, ev->mods[owner].name);
                for (int x = 0; x < nr; x++) {
                    int dup = 0;
                    for (int y = 0; y < nh; y++) dup |= higher[y] == refd[x];
                    if (!dup)
                        event(ev, CAVEAT, refd[x], "table %08X: it refers to %s, which %s removes", t->name, rn,
                              ev->mods[owner].name);
                }
            }
        }
    // edits of lower mods to rows that are gone
    for (int w = 0; owner >= 0 && w < np; w++) {
        int k = P[w];
        if (k <= owner) continue;
        uint32_t lost = 0, first = NONE;
        for (uint32_t r = 0; r < t->rc; r++)
            if (j->ch[k]->mpos[r] != NONE && j->ch[k]->edited[r] && !present[r]) {
                lost++;
                if (first == NONE) first = r;
            }
        if (lost) {
            rowname(t, first, rn, sizeof rn);
            event(ev, LEFT_OUT, k, "table %08X: its edits to %u row%s that %s removes (%s%s)", t->name, lost,
                  lost > 1 ? "s" : "", ev->mods[owner].name, rn, lost > 1 ? ", ..." : "");
        }
    }
    // rows the other mods add, after the layout's: by key, the higher mod's; without keys, each list once
    for (uint32_t i = 0; keyed && i < n; i++)
        if (seq[i].kind && !set_add(&added, key(j->m[seq[i].mod], seq[i].row))) goto oom;
    int lists[MAX_MODS], nl = 0;
    for (int w = 0; w < np; w++) {
        int k = P[w];
        const Tab *m = j->m[k];
        const Change *c = j->ch[k];
        if (k == owner || !c->ninserts) continue;
        if (!keyed) {
            int dup = 0;
            for (int x = 0; x < nl && !dup; x++) {
                const Tab *o = j->m[lists[x]];
                const Change *oc = j->ch[lists[x]];
                if (oc->ninserts != c->ninserts) continue;
                dup = 1;
                for (uint32_t a = 0, b2 = 0; dup && a < m->rc; a++) {
                    if (c->pos[a] != NONE) continue;
                    while (oc->pos[b2] != NONE) b2++;
                    dup = same_row(m, a, o, b2++);
                }
            }
            if (dup) continue;
            lists[nl++] = k;
        }
        if (!grow(&seq, &cap, n + c->ninserts + 1)) goto oom;
        for (uint32_t i = 0; i < m->rc; i++) {
            if (c->pos[i] != NONE) continue;
            if (keyed) {
                uint32_t kv = key(m, i);
                if (set_has(&added, kv)) {
                    // added already, by a higher mod (or the layout's): the same row is kept once, silently
                    for (uint32_t x = 0; x < n; x++)
                        if (seq[x].kind && key(j->m[seq[x].mod], seq[x].row) == kv) {
                            if (!same_row(j->m[seq[x].mod], seq[x].row, m, i))
                                event(ev, PRIORITY, -1, "table %08X: added row (key %08X): %s's, not %s's", t->name,
                                      kv, ev->mods[seq[x].mod].name, ev->mods[k].name);
                            break;
                        }
                    continue;
                }
                if (!set_add(&added, kv)) goto oom;
            }
            seq[n++] = (Entry){i, 1, (uint8_t)k};
        }
    }
    // cells: the base row with the edits applied lowest priority first, or the added row
    uint32_t cc = t->cc;
    const Tab *src[MAX_MODS + 1];
    src[0] = t;
    for (int k = 0; k < j->n; k++) src[k + 1] = j->m[k];
    if (!(from = malloc(((size_t)n * cc + 1) * sizeof *from)) || !(drop = calloc(n + 1, 1))) goto oom;
    for (uint32_t i = 0; i < n; i++) {
        Src *f = from + (size_t)i * cc;
        if (seq[i].kind) {
            for (uint32_t c = 0; c < cc; c++) f[c] = (Src){seq[i].row, (uint8_t)(seq[i].mod + 1)};
            continue;
        }
        uint32_t r = seq[i].row;
        for (uint32_t c = 0; c < cc; c++) f[c] = (Src){r, 0};
        for (int w = np - 1; w >= 0; w--) {
            int k = P[w];
            const Change *ch = j->ch[k];
            uint32_t mr = ch->mpos[r];
            if (mr == NONE || !ch->edited[r]) continue;
            for (uint32_t c = 0; c < cc; c++) {
                if (same_cell(t, r, j->m[k], mr, c)) continue;
                if (f[c].t && !same_cell(src[f[c].t], f[c].r, j->m[k], mr, c)) {
                    rowname(t, r, rn, sizeof rn);
                    event(ev, PRIORITY, -1, "table %08X %s column %u: %s's value, not %s's", t->name, rn, c,
                          ev->mods[k].name, ev->mods[f[c].t - 1].name);
                }
                f[c] = (Src){mr, (uint8_t)(k + 1)};
            }
        }
    }
    // keys stay unique: the game's key, then the higher mod's
    if (keyed) {
        Pair *at = malloc((n + 1) * sizeof *at);
        if (!at) goto oom;
        int again = 1;
        // each round puts back the game's key of at least one renamed row, and the game's keys are unique, so it ends
        for (uint32_t round = 0; again && round <= n; round++) {
            again = 0;
            uint32_t na = 0;
            for (uint32_t i = 0; i < n; i++)
                if (!drop[i]) at[na++] = (Pair){rd32(cell(src[from[(size_t)i * cc].t], from[(size_t)i * cc].r, 0)), i};
            qsort(at, na, sizeof *at, by_pair);
            for (uint32_t x = 0; x + 1 < na; x++) {
                if (at[x].key != at[x + 1].key) continue;
                uint32_t a = at[x].row, b2 = at[x + 1].row;
                int ra = from[(size_t)a * cc].t ? from[(size_t)a * cc].t - 1 : -1;
                int rb = from[(size_t)b2 * cc].t ? from[(size_t)b2 * cc].t - 1 : -1;
                uint32_t lose = rb >= ra ? b2 : a;
                at[x + 1].row = lose == a ? b2 : a;   // the kept row meets the next one with this key
                if (seq[lose].kind) {
                    drop[lose] = 1;
                    event(ev, LEFT_OUT, seq[lose].mod, "table %08X: its added row (key %08X): another row has that key",
                          t->name, at[x].key);
                } else {
                    rowname(t, seq[lose].row, rn, sizeof rn);
                    event(ev, LEFT_OUT, from[(size_t)lose * cc].t - 1, "table %08X: its new key %08X for %s: another "
                          "row has that key", t->name, at[x].key, rn);
                    from[(size_t)lose * cc] = (Src){seq[lose].row, 0};
                    again = 1;
                }
            }
        }
        free(at);
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (drop[i]) continue;
        seq[kept] = seq[i];
        memmove(from + (size_t)kept * cc, from + (size_t)i * cc, cc * sizeof *from);
        kept++;
    }
    n = kept;
    // row numbers against each mod's own copy
    for (int w = 0; w < np; w++) {
        int k = P[w];
        uint32_t moved = 0, at_i = 0, at_p = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t p = !seq[i].kind ? j->ch[k]->mpos[seq[i].row] : seq[i].mod == k ? seq[i].row : NONE;
            if (p != NONE && p != i) {
                if (!moved++) {
                    at_i = i;
                    at_p = p;
                }
            }
        }
        if (moved)
            event(ev, CAVEAT, k, "table %08X: %u of its rows have other row numbers than in its copy (first: row %u "
                  "is its row %u); what points at them by number may point elsewhere", t->name, moved, t->base + at_i,
                  t->base + at_p);
    }
    if (!write_table(out, t, src, from, n, j->m[P[0]]->extra)) {
        snprintf(why, why_cap, "table %08X could not be written", t->name);
        goto done;
    }
    ok = 1;
    goto done;
oom:
    snprintf(why, why_cap, "out of memory");
done:
    free(added.v);
    free(seq);
    free(present);
    free(from);
    free(drop);
    return ok;
}

// ---- bdat-toolset JSON tables
//
// Some mods ship only the tables they change, as bdat-toolset writes them with -f json, one file per table under
// bdat/<file>/<table>.json: {"schema": [{"name": n, "type": t}, ...], "rows": [{"$id": i, n: value, ...}, ...]}.
// A name is "<8 hex digits>" (the hash itself) or the plain name, which hashes with murmur3 (seed 0) to the game's.
// Hash cells are written the same way, strings as JSON strings, numbers as JSON numbers. Only whole tables are read
// (rows numbered one after another); such a table is the mod's copy of that table, and the game's copy (or the mod's
// full file) gives the rest. The reader is strict JSON (RFC 8259), with no NaN or Infinity and only valid UTF-8.

static uint32_t mm3(const uint8_t *d, size_t n) {
    uint32_t h = 0, k;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        k = rd32(d + i) * 0xcc9e2d51u;
        k = (k << 15 | k >> 17) * 0x1b873593u;
        h ^= k;
        h = (h << 13 | h >> 19) * 5 + 0xe6546b64u;
    }
    k = 0;
    switch (n & 3) {
    case 3: k ^= (uint32_t)d[i + 2] << 16; /* fall through */
    case 2: k ^= (uint32_t)d[i + 1] << 8; /* fall through */
    case 1:
        k ^= d[i];
        k *= 0xcc9e2d51u;
        k = (k << 15 | k >> 17) * 0x1b873593u;
        h ^= k;
    }
    h ^= (uint32_t)n;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}

static int hexval(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// A name as bdat-toolset writes it: "<8 hex digits>" is the hash itself, anything else is hashed.
static uint32_t hashname(const char *s, size_t n) {
    if (n == 10 && s[0] == '<' && s[9] == '>') {
        uint32_t v = 0;
        int ok = 1;
        for (int i = 1; i < 9; i++) {
            int x = hexval((unsigned char)s[i]);
            ok &= x >= 0;
            v = v << 4 | (uint32_t)(x & 15);
        }
        if (ok) return v;
    }
    return mm3((const uint8_t *)s, n);
}

enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct JNode JNode;
struct JNode {
    uint8_t type, isint;
    int64_t i;            // J_NUM with isint
    double d;             // J_NUM
    char *s;              // J_STR: UTF-8, NUL-terminated (may hold NULs: len counts them)
    size_t len;           // J_STR length; J_ARR / J_OBJ item count
    JNode **items;        // J_ARR values; J_OBJ values
    char **keys;          // J_OBJ keys
    size_t *klen;
};

// The parsed JSON's memory: taken from big blocks and given back all at once when the table is made. A large table's
// hundreds of thousands of nodes, keys and strings were as many mallocs and frees, most of the time of reading it.
typedef struct Block Block;
struct Block {
    Block *next;
    size_t used, cap;
};
#define BLOCK_HEAD ((sizeof(Block) + 15) & ~(size_t)15)
typedef struct { Block *head; int nblocks; } Arena;

// n bytes (16-aligned, not zeroed) from a; NULL when out of memory. Blocks grow from 1 MiB to 32 MiB.
static void *aget(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    Block *b = a->head;
    if (!b || b->cap - b->used < n) {
        size_t cap = (size_t)1 << (20 + (a->nblocks < 5 ? a->nblocks : 5));
        if (cap < n) cap = n;
        if (!(b = malloc(BLOCK_HEAD + cap))) return NULL;
        *b = (Block){a->head, 0, cap};
        a->head = b;
        a->nblocks++;
    }
    void *p = (uint8_t *)b + BLOCK_HEAD + b->used;
    b->used += n;
    return p;
}

static void afree(Arena *a) {
    for (Block *b = a->head, *next; b; b = next) {
        next = b->next;
        free(b);
    }
    *a = (Arena){0};
}

typedef struct { const char *p, *end; int depth, bad; Arena *a; } JParse;

static void jws(JParse *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int put_utf8(Buf *b, uint32_t c) {
    uint8_t u[4];
    size_t n;
    if (c < 0x80) u[0] = (uint8_t)c, n = 1;
    else if (c < 0x800) u[0] = (uint8_t)(0xc0 | c >> 6), u[1] = (uint8_t)(0x80 | (c & 63)), n = 2;
    else if (c < 0x10000)
        u[0] = (uint8_t)(0xe0 | c >> 12), u[1] = (uint8_t)(0x80 | (c >> 6 & 63)), u[2] = (uint8_t)(0x80 | (c & 63)), n = 3;
    else
        u[0] = (uint8_t)(0xf0 | c >> 18), u[1] = (uint8_t)(0x80 | (c >> 12 & 63)), u[2] = (uint8_t)(0x80 | (c >> 6 & 63)),
        u[3] = (uint8_t)(0x80 | (c & 63)), n = 4;
    return put(b, u, n);
}

// A JSON string at j->p (on the opening quote) into *s (in j->a, NUL-terminated) and *len.
static int jstring(JParse *j, char **s, size_t *len) {
    // A string without escapes (nearly every string in a bdat-toolset table) is its bytes as they are. The Buf below
    // starts at 4 KiB, which for a table's hundreds of thousands of names and values made the parse most of a merge's
    // time.
    const char *q = j->p + 1;
    while (q < j->end && *q != '"' && *q != '\\' && (unsigned char)*q >= 0x20) q++;
    if (q < j->end && *q == '"') {
        size_t n = (size_t)(q - j->p - 1);
        char *o = aget(j->a, n + 1);
        if (!o) return 0;
        memcpy(o, j->p + 1, n);
        o[n] = 0;
        *s = o;
        *len = n;
        j->p = q + 1;
        return 1;
    }
    Buf b = {0};
    j->p++;
    while (j->p < j->end && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p;
        if (c < 0x20) goto bad;
        if (c != '\\') {
            if (!put(&b, j->p++, 1)) goto bad;
            continue;
        }
        if (++j->p >= j->end) goto bad;
        char e = *j->p++;
        const char *simple = strchr("\"\\/bfnrt", e);
        if (simple && e) {
            static const char out[] = "\"\\/\b\f\n\r\t";
            if (!put(&b, &out[simple - "\"\\/bfnrt"], 1)) goto bad;
            continue;
        }
        if (e != 'u' || j->end - j->p < 4) goto bad;
        uint32_t u = 0;
        for (int i = 0; i < 4; i++) {
            int x = hexval((unsigned char)j->p[i]);
            if (x < 0) goto bad;
            u = u << 4 | (uint32_t)x;
        }
        j->p += 4;
        if (u >= 0xdc00 && u < 0xe000) goto bad;   // half a surrogate pair is not text
        if (u >= 0xd800 && u < 0xdc00) {
            if (j->end - j->p < 6 || j->p[0] != '\\' || j->p[1] != 'u') goto bad;
            uint32_t lo = 0;
            for (int i = 0; i < 4; i++) {
                int x = hexval((unsigned char)j->p[2 + i]);
                if (x < 0) goto bad;
                lo = lo << 4 | (uint32_t)x;
            }
            if (lo < 0xdc00 || lo >= 0xe000) goto bad;
            j->p += 6;
            u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
        }
        if (!put_utf8(&b, u)) goto bad;
    }
    if (j->p >= j->end || !put(&b, "", 1) || !(*s = aget(j->a, b.n))) goto bad;
    j->p++;
    memcpy(*s, b.p, b.n);
    *len = b.n - 1;
    free(b.p);
    return 1;
bad:
    free(b.p);
    return 0;
}

static JNode *jvalue(JParse *j);

// n's lists in a, grown by doubling; the lists they replace stay in a until it is given back.
static int jadd(Arena *a, JNode *n, JNode *v, char *key, size_t klen) {
    if ((n->len & (n->len - 1)) == 0) {   // grow at 0, 1, 2, 4, 8 ...
        size_t cap = n->len ? n->len * 2 : 1;
        JNode **it = aget(a, cap * sizeof *it);
        if (!it) return 0;
        if (n->len) memcpy(it, n->items, n->len * sizeof *it);
        n->items = it;
        if (n->type == J_OBJ) {
            char **k = aget(a, cap * sizeof *k);
            size_t *kl = aget(a, cap * sizeof *kl);
            if (!k || !kl) return 0;
            if (n->len) {
                memcpy(k, n->keys, n->len * sizeof *k);
                memcpy(kl, n->klen, n->len * sizeof *kl);
            }
            n->keys = k;
            n->klen = kl;
        }
    }
    n->items[n->len] = v;
    if (n->type == J_OBJ) {
        n->keys[n->len] = key;
        n->klen[n->len] = klen;
    }
    n->len++;
    return 1;
}

static JNode *jvalue(JParse *j) {
    jws(j);
    if (j->p >= j->end || ++j->depth > 500) return NULL;
    JNode *n = aget(j->a, sizeof *n);
    if (!n) return NULL;
    memset(n, 0, sizeof *n);
    char c = *j->p;
    if (c == '{' || c == '[') {
        n->type = c == '{' ? J_OBJ : J_ARR;
        j->p++;
        jws(j);
        if (j->p < j->end && *j->p == (c == '{' ? '}' : ']')) {
            j->p++;
            j->depth--;
            return n;
        }
        for (;;) {
            char *key = NULL;
            size_t klen = 0;
            if (n->type == J_OBJ) {
                jws(j);
                if (j->p >= j->end || *j->p != '"' || !jstring(j, &key, &klen)) goto bad;
                jws(j);
                if (j->p >= j->end || *j->p != ':') goto bad;
                j->p++;
            }
            JNode *v = jvalue(j);
            if (!v || !jadd(j->a, n, v, key, klen)) goto bad;   // what was taken stays in j->a until it is given back
            jws(j);
            if (j->p < j->end && *j->p == ',') {
                j->p++;
                continue;
            }
            if (j->p < j->end && *j->p == (c == '{' ? '}' : ']')) {
                j->p++;
                break;
            }
            goto bad;
        }
    } else if (c == '"') {
        n->type = J_STR;
        if (!jstring(j, &n->s, &n->len)) goto bad;
    } else if (j->end - j->p >= 4 && !memcmp(j->p, "true", 4)) {
        n->type = J_TRUE;
        j->p += 4;
    } else if (j->end - j->p >= 5 && !memcmp(j->p, "false", 5)) {
        n->type = J_FALSE;
        j->p += 5;
    } else if (j->end - j->p >= 4 && !memcmp(j->p, "null", 4)) {
        n->type = J_NULL;
        j->p += 4;
    } else {
        // -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?
        const char *s = j->p, *q = s;
        n->type = J_NUM;
        n->isint = 1;
        if (q < j->end && *q == '-') q++;
        if (q >= j->end || *q < '0' || *q > '9') goto bad;
        if (*q == '0') q++;
        else
            while (q < j->end && *q >= '0' && *q <= '9') q++;
        if (q < j->end && *q == '.') {
            n->isint = 0;
            if (++q >= j->end || *q < '0' || *q > '9') goto bad;
            while (q < j->end && *q >= '0' && *q <= '9') q++;
        }
        if (q < j->end && (*q == 'e' || *q == 'E')) {
            n->isint = 0;
            if (++q < j->end && (*q == '+' || *q == '-')) q++;
            if (q >= j->end || *q < '0' || *q > '9') goto bad;
            while (q < j->end && *q >= '0' && *q <= '9') q++;
        }
        char tmp[400];
        size_t len = (size_t)(q - s);
        if (len >= sizeof tmp) goto bad;
        memcpy(tmp, s, len);
        tmp[len] = 0;
        n->d = strtod(tmp, NULL);
        if (n->isint) {
            // an integer past 64 bits is still an integer, just out of every column's range
            int neg = tmp[0] == '-';
            uint64_t v = 0;
            int big = 0;
            for (const char *x = tmp + neg; *x; x++) {
                if (v > (UINT64_MAX - 9) / 10) big = 1;
                v = v * 10 + (uint64_t)(*x - '0');
            }
            if (big || v > (uint64_t)INT64_MAX) n->i = neg ? INT64_MIN : INT64_MAX, n->isint = 2;   // out of range
            else n->i = neg ? -(int64_t)v : (int64_t)v;
        }
        j->p = q;
    }
    j->depth--;
    return n;
bad:   // n and what it holds stay in j->a until it is given back
    return NULL;
}

// Valid UTF-8, without overlong forms or surrogates (as Python's decoder takes it).
static int utf8_ok(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t c = p[i];
        size_t k = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        if (!k || i + k > n) return 0;
        uint32_t u = k == 1 ? c : k == 2 ? c & 31u : k == 3 ? c & 15u : c & 7u;
        for (size_t x = 1; x < k; x++) {
            if ((p[i + x] & 0xc0) != 0x80) return 0;
            u = u << 6 | (p[i + x] & 63u);
        }
        if ((k == 2 && u < 0x80) || (k == 3 && u < 0x800) || (k == 4 && (u < 0x10000 || u > 0x10ffff)) ||
            (u >= 0xd800 && u < 0xe000))
            return 0;
        i += k;
    }
    return 1;
}

// The value of key in object o (the last one when a key repeats, as Python's json does), or NULL.
static const JNode *jget(const JNode *o, const char *key, size_t klen) {
    if (!o || o->type != J_OBJ) return NULL;
    for (size_t i = o->len; i-- > 0;)
        if (o->klen[i] == klen && !memcmp(o->keys[i], key, klen)) return o->items[i];
    return NULL;
}

// A JSON table as BDAT table bytes in out (the writer's layout), or 0 with the reason in why.
static int json_table(const uint8_t *data, size_t size, const char *stem, Buf *out, char *why, size_t cap) {
    JNode *root = NULL;
    Arena arena = {0};   // the parsed JSON, given back at the end
    Buf raw = {0};
    Src *from = NULL;
    Tab t = {0};
    int ok = 0;
    uint32_t cc = 0, rc = 0;
    if (!utf8_ok(data, size)) goto notjson;
    JParse jp = {(const char *)data, (const char *)data + size, 0, 0, &arena};
    root = jvalue(&jp);
    jws(&jp);
    if (!root || jp.p != jp.end) goto notjson;
    const JNode *schema = jget(root, "schema", 6), *rows = jget(root, "rows", 4);
    if (!schema || schema->type != J_ARR || !rows || rows->type != J_ARR) {
        snprintf(why, cap, "no schema or rows");
        goto done;
    }
    cc = (uint32_t)schema->len;
    rc = (uint32_t)rows->len;
    if (!cc || !rc || cc > 4096 || rows->len > 0xffffffu) {
        snprintf(why, cap, "no columns or no rows");
        goto done;
    }
    // a table of the writer's layout with each string written where it is met: read_table then write_table give
    // the canonical bytes
    uint8_t types[4096];
    uint32_t names[4096], rl = 0;
    for (uint32_t c = 0; c < cc; c++) {
        const JNode *col = schema->items[c], *nm = jget(col, "name", 4), *ty = jget(col, "type", 4);
        if (!nm || nm->type != J_STR || !ty || ty->type != J_NUM || ty->isint != 1 || ty->i < 1 || ty->i > 13) {
            snprintf(why, cap, "column %u: no name or no known type", c);
            goto done;
        }
        types[c] = (uint8_t)ty->i;
        names[c] = hashname(nm->s, nm->len);
        rl += TSIZE[types[c]];
    }
    Buf st = {0}, rb = {0};
    uint8_t head[9] = {0};
    wr32(head + 1, hashname(stem, strlen(stem)));
    if (!put(&st, head, 9)) goto oom_st;
    for (uint32_t c = 0; c < cc; c++) {
        uint8_t h[4];
        wr32(h, names[c]);
        if (!put(&st, h, 4)) goto oom_st;
    }
    if (!put(&rb, NULL, (size_t)rl * rc)) goto oom_st;
    int64_t first = 0;
    for (uint32_t r = 0; r < rc; r++) {
        const JNode *row = rows->items[r], *id = jget(row, "$id", 3);
        if (!row || row->type != J_OBJ || !id || id->type != J_NUM || id->isint != 1) {
            snprintf(why, cap, "row %u: no $id", r);
            goto fail_st;
        }
        if (!r) first = id->i;
        if (id->i != first + r || id->i < 0 || id->i > 0xffffffffll) {
            snprintf(why, cap, "its rows are not numbered one after another (a table with only some rows is not read)");
            goto fail_st;
        }
        uint8_t *d = rb.p + (size_t)r * rl;
        for (uint32_t c = 0; c < cc; c++) {
            const JNode *nm = jget(schema->items[c], "name", 4), *v = jget(row, nm->s, nm->len);
            uint8_t ty = types[c];
            if (!v) {
                snprintf(why, cap, "row %lld: no value for column %u", (long long)id->i, c);
                goto fail_st;
            }
            int good = 1;
            if (IS_STR(ty)) {
                good = v->type == J_STR && !memchr(v->s, 0, v->len);
                if (good) {
                    if (st.n + v->len + 1 > 0xffffffffu) good = 0;
                    else {
                        wr32(d, (uint32_t)st.n);
                        if (!put(&st, v->s, v->len + 1)) goto oom_st;
                    }
                }
            } else if (ty == 8) {
                double x = v->type != J_NUM ? 0 : v->isint == 1 ? (double)v->i : v->d;
                good = v->type == J_NUM && x >= -3.4028234663852886e38 && x <= 3.4028234663852886e38;
                float f = (float)x;
                memcpy(d, &f, 4);
            } else if (ty == 9 && v->type == J_STR) {
                wr32(d, hashname(v->s, v->len));
            } else if (v->type != J_NUM || v->isint != 1) {
                good = 0;
            } else {
                int64_t x = v->i;
                int64_t lo = ty == 4 ? -128 : ty == 5 ? -32768 : ty == 6 ? INT32_MIN : 0;
                int64_t hi = ty == 4 ? 127 : ty == 5 ? 32767 : ty == 6 ? INT32_MAX : TSIZE[ty] == 1 ? 255 :
                             TSIZE[ty] == 2 ? 65535 : 0xffffffffll;
                good = x >= lo && x <= hi;
                uint32_t u = (uint32_t)x;
                memcpy(d, &u, TSIZE[ty]);
            }
            if (!good) {
                snprintf(why, cap, "row %lld column %u: not a value of its type", (long long)id->i, c);
                goto fail_st;
            }
            d += TSIZE[ty];
        }
    }
    uint32_t co = 48, ro = co + 3 * cc;
    uint64_t so = (uint64_t)ro + (uint64_t)rl * rc;
    uint8_t h[48];
    memcpy(h, "BDAT", 4);
    uint32_t f[11] = {0x3004, cc, rc, (uint32_t)first, 0, co, ro, ro, rl, (uint32_t)so, (uint32_t)st.n};
    if (so + st.n > 0xffffffffu) goto oom_st;
    for (int i = 0; i < 11; i++) wr32(h + 4 + 4 * i, f[i]);
    if (!put(&raw, h, 48)) goto oom_st;
    for (uint32_t c = 0; c < cc; c++) {
        uint8_t col[3] = {types[c]};
        wr16(col + 1, (uint16_t)(9 + 4 * c));
        if (!put(&raw, col, 3)) goto oom_st;
    }
    if (!put(&raw, rb.p, rb.n) || !put(&raw, st.p, st.n)) goto oom_st;
    free(st.p);
    free(rb.p);
    if (!read_table(raw.p, (uint32_t)raw.n, &t)) {
        snprintf(why, cap, "no table can be made of it");
        goto done;
    }
    const Tab *src[1] = {&t};
    if (!(from = malloc(((size_t)rc * cc + 1) * sizeof *from))) goto oom;
    for (uint32_t r = 0; r < rc; r++)
        for (uint32_t c = 0; c < cc; c++) from[(size_t)r * cc + c] = (Src){r, 0};
    if (!write_table(out, &t, src, from, rc, 0)) goto oom;
    ok = 1;
    goto done;
fail_st:
    free(st.p);
    free(rb.p);
    goto done;
oom_st:
    free(st.p);
    free(rb.p);
oom:
    snprintf(why, cap, "out of memory");
    goto done;
notjson:
    snprintf(why, cap, "not valid JSON");
done:
    afree(&arena);
    free(raw.p);
    free(from);
    free(t.off);
    return ok;
}

static int by_stem(const void *a, const void *b) {
    return strcmp(((const BdatJson *)a)->stem, ((const BdatJson *)b)->stem);
}

// file (the mod's full copy, or the game's) read into *out with the mod's JSON tables in place of those tables. No new
// file is written: *tabs_out (one Buf per table, empty but for the JSON ones) holds those tables' bytes for as long as
// *out is used, and file_bytes makes the file when one is needed whole.
static int apply_json(const uint8_t *file, size_t size, const BdatInput *in, int k, Events *ev, File *out,
                      Buf **tabs_out, char *why, size_t cap) {
    File f;
    char r[300];
    if (!read_file(file, size, &f, r, sizeof r)) {
        snprintf(why, cap, "%s", r);
        return 0;
    }
    BdatJson *js = malloc((size_t)in->njson * sizeof *js);
    Buf *tabs = calloc(f.count, sizeof *tabs);
    int ok = 0;
    if (!js || !tabs) goto done;
    memcpy(js, in->json, (size_t)in->njson * sizeof *js);
    qsort(js, (size_t)in->njson, sizeof *js, by_stem);
    for (int x = 0; x < in->njson; x++) {
        Buf t = {0};
        if (!json_table(js[x].data, js[x].size, js[x].stem, &t, r, sizeof r)) {
            event(ev, LEFT_OUT, k, "its table %s.json is not used (%s)", js[x].stem, r);
            free(t.p);
            continue;
        }
        uint32_t name = rd32(t.p + rd32(t.p + 40) + 1), j = 0;
        // bdat-toolset names a table known only by its hash with the 8 hex digits, no brackets ("1A2B3C4D.json"):
        // such a name is the hash when the game's file has that table (else, as any name, its murmur3)
        int hex8 = strlen(js[x].stem) == 8;
        uint32_t h = 0;
        for (int i = 0; hex8 && i < 8; i++) {
            int v = hexval((unsigned char)js[x].stem[i]);
            hex8 = v >= 0;
            h = h << 4 | (uint32_t)(v & 15);
        }
        for (uint32_t i = 0; hex8 && i < f.count; i++)
            if (f.t[i].name == h) {
                name = h;
                wr32(t.p + rd32(t.p + 40) + 1, h);   // the name is in the string table only
                break;
            }
        while (j < f.count && f.t[j].name != name) j++;
        if (j == f.count) {
            event(ev, LEFT_OUT, k, "its table %s.json is not used (the game's file has no such table)", js[x].stem);
            free(t.p);
            continue;
        }
        free(tabs[j].p);
        tabs[j] = t;
    }
    uint64_t total = 16 + 4 * (uint64_t)f.count;   // the file it would be must still fit a BDAT file's 32-bit sizes
    for (uint32_t j = 0; j < f.count; j++) total += tabs[j].p ? tabs[j].n : f.t[j].len;
    if (total > 0xffffffffu) goto done;
    for (uint32_t j = 0; j < f.count; j++) {
        Tab t;
        if (!tabs[j].p) continue;
        if (!read_table(tabs[j].p, (uint32_t)tabs[j].n, &t)) {
            snprintf(why, cap, "table %u is damaged", j);
            goto done;
        }
        free(f.t[j].off);
        f.t[j] = t;
    }
    ok = 1;
done:
    if (!ok && !why[0]) snprintf(why, cap, "out of memory");
    if (ok) {
        *out = f;
        *tabs_out = tabs;
    } else {
        for (uint32_t j = 0; tabs && j < f.count; j++) free(tabs[j].p);
        free(tabs);
        file_free(&f);
    }
    free(js);
    return ok;
}

// The file of f's tables as they are (a mod's copy with its JSON tables put in, when it is used whole).
static uint8_t *file_bytes(const File *f, size_t *out_n) {
    Buf out = {0};
    size_t head = 16 + 4 * (size_t)f->count;
    if (!put(&out, NULL, head)) return NULL;
    memcpy(out.p, "BDAT", 4);
    wr32(out.p + 4, 0x01001004);
    wr32(out.p + 8, f->count);
    for (uint32_t j = 0; j < f->count; j++) {
        wr32(out.p + 16 + 4 * j, (uint32_t)out.n);
        if (!put(&out, f->t[j].p, f->t[j].len)) {
            free(out.p);
            return NULL;
        }
    }
    wr32(out.p + 12, (uint32_t)out.n);
    *out_n = out.n;
    return out.p;
}

// The hash values a mod's changes put in cells other than keys: what its added and edited rows refer to.
static int refs_of(const File *bf, const File *mf, const Change *ch, Set *out) {
    for (uint32_t j = 0; j < bf->count; j++) {
        const Change *c = &ch[j];
        if (c->kind != CHANGES) continue;
        const Tab *b = &bf->t[j], *m = &mf->t[j];
        for (uint32_t i = 0; i < m->rc; i++) {
            uint32_t r = c->pos[i];
            if (r != NONE && !c->edited[r]) continue;
            for (uint32_t col = 1; col < m->cc; col++)
                if (ctype(m, col) == 9 && (r == NONE || !same_cell(b, r, m, i, col)) && !set_add(out, rd32(cell(m, i, col))))
                    return 0;
        }
    }
    return 1;
}

static uint8_t *copy_of(const BdatInput *in, size_t *out_n) {
    uint8_t *p = malloc(in->size ? in->size : 1);
    if (p) {
        memcpy(p, in->data, in->size);
        *out_n = in->size;
    }
    return p;
}

// ---- each mod's own work, possibly at the same time (bdat_merge_set_parallel)

static BdatParallelFor parallel;

void bdat_merge_set_parallel(BdatParallelFor f) { parallel = f; }

static void each(int n, void (*fn)(void *ctx, int i), void *ctx) {
    if (parallel && n > 1) {
        parallel(n, fn, ctx);
        return;
    }
    for (int i = 0; i < n; i++) fn(ctx, i);
}

// The events of one mod's work, kept with it and passed on in mod order afterwards, so the report reads the same
// however the work was spread: its lines one after another, each with its NUL, and its counts.
typedef struct { Buf lines; BdatSummary s; int oom; } Kept;

static void keep_line(void *ctx, const char *line) {
    Kept *k = ctx;
    if (!put(&k->lines, line, strlen(line) + 1)) k->oom = 1;
}

static void pass_on(Events *ev, const Kept *k) {
    for (size_t o = 0; ev->note && o < k->lines.n; o += strlen((const char *)k->lines.p + o) + 1)
        ev->note(ev->ctx, (const char *)k->lines.p + o);
    ev->s->priority += k->s.priority;
    ev->s->left_out += k->s.left_out;
    ev->s->caveats += k->s.caveats;
    ev->s->adds_nothing += k->s.adds_nothing;
    ev->s->partly_alike += k->s.partly_alike;
}

// A mod's file as it is merged: its JSON tables put in, then read. state: what came of it.
enum { PREP_SAME_TABLES, PREP_UNREADABLE, PREP_OTHER_TABLES };
typedef struct {
    const BdatInput *base, *mods;
    const File *bf;
    File *mf;
    Buf *tabs[MAX_MODS];     // apply_json: a JSON mod's tables' bytes, which mf[k] points into
    uint32_t ntabs[MAX_MODS];
    int state[MAX_MODS];
    char why[MAX_MODS][300];
    Kept kept[MAX_MODS];
} Prep;

static void prepare(void *ctx, int k) {
    Prep *p = ctx;
    char *r = p->why[k];
    Events ev = {keep_line, &p->kept[k], &p->kept[k].s, p->mods};
    const BdatInput *m = &p->mods[k];
    p->state[k] = PREP_UNREADABLE;
    if (m->njson) {
        // the mod's JSON tables in its full copy, or in the game's when it has none
        r[0] = 0;
        if (!apply_json(m->data ? m->data : p->base->data, m->data ? m->size : p->base->size, m, k, &ev, &p->mf[k],
                        &p->tabs[k], r, sizeof p->why[k]))
            return;
        p->ntabs[k] = p->mf[k].count;
    } else if (!m->data) {
        snprintf(r, sizeof p->why[k], "none given");
        return;
    } else if (!read_file(m->data, m->size, &p->mf[k], r, sizeof p->why[k])) {
        return;
    }
    int same = p->mf[k].count == p->bf->count;
    for (uint32_t j = 0; same && j < p->bf->count; j++) same = p->mf[k].t[j].name == p->bf->t[j].name;
    p->state[k] = same ? PREP_SAME_TABLES : PREP_OTHER_TABLES;
}

// Mod k's copy as the result, when it is used whole: its own file, or for a mod with JSON tables the file of them put in.
static uint8_t *whole_copy(const Prep *p, int k, size_t *out_n) {
    return p->tabs[k] ? file_bytes(&p->mf[k], out_n) : copy_of(&p->mods[k], out_n);
}

// A live mod's change to every table, and what its changes refer to.
typedef struct {
    const File *bf, *mf;
    Pair *const *bkeys;
    const int *bunique, *live;
    Change **ch;
    Set *refs;
    int oom[MAX_MODS];
} Compare;

static void compare(void *ctx, int x) {
    Compare *c = ctx;
    int k = c->live[x];
    uint32_t count = c->bf->count;
    if (!(c->ch[k] = calloc(count, sizeof *c->ch[k]))) {
        c->oom[x] = 1;
        return;
    }
    for (uint32_t j = 0; j < count; j++)
        if (!classify(&c->bf->t[j], c->bkeys[j], c->bunique[j], &c->mf[k].t[j], &c->ch[k][j])) {
            c->oom[x] = 1;
            return;
        }
    if (!refs_of(c->bf, &c->mf[k], c->ch[k], &c->refs[k])) c->oom[x] = 1;
}

uint8_t *bdat_merge(const BdatInput *base, const BdatInput *mods, int n, size_t *out_n, BdatSummary *sum,
                    BdatNote note, void *ctx, char *why, size_t why_cap) {
    BdatSummary s = {0};
    s.file_mod = -1;
    Events ev = {note, ctx, &s, mods};
    File bf = {0}, mf[MAX_MODS] = {{0}};
    int live[MAX_MODS], nlive = 0;
    Change *ch[MAX_MODS] = {0};
    Set refs[MAX_MODS] = {{0}};
    Buf body = {0};
    uint32_t *offs = NULL;
    uint8_t *result = NULL;
    Pair **bkeys = NULL;
    int *bunique = NULL;
    Prep *prep = NULL;
    char r[300];
    if (n < 1 || n > MAX_MODS) {
        snprintf(why, why_cap, "%d mods (1 to %d can be merged)", n, MAX_MODS);
        return NULL;
    }
    if (!read_file(base->data, base->size, &bf, r, sizeof r)) {
        snprintf(why, why_cap, "the game's copy: %s", r);
        return NULL;
    }
    // every mod's file made and read (at the same time when there are threads), then taken in priority order as if
    // one after another: a mod whose copy is used whole ends it, and the mods after it say nothing more
    if (!(prep = calloc(1, sizeof *prep))) goto oom;
    *prep = (Prep){.base = base, .mods = mods, .bf = &bf, .mf = mf};
    each(n, prepare, prep);
    for (int k = 0; k < n; k++) {
        pass_on(&ev, &prep->kept[k]);
        if (prep->kept[k].oom) goto oom;
        if (prep->state[k] == PREP_UNREADABLE) {
            event(&ev, LEFT_OUT, k, "its copy can't be read (%s)", prep->why[k]);
            continue;
        }
        if (prep->state[k] == PREP_SAME_TABLES) {
            live[nlive++] = k;
            continue;
        }
        if (!nlive) {
            // the top mod that can be read has another list of tables: its whole file, the others left out
            for (int x = k + 1; x < n; x++)
                event(&ev, LEFT_OUT, x, "its changes to this file are not used: %s's copy has another list of tables "
                      "and is used whole", mods[k].name);
            s.file_mode = BDAT_FILE_WHOLE;
            s.file_mod = k;
            if (!(result = whole_copy(prep, k, out_n))) snprintf(why, why_cap, "out of memory");
            goto done;
        }
        file_free(&mf[k]);
        event(&ev, LEFT_OUT, k, "its changes to this file are not used: its copy has another list of tables");
    }
    if (!nlive) {
        snprintf(why, why_cap, "no mod's copy can be read");
        goto done;
    }
    if (nlive == 1) {
        s.file_mode = BDAT_FILE_COPIED;
        s.file_mod = live[0];
        if (!(result = whole_copy(prep, live[0], out_n))) snprintf(why, why_cap, "out of memory");
        goto done;
    }
    // every mod's change to every table, and what its changes refer to
    bkeys = calloc(bf.count, sizeof *bkeys);
    bunique = calloc(bf.count, sizeof *bunique);
    if (!bkeys || !bunique) goto oom;
    for (uint32_t j = 0; j < bf.count; j++)
        if (ctype(&bf.t[j], 0) == 9 && !(bkeys[j] = sorted_keys(&bf.t[j], &bunique[j]))) goto oom;
    {
        Compare c = {.bf = &bf, .mf = mf, .bkeys = bkeys, .bunique = bunique, .live = live, .ch = ch, .refs = refs};
        each(nlive, compare, &c);
        for (int x = 0; x < nlive; x++)
            if (c.oom[x]) goto oom;
    }
    // changed tables a higher mod has alike: all of them, the mod adds nothing; some, partly alike
    // (docs/bdat-merge.md, "The report")
    for (int x = 1; x < nlive; x++) {
        int k = live[x], changed = 0, alike = 0, used[MAX_MODS] = {0};
        for (uint32_t j = 0; j < bf.count; j++) {
            if (ch[k][j].kind == SAME) continue;
            changed++;
            int h = -1;
            for (int y = 0; h < 0 && y < x; y++)
                if (same_table(&mf[live[y]].t[j], &mf[k].t[j])) h = y;
            if (h >= 0) {
                alike++;
                used[h] = 1;
            }
        }
        if (!alike) continue;
        char who[400];
        size_t w = 0;
        who[0] = 0;
        for (int y = 0; y < x; y++)
            if (used[y] && w < sizeof who)
                w += (size_t)snprintf(who + w, sizeof who - w, "%s%s", w ? ", " : "", mods[live[y]].name);
        if (alike == changed) event(&ev, ADDS_NOTHING, k, "every table it changes is the same in %s", who);
        else event(&ev, PARTLY_ALIKE, k, "%d of the %d tables it changes %s the same in %s", alike, changed,
                   alike == 1 ? "is" : "are", who);
    }
    s.file_mode = BDAT_FILE_MERGED;
    if (!(offs = malloc(bf.count * sizeof *offs))) goto oom;
    for (uint32_t j = 0; j < bf.count; j++) {
        const Tab *b = &bf.t[j];
        int touch[MAX_MODS], nt = 0;
        for (int x = 0; x < nlive; x++)
            if (ch[live[x]][j].kind != SAME) touch[nt++] = live[x];
        offs[j] = (uint32_t)body.n;
        const Tab *copy = NULL;
        if (!nt) copy = b;
        else if (nt == 1) copy = &mf[touch[0]].t[j];
        else if (ch[touch[0]][j].kind == OPAQUE) {
            copy = &mf[touch[0]].t[j];
            for (int x = 1; x < nt; x++)
                event(&ev, LEFT_OUT, touch[x], "table %08X: its changes are not used; %s's whole table is (%s)",
                      b->name, mods[touch[0]].name, ch[touch[0]][j].reason);
            s.tables_whole++;
        }
        if (nt) s.tables_changed++;
        if (!copy) {
            int P[MAX_MODS], np = 0;
            for (int x = 0; x < nt; x++) {
                if (ch[touch[x]][j].kind == OPAQUE)
                    event(&ev, LEFT_OUT, touch[x], "table %08X: its change is not used (%s, and %s changes the table "
                          "too)", b->name, ch[touch[x]][j].reason, mods[touch[0]].name);
                else P[np++] = touch[x];
            }
            if (np == 1) {
                copy = &mf[P[0]].t[j];
            } else {
                Job job = {.b = b, .refs = refs, .n = n, .ev = &ev};
                for (int k = 0; k < n; k++) {
                    job.m[k] = mf[k].t ? &mf[k].t[j] : NULL;
                    job.ch[k] = ch[k] ? &ch[k][j] : NULL;
                }
                if (!merge_table(&job, P, np, &body, why, why_cap)) goto done;
                s.tables_merged++;
                continue;
            }
        }
        if (!put(&body, copy->p, copy->len) || !put(&body, NULL, (4 - (body.n & 3)) & 3)) goto oom;   // tables 4-aligned
    }
    size_t head = 16 + 4 * (size_t)bf.count, total = head + body.n;
    if (total > 0xffffffffu || !(result = malloc(total))) {
        snprintf(why, why_cap, "the merged file is too big");
        goto done;
    }
    memcpy(result, "BDAT", 4);
    wr32(result + 4, 0x01001004);
    wr32(result + 8, bf.count);
    wr32(result + 12, (uint32_t)total);
    for (uint32_t j = 0; j < bf.count; j++) wr32(result + 16 + 4 * j, (uint32_t)(head + offs[j]));
    memcpy(result + head, body.p, body.n);
    *out_n = total;
    goto done;
oom:
    snprintf(why, why_cap, "out of memory");
done:
    if (bkeys)
        for (uint32_t j = 0; j < bf.count; j++) free(bkeys[j]);
    free(bkeys);
    free(bunique);
    for (int k = 0; k < n; k++) {
        if (ch[k])
            for (uint32_t j = 0; j < bf.count; j++) change_free(&ch[k][j]);
        free(ch[k]);
        free(refs[k].v);
        file_free(&mf[k]);
        if (prep) {
            free(prep->kept[k].lines.p);
            for (uint32_t j = 0; prep->tabs[k] && j < prep->ntabs[k]; j++) free(prep->tabs[k][j].p);
            free(prep->tabs[k]);
        }
    }
    free(prep);
    file_free(&bf);
    free(offs);
    free(body.p);
    if (sum) *sum = s;
    return result;
}

int bdat_rewrite_check(const uint8_t *data, size_t size) {
    File f;
    char why[200];
    if (!read_file(data, size, &f, why, sizeof why)) return -1;
    int bad = 0;
    for (uint32_t j = 0; j < f.count; j++) {
        Tab *t = &f.t[j];
        Src *from = malloc(((size_t)t->rc * t->cc + 1) * sizeof *from);
        const Tab *src[1] = {t};
        Buf out = {0};
        for (uint32_t r = 0; from && r < t->rc; r++)
            for (uint32_t c = 0; c < t->cc; c++) from[(size_t)r * t->cc + c] = (Src){r, 0};
        if (!from || !write_table(&out, t, src, from, t->rc, t->extra) || out.n != t->len || memcmp(out.p, t->p, t->len))
            bad++;
        free(from);
        free(out.p);
    }
    file_free(&f);
    return bad;
}
