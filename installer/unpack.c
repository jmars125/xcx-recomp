// Reads RAR (4 and 5) archives for the mod manager, which handles zips itself (installer/mods.py, archive.py).
// Compiled with the bundled clang the first time it is needed, against the part of libarchive in deps/libarchive.
//
//     unpack list ARCHIVE              one JSON object: {"format": ..., "entries": [{"name", "type", "size"}]}
//     unpack extract ARCHIVE DIR LIMIT [NAMES]
//                                      writes the regular files under DIR; {"files": N, "bytes": N}. NAMES: a file of
//                                      entry names, one per line (UTF-8, '/' separators): only those are written and
//                                      count toward LIMIT; the others are checked and skipped
//
// On failure both print {"error": ID, "detail": TEXT} and exit 1. The ids are the manager's: archive.unsafe_name,
// archive.encrypted, archive.too_big, archive.bad (libarchive could not read it), archive.write (DIR could not be
// written), archive.too_many (more entries than MAX_ENTRIES). extract checks every name again, as
// installer/archive.py does when it lists: valid UTF-8, no absolute paths, drive letters or '..', no Windows device
// names (CON, aux.bdat) or parts ending in a dot or space, and nothing but regular files and folders (no links,
// devices or sparse holes).
//
// On Windows the arguments are read as UTF-16 (wmain) and passed on as UTF-8, and the archive is opened by its wide
// name, so a path outside the system code page (a user folder in Cyrillic on an English system) works.
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "archive.h"
#include "archive_entry.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

static void json_string(const char *s) {
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static int fail(const char *id, const char *detail) {
    fputs("{\"error\": ", stdout);
    json_string(id);
    fputs(", \"detail\": ", stdout);
    json_string(detail ? detail : "");
    fputs("}\n", stdout);
    return 1;
}

#define MAX_ENTRIES 100000   // far above any mod; an archive of millions of empty entries is refused, not listed

#ifdef _WIN32
static wchar_t *wide(const char *s);
#endif

static struct archive *open_archive(const char *path, int *ok) {
    struct archive *a = archive_read_new();
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
#ifdef _WIN32
    wchar_t *w = wide(path);
    *ok = w && archive_read_open_filename_w(a, w, 1 << 16) == ARCHIVE_OK;
    free(w);
#else
    *ok = archive_read_open_filename(a, path, 1 << 16) == ARCHIVE_OK;
#endif
    return a;
}

// Whether s is valid UTF-8 (no overlong forms, surrogates or values past U+10FFFF).
static int utf8_ok(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p, n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 9;
        if (n == 9 || (n == 1 && c < 0xC2)) return 0;
        unsigned v = n ? c & (0x3Fu >> n) : c;
        for (unsigned i = 1; i <= n; i++) {
            if ((p[i] & 0xC0) != 0x80) return 0;
            v = v << 6 | (p[i] & 0x3F);
        }
        if ((n == 2 && (v < 0x800 || (v >= 0xD800 && v < 0xE000))) || (n == 3 && (v < 0x10000 || v > 0x10FFFF)))
            return 0;
        p += n + 1;
    }
    return 1;
}

// Whether a path part (len bytes) is a Windows device name, which no file may have, with any extension: CON, PRN, AUX,
// NUL, COM0-9, LPT0-9 and COM/LPT with a superscript 1-3 (Windows 11 takes those too; 2 bytes each in UTF-8: C2 B9,
// C2 B2, C2 B3) ("aux.bdat" too), compared without case.
static int device_name(const char *part, size_t len) {
    size_t stem = 0;
    while (stem < len && part[stem] != '.') stem++;
    while (stem && part[stem - 1] == ' ') stem--;   // "CON .txt" is CON as well
    char up[6] = {0};
    if (stem < 3 || stem > 5) return 0;
    for (size_t i = 0; i < stem; i++) up[i] = (char)toupper((unsigned char)part[i]);
    if (stem == 3) return !strcmp(up, "CON") || !strcmp(up, "PRN") || !strcmp(up, "AUX") || !strcmp(up, "NUL");
    if (strncmp(up, "COM", 3) && strncmp(up, "LPT", 3)) return 0;
    if (stem == 4) return up[3] >= '0' && up[3] <= '9';
    unsigned char a = (unsigned char)part[3], b = (unsigned char)part[4];
    return a == 0xC2 && (b == 0xB9 || b == 0xB2 || b == 0xB3);
}

// The entry's name with '\' as '/', or NULL when it is not one the manager may write: not UTF-8, empty, absolute, a
// drive letter, a '..' or '.' part, a control character or one of < > " | ? *, a device name, a part ending in a dot
// or space (Windows
// drops those, so two names could become one). out holds at least 4096 bytes.
static const char *safe_name(struct archive_entry *e, char *out) {
    const char *name = archive_entry_pathname_utf8(e);   // no fallback to the raw bytes: those may not be UTF-8
    if (!name || !*name || strlen(name) >= 4096 || !utf8_ok(name)) return NULL;
    size_t n = 0;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || strchr(":<>\"|?*", c)) return NULL;   // c is never 0 here
        out[n++] = c == '\\' ? '/' : (char)c;
    }
    out[n] = 0;
    while (n && out[n - 1] == '/') out[--n] = 0;   // folders may end in '/'
    if (!n || out[0] == '/') return NULL;
    for (char *part = out;;) {
        char *slash = strchr(part, '/');
        size_t len = slash ? (size_t)(slash - part) : strlen(part);
        // ".", ".." and any other part ending in a dot or space; Windows would drop that end
        if (len == 0 || part[len - 1] == '.' || part[len - 1] == ' ' || device_name(part, len)) return NULL;
        if (!slash) break;
        part = slash + 1;
    }
    return out;
}

static const char *type_of(struct archive_entry *e) {
    if (archive_entry_hardlink(e) || archive_entry_symlink(e)) return "link";
    switch (archive_entry_filetype(e)) {
    case AE_IFREG: return "file";
    case AE_IFDIR: return "dir";
    case AE_IFLNK: return "link";
    default: return "other";
    }
}

static int list(const char *path) {
    int ok;
    struct archive *a = open_archive(path, &ok);
    if (!ok) {
        int r = fail("archive.bad", archive_error_string(a));
        archive_read_free(a);
        return r;
    }
    struct archive_entry *e;
    int first = 1, r;
    unsigned long count = 0;
    char name[4096];
    printf("{\"entries\": [");
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        if (++count > MAX_ENTRIES) {
            printf("\n], \"error\": \"archive.too_many\", \"detail\": \"\"}\n");
            archive_read_free(a);
            return 1;
        }
        const char *raw = archive_entry_pathname_utf8(e);
        if (raw && !utf8_ok(raw)) raw = NULL;   // shown as "", and safe_name refuses it
        printf(first ? "\n" : ",\n");
        first = 0;
        printf(" {\"name\": ");
        json_string(raw ? raw : "");
        printf(", \"type\": \"%s\", \"size\": %lld, \"safe\": %s, \"encrypted\": %s}", type_of(e),
               (long long)archive_entry_size(e), safe_name(e, name) ? "true" : "false",
               archive_entry_is_encrypted(e) ? "true" : "false");
        archive_read_data_skip(a);
    }
    if (r != ARCHIVE_EOF) {
        // The entries so far are printed; end the object with the error so the reader gets one JSON value.
        printf("\n], \"error\": \"archive.bad\", \"detail\": ");
        json_string(archive_error_string(a) ? archive_error_string(a) : "");
        printf("}\n");
        archive_read_free(a);
        return 1;
    }
    printf("\n], \"format\": ");
    json_string(archive_format_name(a) ? archive_format_name(a) : "");
    printf("}\n");
    archive_read_free(a);
    return 0;
}

// ---- writing

#ifdef _WIN32
static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) { free(w); w = NULL; }
    return w;
}
static int make_dir(const char *path) {
    wchar_t *w = wide(path);
    if (!w) return -1;
    int r = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
    free(w);
    return r;
}
static FILE *create(const char *path) {
    wchar_t *w = wide(path);
    if (!w) return NULL;
    FILE *f = _wfopen(w, L"wbx");   // never over an existing file: two entries of one name are refused
    free(w);
    return f;
}
static FILE *open_read(const char *path) {
    wchar_t *w = wide(path);
    if (!w) return NULL;
    FILE *f = _wfopen(w, L"rb");
    free(w);
    return f;
}
#else
static int make_dir(const char *path) { return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1; }
static FILE *create(const char *path) { return fopen(path, "wbx"); }
static FILE *open_read(const char *path) { return fopen(path, "rb"); }
#endif

// The names extract writes, from a NAMES file, sorted for bsearch; the names point into text.
typedef struct {
    char *text;
    char **names;
    size_t count;
} Wanted;

static int cmp_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int read_wanted(const char *path, Wanted *w) {
    FILE *f = open_read(path);
    if (!f) return -1;
    size_t cap = 1 << 16, len = 0, n;
    char *text = malloc(cap + 1);
    while (text && (n = fread(text + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len < cap) continue;
        char *more = cap < ((size_t)64 << 20) ? realloc(text, cap * 2 + 1) : NULL;   // 64 MB of names at most
        if (!more) {
            free(text);
            text = NULL;
            break;
        }
        text = more;
        cap *= 2;
    }
    int bad = ferror(f);
    fclose(f);
    if (!text || bad) {
        free(text);
        return -1;
    }
    text[len] = 0;
    size_t lines = 1;
    for (size_t i = 0; i < len; i++) lines += text[i] == '\n';
    char **names = malloc(lines * sizeof *names);
    if (!names) {
        free(text);
        return -1;
    }
    size_t k = 0;
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = 0;
            if (end > line && end[-1] == '\r') end[-1] = 0;   // a list written with Windows line ends
        }
        if (*line) names[k++] = line;
        if (!end) break;
        line = end + 1;
    }
    qsort(names, k, sizeof *names, cmp_name);
    w->text = text;
    w->names = names;
    w->count = k;
    return 0;
}

// Whether extract writes the entry: every one without a NAMES file.
static int wanted(const Wanted *w, const char *name) {
    return !w || bsearch(&name, w->names, w->count, sizeof *w->names, cmp_name) != NULL;
}

// Makes every folder of rel (a safe name) below dir; with file set the last part is a file and is left out.
static int make_parents(const char *dir, const char *rel, int file, char *full, size_t cap) {
    if ((size_t)snprintf(full, cap, "%s/%s", dir, rel) >= cap) return -1;
    char *p = full + strlen(dir) + 1;
    for (;;) {
        char *slash = strchr(p, '/');
        if (!slash) break;
        *slash = 0;
        int r = make_dir(full);
        *slash = '/';
        if (r) return -1;
        p = slash + 1;
    }
    return file ? 0 : make_dir(full);
}

static int extract(const char *path, const char *dir, unsigned long long limit, const Wanted *only) {
    int ok;
    struct archive *a = open_archive(path, &ok);
    if (!ok) {
        int r = fail("archive.bad", archive_error_string(a));
        archive_read_free(a);
        return r;
    }
    static char buf[1 << 16];
    char name[4096], full[8192];
    struct archive_entry *e;
    unsigned long long total = 0, files = 0, seen = 0;
    int r;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        if (++seen > MAX_ENTRIES) { archive_read_free(a); return fail("archive.too_many", ""); }
        const char *rel = safe_name(e, name);
        const char *type = type_of(e);
        // a copy: the entry's own string goes away with archive_read_free, before the error line is printed
        char shown[4096];
        const char *raw = archive_entry_pathname_utf8(e);
        snprintf(shown, sizeof shown, "%s", raw && utf8_ok(raw) ? raw : "");
        if (!rel || (strcmp(type, "file") && strcmp(type, "dir")) || archive_entry_sparse_count(e)) {
            archive_read_free(a);
            return fail("archive.unsafe_name", shown);
        }
        if (archive_entry_is_encrypted(e)) {
            archive_read_free(a);
            return fail("archive.encrypted", shown);
        }
        if (!strcmp(type, "dir")) {
            if (!only && make_parents(dir, rel, 0, full, sizeof full)) { archive_read_free(a); return fail("archive.write", rel); }
            continue;
        }
        if (!wanted(only, rel)) {   // not picked: checked above, never written, not counted
            if (archive_read_data_skip(a) != ARCHIVE_OK) {
                int ret = fail("archive.bad", archive_error_string(a));
                archive_read_free(a);
                return ret;
            }
            continue;
        }
        if (make_parents(dir, rel, 1, full, sizeof full)) { archive_read_free(a); return fail("archive.write", rel); }
        FILE *f = create(full);
        if (!f) { archive_read_free(a); return fail("archive.write", rel); }
        la_ssize_t n;
        while ((n = archive_read_data(a, buf, sizeof buf)) > 0) {
            total += (unsigned long long)n;
            if (total > limit) { fclose(f); archive_read_free(a); return fail("archive.too_big", rel); }
            if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); archive_read_free(a); return fail("archive.write", rel); }
        }
        if (fclose(f) || n < 0) {
            int bad = n < 0;
            char detail[600];
            snprintf(detail, sizeof detail, "%s: %s", rel, bad && archive_error_string(a) ? archive_error_string(a) : "");
            archive_read_free(a);
            return fail(bad ? "archive.bad" : "archive.write", detail);
        }
        files++;
    }
    if (r != ARCHIVE_EOF) {
        int ret = fail("archive.bad", archive_error_string(a));
        archive_read_free(a);
        return ret;
    }
    archive_read_free(a);
    printf("{\"files\": %llu, \"bytes\": %llu}\n", files, total);
    return 0;
}

static int run(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "list")) return list(argv[2]);
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "extract")) {
        Wanted w = {0};
        if (argc == 6 && read_wanted(argv[5], &w)) return fail("archive.bad", "the list of names could not be read");
        int r = extract(argv[2], argv[3], strtoull(argv[4], NULL, 10), argc == 6 ? &w : NULL);
        free(w.names);
        free(w.text);
        return r;
    }
    fputs("usage: unpack list ARCHIVE | unpack extract ARCHIVE DIR LIMIT [NAMES]\n", stderr);
    return 2;
}

#ifdef _WIN32
// The arguments as UTF-8, whatever the system code page is.
int wmain(int argc, wchar_t **wargv) {
    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    if (!argv) return fail("archive.bad", "out of memory");
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wargv[i], -1, NULL, 0, NULL, NULL);
        if (n <= 0 || !(argv[i] = malloc((size_t)n)) ||
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wargv[i], -1, argv[i], n, NULL, NULL) != n)
            return fail("archive.bad", "an argument is not valid Unicode");
    }
    return run(argc, argv);
}
#else
int main(int argc, char **argv) { return run(argc, argv); }
#endif
