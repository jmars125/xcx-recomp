// A small pool of spare rows in the game's message tables whose text a mod can set, so a native screen's message window can
// show the mod's own words. The window is still the game's own; only the text of a row changes, in the copy of the table the
// game holds in guest memory (runtime/native_ui_adapter.c finds the rows and calls these helpers). Nothing is written to
// disk, and the texts come from the mod: the recomp only owns the mechanism.
//
// None of the game's own text is stored here. A table is found in guest memory by its shape (a BDAT table: the magic, then its
// column, row and row-size counts, its first row ID and its column types), a row by its ID, and the row is only touched when its
// string has the length and the hash recorded below, so another language or a modified table is left alone and the caller falls
// back to the game's own message. The adapter keeps a copy of a row's original bytes while it runs, in memory only, to put the
// row back. A custom text is at most MESSAGE_TEXT_MAX characters (the shortest row), is written in place and the rest of the row
// is zero-filled, so the NUL-terminated string, its neighbours and every offset in the table stay as they were.
#pragma once
#include <stdint.h>
#include <string.h>

// Rows 0 to MESSAGE_POOL_ROWS - 1 are the pool for message windows (spare rows of the main message table that no native screen of
// ours shows); MESSAGE_TOAST_ROW is the text of the game's red "not available offline" toast, which a mod may borrow while it shows a
// toast of its own, and MESSAGE_LABEL_ROW is the Details screen's timer label, which a mod may borrow while that screen is open. The
// timer's own text is a pane the adapter fills directly (MESSAGE_STATUS_MAX characters fit its width).
enum { MESSAGE_POOL_ROWS = 3, MESSAGE_ROWS = 5, MESSAGE_TOAST_ROW = 3, MESSAGE_LABEL_ROW = 4, MESSAGE_TEXT_MAX = 75, MESSAGE_TOAST_MAX = 46,
       MESSAGE_LABEL_MAX = 13, MESSAGE_STATUS_MAX = 8, MESSAGE_ORIGINAL_MAX = 128 };
enum { MESSAGE_TABLE_MAIN, MESSAGE_TABLE_NOTICE, MESSAGE_TABLES };

// What a table looks like: column count, row count, first row ID and row size in bytes. Both tables have three columns (an ID hash,
// a number and the string, types 9, 2 and 7), so the string offset is the u32 at byte 6 of a row.
typedef struct { uint32_t cols, rows, first_id, row_len; } MessageTable;
// A row: its table, its ID there, and the length and FNV-1a 64 hash of its original string (without the terminating NUL).
typedef struct { uint8_t table; uint32_t id, length; uint64_t hash; } MessageRow;

#define MESSAGE_TABLES_INIT { \
        {3, 5565, 1, 10}, /* MESSAGE_TABLE_MAIN: the main message table */ \
        {3, 683, 1, 10},  /* MESSAGE_TABLE_NOTICE: the table of the toast and notice texts */ \
    }
#define MESSAGE_ROWS_INIT { \
        {MESSAGE_TABLE_MAIN, 0x12ea, 78, 0x89d768793ec806acull},   /* spare row 0: a friends-list notice about another player's state */ \
        {MESSAGE_TABLE_MAIN, 0x12eb, 102, 0x17632301ee125364ull},  /* spare row 1: a friends-list notice about another player's squad */ \
        {MESSAGE_TABLE_MAIN, 0x12ec, 75, 0xaaec100989bf0914ull},   /* spare row 2: a friends-list notice about the squad and blocked players */ \
        {MESSAGE_TABLE_NOTICE, 0x23e, 46, 0xba0c098abb359378ull},  /* the toast row: shown through the game's toast request 0x241 */ \
        {MESSAGE_TABLE_MAIN, 0x12f1, 13, 0xe1b54f9756897a58ull},   /* the Details screen's timer label (read by the screen itself) */ \
    }
#ifdef XCX_MOD_TESTING
// Test builds keep one copy of the shapes and rows, so a test can describe synthetic tables for the adapter: the translation unit that
// defines MESSAGE_TEXT_STORAGE owns it, every other one declares it.
#ifdef MESSAGE_TEXT_STORAGE
MessageTable *message_text_tables(void) { static MessageTable tables[MESSAGE_TABLES] = MESSAGE_TABLES_INIT; return tables; }
MessageRow *message_text_rows(void) { static MessageRow rows[MESSAGE_ROWS] = MESSAGE_ROWS_INIT; return rows; }
#else
MessageTable *message_text_tables(void);
MessageRow *message_text_rows(void);
#endif
#else
static inline MessageTable *message_text_tables(void) { static MessageTable tables[MESSAGE_TABLES] = MESSAGE_TABLES_INIT; return tables; }
static inline MessageRow *message_text_rows(void) { static MessageRow rows[MESSAGE_ROWS] = MESSAGE_ROWS_INIT; return rows; }
#endif

static inline const MessageRow *message_text_row(unsigned k) { return k < MESSAGE_ROWS ? &message_text_rows()[k] : NULL; }

#ifdef XCX_MOD_TESTING
// Tests describe their own synthetic tables and rows: the shape of a table and a row's table, ID, length and hash.
static inline void message_text_test_table(unsigned t, uint32_t rows, uint32_t first_id) {
    message_text_tables()[t].rows = rows;
    message_text_tables()[t].first_id = first_id;
}
static inline void message_text_test_row(unsigned k, unsigned table, uint32_t id, uint32_t length, uint64_t hash) {
    message_text_rows()[k] = (MessageRow){(uint8_t)table, id, length, hash};
}
#endif

// FNV-1a, 64 bit, of n bytes.
static inline uint64_t message_text_hash(const uint8_t *s, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ s[i]) * 0x100000001b3ull;
    return h;
}

// A text a mod may set: 1 to MESSAGE_TEXT_MAX characters, printable ASCII or a newline.
static inline int message_text_valid(const char *text) {
    size_t n = text ? strlen(text) : 0;
    if (!n || n > MESSAGE_TEXT_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)text[i] != '\n' && ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e)) return 0;
    return 1;
}

static inline uint32_t message_text_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; } // the game runs little endian

// The offset from `t` of the string of `row`, when the `avail` readable bytes at `t` are a table of that row's shape and the row's
// string has the recorded length and hash; 0 otherwise. A loaded table keeps the offsets of the file it came from: from the table's
// start, the column definitions (3 bytes each: a type, then a name) at word 6, the rows at word 8, the row size at word 9, the strings
// at word 10 and their size at word 11 (words counted from the magic); a row's third column holds an offset into the strings.
static inline uint64_t message_text_locate(const uint8_t *t, uint64_t avail, const MessageRow *row) {
    if (avail < 48 || memcmp(t, "BDAT", 4) || row->table >= MESSAGE_TABLES) return 0;
    const MessageTable *shape = &message_text_tables()[row->table];
    uint32_t cols = message_text_u32(t + 8), rows = message_text_u32(t + 12), first = message_text_u32(t + 16);
    uint32_t coldef = message_text_u32(t + 24), rows_off = message_text_u32(t + 32), row_len = message_text_u32(t + 36);
    uint32_t str_off = message_text_u32(t + 40), str_len = message_text_u32(t + 44);
    if (cols != shape->cols || rows != shape->rows || first != shape->first_id || row_len != shape->row_len) return 0;
    if ((uint64_t)coldef + 3u * cols > avail || (uint64_t)rows_off + (uint64_t)rows * row_len > str_off || (uint64_t)str_off + str_len > avail) return 0;
    if (t[coldef] != 9 || t[coldef + 3] != 2 || t[coldef + 6] != 7) return 0;
    if (row->id < first || row->id - first >= rows || row_len < 10) return 0;
    uint32_t at = message_text_u32(t + rows_off + (uint64_t)(row->id - first) * row_len + 6);
    if ((uint64_t)at + row->length >= str_len) return 0; // the string and its terminator must lie in the strings
    const uint8_t *s = t + str_off + at;
    if (s[row->length] || memchr(s, 0, row->length) || message_text_hash(s, row->length) != row->hash) return 0;
    return (uint64_t)(str_off + at);
}

// Appends to out[*have] (up to cap) the offset in buf[0, size) of the row's string in every table found there.
static inline void message_text_find(const uint8_t *buf, uint64_t size, const MessageRow *row, uint64_t *out, unsigned cap,
                                     unsigned *have) {
    for (uint64_t i = 0; i + 48 <= size && *have < cap; i++) {
        const uint8_t *p = memchr(buf + i, 'B', (size_t)(size - 48 - i + 1));
        if (!p) break;
        i = (uint64_t)(p - buf);
        uint64_t off = message_text_locate(p, size - i, row);
        if (off) out[(*have)++] = i + off;
    }
}

// The bytes a row holds when it shows `text` (NULL or "" = the original): the text, zero-filled to the original's length.
// `original` is the row's original string (n bytes), which the caller copied from the game before changing the row.
static inline int message_text_holds(const uint8_t *at, const char *original, size_t n, const char *text) {
    if (!text || !*text) return !memcmp(at, original, n) && !at[n];
    size_t t = strlen(text);
    if (t > n || memcmp(at, text, t)) return 0;
    for (size_t i = t; i <= n; i++) if (at[i]) return 0;
    return 1;
}

// A text a mod may show in the toast: 1 to MESSAGE_TOAST_MAX printable ASCII characters on one line. The game's own text markup
// (for example the red colour tag) is plain ASCII and passes through.
static inline int message_text_toast_valid(const char *text) {
    size_t n = text ? strlen(text) : 0;
    if (!n || n > MESSAGE_TOAST_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e) return 0;
    return 1;
}

// A text for the Details screen's timer label (1 to MESSAGE_LABEL_MAX printable ASCII characters) or its timer pane (1 to
// MESSAGE_STATUS_MAX).
static inline int message_text_short_valid(const char *text, size_t max) {
    size_t n = text ? strlen(text) : 0;
    if (!n || n > max) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e) return 0;
    return 1;
}

// Puts the row's original text back (the row at `at` held some other text of at most the original's length n).
static inline void message_text_restore(uint8_t *at, const char *original, size_t n) {
    memcpy(at, original, n);
    at[n] = 0;
}

// Makes the row at `at`, whose original text has n bytes, show `text`; the caller has checked message_text_valid.
static inline void message_text_write(uint8_t *at, size_t n, const char *text) {
    size_t t = strlen(text);
    memset(at, 0, n);
    memcpy(at, text, t);
}
