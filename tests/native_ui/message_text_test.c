// The message row helpers (runtime/message_text.h) on synthetic tables and, when XCX_MESSAGE_TABLE names the player's own message
// table file, on that file as if it were the memory the game holds: the recorded lengths and hashes must match its rows. The
// repository holds no game text; every string below is made up.
#define MESSAGE_TEXT_STORAGE
#include "message_text.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void put_u32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

// A table of the shape message_text.h looks for, at t, with `rows` rows (first ID 1) where row ids[i] has string strs[i]. Returns its
// size; off[i] receives each string's offset from the table's start.
static size_t build(uint8_t *t, uint32_t rows, unsigned n, const uint32_t *ids, const char *const *strs, uint64_t *off) {
    uint32_t str_off = 64 + rows * 10, at = 1;
    memset(t, 0, str_off);
    memcpy(t, "BDAT", 4);
    put_u32(t + 4, 0x3004); put_u32(t + 8, 3); put_u32(t + 12, rows); put_u32(t + 16, 1);
    put_u32(t + 24, 48); put_u32(t + 32, 64); put_u32(t + 36, 10); put_u32(t + 40, str_off);
    t[48] = 9; t[51] = 2; t[54] = 7; // column types
    for (unsigned i = 0; i < n; i++) {
        size_t len = strlen(strs[i]) + 1;
        memcpy(t + str_off + at, strs[i], len);
        put_u32(t + 64 + (ids[i] - 1) * 10 + 6, at);
        off[i] = str_off + at;
        at += (uint32_t)len;
    }
    put_u32(t + 44, at);
    return str_off + at;
}

int main(void) {
    // The hash is FNV-1a 64 (known vectors), and the recorded rows keep the limits the callers rely on.
    assert(message_text_hash((const uint8_t *)"", 0) == 0xcbf29ce484222325ull && message_text_hash((const uint8_t *)"a", 1) == 0xaf63dc4c8601ec8cull);
    for (unsigned k = 0; k < MESSAGE_POOL_ROWS; k++) assert(message_text_row(k)->length >= MESSAGE_TEXT_MAX && message_text_row(k)->length < MESSAGE_ORIGINAL_MAX);
    assert(message_text_row(MESSAGE_ROWS) == NULL && MESSAGE_LABEL_ROW == MESSAGE_ROWS - 1 && MESSAGE_TOAST_ROW == MESSAGE_LABEL_ROW - 1);
    assert(message_text_row(MESSAGE_TOAST_ROW)->table == MESSAGE_TABLE_NOTICE && message_text_row(MESSAGE_TOAST_ROW)->id == 0x23e &&
           message_text_row(MESSAGE_TOAST_ROW)->length == MESSAGE_TOAST_MAX);
    assert(message_text_row(MESSAGE_LABEL_ROW)->length == MESSAGE_LABEL_MAX);

    const char *file_name = getenv("XCX_MESSAGE_TABLE");
    if (file_name) { // the player's own table, before any synthetic override: each recorded row must be found exactly once
        FILE *fp = fopen(file_name, "rb");
        assert(fp);
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        uint8_t *file = malloc((size_t)size);
        assert(file && fread(file, 1, (size_t)size, fp) == (size_t)size);
        fclose(fp);
        for (unsigned k = 0; k < MESSAGE_ROWS; k++) {
            uint64_t where[4]; unsigned have = 0;
            message_text_find(file, (uint64_t)size, message_text_row(k), where, 4, &have);
            assert(have == 1);
            assert(message_text_hash(file + where[0], message_text_row(k)->length) == message_text_row(k)->hash && !file[where[0] + message_text_row(k)->length]);
            printf("player's table: row %u (id %#x) found once, %u characters\n", k, message_text_row(k)->id, message_text_row(k)->length);
        }
        free(file);
    }

    // Valid texts: 1 to MESSAGE_TEXT_MAX printable ASCII characters or newlines.
    char exact[MESSAGE_TEXT_MAX + 2], longer[MESSAGE_TEXT_MAX + 2];
    memset(exact, 'a', MESSAGE_TEXT_MAX); exact[MESSAGE_TEXT_MAX] = 0;
    memset(longer, 'a', MESSAGE_TEXT_MAX + 1); longer[MESSAGE_TEXT_MAX + 1] = 0;
    assert(message_text_valid("x") && message_text_valid("Made-up text") && message_text_valid("a\nb") && message_text_valid(exact));
    assert(!message_text_valid(NULL) && !message_text_valid("") && !message_text_valid(longer) && !message_text_valid("tab\there") &&
           !message_text_valid("caf\xc3\xa9") && !message_text_valid("bell\a"));
    // A toast text is one line of at most as many characters as the toast row; the game's text markup is plain ASCII and passes.
    char toast_exact[MESSAGE_TOAST_MAX + 1], toast_longer[MESSAGE_TOAST_MAX + 2];
    memset(toast_exact, 'c', MESSAGE_TOAST_MAX); toast_exact[MESSAGE_TOAST_MAX] = 0;
    memset(toast_longer, 'c', MESSAGE_TOAST_MAX + 1); toast_longer[MESSAGE_TOAST_MAX + 1] = 0;
    assert(message_text_toast_valid("A toast.") && message_text_toast_valid("[ST:col p1=red ]A red toast") && message_text_toast_valid(toast_exact));
    assert(!message_text_toast_valid(NULL) && !message_text_toast_valid("") && !message_text_toast_valid(toast_longer) &&
           !message_text_toast_valid("two\nlines") && !message_text_toast_valid("caf\xc3\xa9"));
    // The label and the status are short printable texts (the label as long as its row).
    char label_exact[MESSAGE_LABEL_MAX + 1], label_longer[MESSAGE_LABEL_MAX + 2];
    memset(label_exact, 'd', MESSAGE_LABEL_MAX); label_exact[MESSAGE_LABEL_MAX] = 0;
    memset(label_longer, 'd', MESSAGE_LABEL_MAX + 1); label_longer[MESSAGE_LABEL_MAX + 1] = 0;
    assert(message_text_short_valid("Label:", MESSAGE_LABEL_MAX) && message_text_short_valid(label_exact, MESSAGE_LABEL_MAX) &&
           message_text_short_valid("1 / 4", MESSAGE_STATUS_MAX) && message_text_short_valid("12345678", MESSAGE_STATUS_MAX));
    assert(!message_text_short_valid(NULL, 8) && !message_text_short_valid("", 8) && !message_text_short_valid(label_longer, MESSAGE_LABEL_MAX) &&
           !message_text_short_valid("123456789", MESSAGE_STATUS_MAX) && !message_text_short_valid("two\nlines", 20) && !message_text_short_valid("caf\xc3\xa9", 20));

    // Synthetic tables: a main-shaped table (8 rows) with the pool rows and the label row, a notice-shaped one (6 rows) with the toast row.
    static const unsigned lens[MESSAGE_ROWS] = {78, 102, 75, 46, 13};
    static const uint32_t main_ids[4] = {2, 3, 4, 5}, notice_ids[1] = {3};
    char original[MESSAGE_ROWS][MESSAGE_ORIGINAL_MAX];
    message_text_test_table(MESSAGE_TABLE_MAIN, 8, 1);
    message_text_test_table(MESSAGE_TABLE_NOTICE, 6, 1);
    for (unsigned k = 0; k < MESSAGE_ROWS; k++) {
        memset(original[k], 'a' + (int)k, lens[k]); original[k][lens[k]] = 0;
        memcpy(original[k], "row ", 4); original[k][4] = (char)('0' + k);
        message_text_test_row(k, k == MESSAGE_TOAST_ROW ? MESSAGE_TABLE_NOTICE : MESSAGE_TABLE_MAIN,
                              k == MESSAGE_TOAST_ROW ? notice_ids[0] : main_ids[k < MESSAGE_TOAST_ROW ? k : 3], lens[k],
                              message_text_hash((const uint8_t *)original[k], lens[k]));
    }
    const char *main_strs[4] = {original[0], original[1], original[2], original[MESSAGE_LABEL_ROW]}, *notice_strs[1] = {original[MESSAGE_TOAST_ROW]};
    uint8_t heap[4096] = {0};
    uint64_t moff[4], noff[1];
    uint8_t *mt = heap + 7;                          // an odd address: the helpers read words with memcpy
    size_t msize = build(mt, 8, 4, main_ids, main_strs, moff);
    uint8_t *nt = mt + ((msize + 15) & ~(size_t)15) + 3;
    size_t nsize = build(nt, 6, 1, notice_ids, notice_strs, noff);
    uint64_t pos[MESSAGE_ROWS];
    for (unsigned k = 0; k < MESSAGE_ROWS; k++)
        pos[k] = k == MESSAGE_TOAST_ROW ? (uint64_t)(nt - heap) + noff[0] : (uint64_t)(mt - heap) + moff[k < MESSAGE_TOAST_ROW ? k : 3];
    size_t end = (size_t)(nt - heap) + nsize;
    memcpy(heap + end, "neighbour", 10);               // a string after the tables

    for (unsigned k = 0; k < MESSAGE_ROWS; k++) {      // each row is found once, in its own table, and only there
        const MessageRow *row = message_text_row(k);
        uint64_t found[4]; unsigned have = 0;
        message_text_find(heap, end + 10, row, found, 4, &have);
        assert(have == 1 && found[0] == pos[k]);
        assert(message_text_holds(heap + pos[k], original[k], row->length, NULL) && message_text_holds(heap + pos[k], original[k], row->length, ""));
        assert(!message_text_holds(heap + pos[k], original[k], row->length, "Made-up text"));
        message_text_write(heap + pos[k], row->length, "Made-up text");
        assert(message_text_holds(heap + pos[k], original[k], row->length, "Made-up text") && !message_text_holds(heap + pos[k], original[k], row->length, NULL));
        assert(!strcmp((char *)heap + pos[k], "Made-up text"));
        for (size_t i = strlen("Made-up text"); i < row->length; i++) assert(!heap[pos[k] + i]); // zero fill
        uint64_t again[4]; unsigned none = 0;
        message_text_find(heap, end + 10, row, again, 4, &none);
        assert(none == 0);                              // the original is gone while the row holds another text (its hash no longer matches)
        message_text_restore(heap + pos[k], original[k], row->length);
        assert(message_text_holds(heap + pos[k], original[k], row->length, NULL) && !strcmp((char *)heap + pos[k], original[k]));
        have = 0; message_text_find(heap, end + 10, row, found, 4, &have);
        assert(have == 1);                              // found again once restored
    }
    assert(!strcmp((char *)heap + end, "neighbour"));    // the neighbour is intact

    // Another text over the first one leaves nothing of it, for the longest text and a short one.
    message_text_write(heap + pos[0], lens[0], exact);
    assert(message_text_holds(heap + pos[0], original[0], lens[0], exact));
    message_text_write(heap + pos[0], lens[0], "short");
    assert(message_text_holds(heap + pos[0], original[0], lens[0], "short") && !message_text_holds(heap + pos[0], original[0], lens[0], exact));
    message_text_restore(heap + pos[0], original[0], lens[0]);

    // Two copies of a table are both found (the cap limits how many are kept).
    {
        uint8_t two[4096] = {0};
        uint64_t off[4], f[4]; unsigned n = 0;
        size_t first = build(two + 1, 8, 4, main_ids, main_strs, off);
        build(two + 1 + ((first + 15) & ~(size_t)15), 8, 4, main_ids, main_strs, off);
        message_text_find(two, sizeof two, message_text_row(0), f, 4, &n);
        assert(n == 2);
        n = 0; message_text_find(two, sizeof two, message_text_row(0), f, 1, &n);
        assert(n == 1);
    }
    // Not found: another shape, another language or a modified table (a different hash), a row outside the table, wrong column types,
    // a table cut short, and a plain buffer with no table at all.
    {
        uint8_t t2[2048] = {0};
        uint64_t off[4], f[4]; unsigned n;
        size_t size = build(t2, 8, 4, main_ids, main_strs, off);
        n = 0; message_text_find(t2, size, message_text_row(0), f, 4, &n); assert(n == 1);
        put_u32(t2 + 12, 9);                                          // another row count
        n = 0; message_text_find(t2, size, message_text_row(0), f, 4, &n); assert(n == 0);
        put_u32(t2 + 12, 8);
        t2[off[0] + 3] ^= 1;                                          // another text of the same length
        n = 0; message_text_find(t2, size, message_text_row(0), f, 4, &n); assert(n == 0);
        t2[off[0] + 3] ^= 1;
        t2[48] = 7;                                                   // another column type
        n = 0; message_text_find(t2, size, message_text_row(0), f, 4, &n); assert(n == 0);
        t2[48] = 9;
        n = 0; message_text_find(t2, size - 3, message_text_row(3 - 3), f, 4, &n); assert(n == 0); // strings cut short
        message_text_test_row(0, MESSAGE_TABLE_MAIN, 9, lens[0], message_text_row(0)->hash);       // a row after the last
        n = 0; message_text_find(t2, size, message_text_row(0), f, 4, &n); assert(n == 0);
        message_text_test_row(0, MESSAGE_TABLE_MAIN, main_ids[0], lens[0], message_text_hash((const uint8_t *)original[0], lens[0]));
        n = 0; message_text_find(t2, size, message_text_row(0), f, 4, &n); assert(n == 1);
        n = 0; message_text_find((const uint8_t *)"\0some other bytes, no table here at all............", 50, message_text_row(2), f, 4, &n); assert(n == 0);
        // a notice-shaped table is not taken for the main one
        uint8_t t3[2048] = {0};
        size_t nsz = build(t3, 6, 1, notice_ids, notice_strs, off);
        n = 0; message_text_find(t3, nsz, message_text_row(0), f, 4, &n); assert(n == 0);
        n = 0; message_text_find(t3, nsz, message_text_row(MESSAGE_TOAST_ROW), f, 4, &n); assert(n == 1);
    }
    puts("message rows: valid texts, tables found by shape, rows by id, length and hash, write, restore, fallbacks");
    return 0;
}
