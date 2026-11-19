// Command-line driver for runtime/bdat_merge.c, for tests/bdat_merge_test.py and for checks against real files.
//   bdat_merge_check rewrite FILE...                  rebuild every table, print how many differ from the file's own
//   bdat_merge_check merge BASE OUT MOD...            merge (mods highest priority first), print the notes and counts
// Exit status: 0 done, 2 the files can't be merged (the reason printed), 1 a usage or I/O error.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bdat_merge.h"

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = len >= 0 ? malloc((size_t)len + 1) : NULL;
    if (p && fread(p, 1, (size_t)len, f) != (size_t)len) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *n = (size_t)len;
    return p;
}

static void print_note(void *ctx, const char *line) {
    (void)ctx;
    printf("note %s\n", line);
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "rewrite")) {
        for (int i = 2; i < argc; i++) {
            size_t n;
            uint8_t *p = slurp(argv[i], &n);
            if (!p) return 1;
            printf("rewrite %s %d\n", argv[i], bdat_rewrite_check(p, n));
            free(p);
        }
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "merge")) {
        // name=file.bdat (the mod's full copy) or name=Table.json (one of its tables), any number per name; a mod's
        // priority is where its name first appears
        BdatInput base = {0}, mods[64] = {{0}};
        static BdatJson json[64][256];
        int n = 0;
        base.name = "game";
        if (!(base.data = slurp(argv[2], &base.size))) return 1;
        for (int a = 4; a < argc; a++) {
            const char *p = argv[a], *eq = strchr(p, '=');
            if (!eq) return 1;
            char *name = malloc((size_t)(eq - p) + 1);
            memcpy(name, p, (size_t)(eq - p));
            name[eq - p] = 0;
            p = eq + 1;
            int k = 0;
            while (k < n && strcmp(mods[k].name, name)) k++;
            if (k == n) {
                if (n == 64) return 1;
                mods[n].name = name;
                mods[n].json = json[n];
                n++;
            }
            size_t len = strlen(p), sz;
            const uint8_t *data = slurp(p, &sz);
            if (!data) return 1;
            if (len > 5 && !strcmp(p + len - 5, ".json")) {
                if (mods[k].njson == 256) return 1;
                const char *slash = strrchr(p, '/'), *back = strrchr(p, '\\');
                const char *file = slash && (!back || slash > back) ? slash + 1 : back ? back + 1 : p;
                char *stem = malloc(strlen(file) - 4);
                memcpy(stem, file, strlen(file) - 5);
                stem[strlen(file) - 5] = 0;
                json[k][mods[k].njson++] = (BdatJson){stem, data, sz};
            } else {
                mods[k].data = data;
                mods[k].size = sz;
            }
        }
        size_t out_n = 0;
        BdatSummary s;
        char why[512];
        uint8_t *out = bdat_merge(&base, mods, n, &out_n, &s, print_note, NULL, why, sizeof why);
        if (!out) {
            printf("unmergeable %s\n", why);
            return 2;
        }
        FILE *f = fopen(argv[3], "wb");
        if (!f || fwrite(out, 1, out_n, f) != out_n) return 1;
        fclose(f);
        char file[100] = "merged";
        if (s.file_mode != BDAT_FILE_MERGED)
            snprintf(file, sizeof file, "%s:%s", s.file_mode == BDAT_FILE_WHOLE ? "whole" : "copied",
                     mods[s.file_mod].name);
        printf("summary changed=%d merged=%d whole=%d priority=%d leftout=%d caveat=%d nothing=%d alike=%d file=%s\n", s.tables_changed,
               s.tables_merged, s.tables_whole, s.priority, s.left_out, s.caveats,
               s.adds_nothing, s.partly_alike, file);
        return 0;
    }
    fprintf(stderr, "usage: bdat_merge_check rewrite FILE... | merge BASE OUT MOD...\n");
    return 1;
}
