/*
 * nbs-help — Search the NBS framework manifest.
 *
 * Drop-in replacement for the bash version. Parses MANIFEST.honest
 * once into memory, searches in microseconds instead of spawning
 * hundreds of honest-get subprocesses.
 *
 * Usage:
 *   nbs-help <query>              Search by keyword (AND-matched)
 *   nbs-help --kind=tool <query>  Filter to a specific kind
 *   nbs-help --list               List all entries grouped by kind
 *   nbs-help --list --kind=tool   List entries of one kind
 *   nbs-help --manifest=PATH      Use a specific manifest file
 *   nbs-help --help               Show help
 *
 * Exit codes: 0 = ok, 1 = error, 2 = usage
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "honest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>

#define MAX_ENTRIES 512
#define MAX_KEYWORDS 32
#define MAX_FIELD 4096
#define MAX_QUERY_WORDS 16

/* --- Manifest entry --- */

typedef struct {
    char kind[32];
    char name[256];
    char path[MAX_FIELD];
    char summary[MAX_FIELD];
    char when_to_use[MAX_FIELD];
    char keywords[MAX_KEYWORDS][256];
    int keyword_count;
} entry_t;

static entry_t g_entries[MAX_ENTRIES];
static int g_entry_count = 0;
static char g_manifest_root[PATH_MAX] = "";

/* --- Honest helpers --- */

static const hon_value *find_field(const hon_value *rec, const char *name) {
    if (!rec || rec->kind != HON_VAL_RECORD) return NULL;
    hon_str s = {name, strlen(name)};
    for (uint32_t i = 0; i < rec->u.record.field_count; i++) {
        if (hon_str_eq(rec->u.record.fields[i].name, s))
            return rec->u.record.fields[i].value;
    }
    return NULL;
}

static void val_to_str(const hon_value *v, char *buf, size_t bufsz) {
    buf[0] = '\0';
    if (!v) return;
    if (v->kind == HON_VAL_STRING) {
        size_t n = v->u.string.len;
        if (n >= bufsz) n = bufsz - 1;
        memcpy(buf, v->u.string.data, n);
        buf[n] = '\0';
    } else if (v->kind == HON_VAL_ENUM) {
        size_t n = v->u.enum_val.len;
        if (n >= bufsz) n = bufsz - 1;
        memcpy(buf, v->u.enum_val.data, n);
        buf[n] = '\0';
    }
}

/* --- Load manifest --- */

static int load_manifest(const char *path) {
    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    if (!doc) {
        fprintf(stderr, "Error: failed to parse %s\n", path);
        for (uint32_t i = 0; i < diags.count; i++)
            fprintf(stderr, "  %s\n", diags.items[i].message);
        hon_diag_free(&diags);
        return -1;
    }
    hon_diag_free(&diags);

    /* Find the "manifest" variable */
    const hon_value *manifest = NULL;
    for (uint32_t i = 0; i < doc->var_count; i++) {
        if (hon_str_eq(doc->vars[i].name, (hon_str){"manifest", 8})) {
            manifest = doc->vars[i].value;
            break;
        }
    }
    if (!manifest || manifest->kind != HON_VAL_RECORD) {
        fprintf(stderr, "Error: no 'manifest' record in %s\n", path);
        hon_doc_free(doc);
        return -1;
    }

    /* Get entries sequence */
    const hon_value *entries = find_field(manifest, "entries");
    if (!entries || entries->kind != HON_VAL_SEQUENCE) {
        fprintf(stderr, "Error: manifest has no 'entries' sequence\n");
        hon_doc_free(doc);
        return -1;
    }

    g_entry_count = 0;
    for (uint32_t i = 0; i < entries->u.sequence.count && g_entry_count < MAX_ENTRIES; i++) {
        const hon_value *e = &entries->u.sequence.elements[i];
        if (e->kind != HON_VAL_RECORD) continue;

        entry_t *ent = &g_entries[g_entry_count];
        val_to_str(find_field(e, "kind"), ent->kind, sizeof(ent->kind));
        val_to_str(find_field(e, "name"), ent->name, sizeof(ent->name));
        val_to_str(find_field(e, "path"), ent->path, sizeof(ent->path));
        val_to_str(find_field(e, "summary"), ent->summary, sizeof(ent->summary));
        val_to_str(find_field(e, "when_to_use"), ent->when_to_use, sizeof(ent->when_to_use));

        /* Keywords sequence */
        ent->keyword_count = 0;
        const hon_value *kw = find_field(e, "keywords");
        if (kw && kw->kind == HON_VAL_SEQUENCE) {
            for (uint32_t k = 0; k < kw->u.sequence.count && ent->keyword_count < MAX_KEYWORDS; k++) {
                const hon_value *kv = &kw->u.sequence.elements[k];
                if (kv->kind == HON_VAL_STRING) {
                    size_t n = kv->u.string.len;
                    if (n >= sizeof(ent->keywords[0])) n = sizeof(ent->keywords[0]) - 1;
                    memcpy(ent->keywords[ent->keyword_count], kv->u.string.data, n);
                    ent->keywords[ent->keyword_count][n] = '\0';
                    ent->keyword_count++;
                }
            }
        }
        g_entry_count++;
    }

    hon_doc_free(doc);
    return 0;
}

/* --- Search --- */

static void str_tolower(char *dst, const char *src, size_t n) {
    for (size_t i = 0; i < n && src[i]; i++)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[n > 0 ? n - 1 : 0] = '\0';
}

static int entry_matches_kind(const entry_t *e, const char *kind_filter) {
    if (!kind_filter || !kind_filter[0]) return 1;
    char ek[32], kf[32];
    str_tolower(ek, e->kind, sizeof(ek));
    str_tolower(kf, kind_filter, sizeof(kf));
    return strcmp(ek, kf) == 0;
}

static int entry_matches_query(const entry_t *e, const char *words[], int word_count) {
    /* Build searchable text: name + summary + when_to_use + keywords */
    char searchable[MAX_FIELD * 3 + MAX_KEYWORDS * 256];
    int off = 0;
    off += snprintf(searchable + off, sizeof(searchable) - (size_t)off, "%s %s %s",
                    e->name, e->summary, e->when_to_use);
    for (int k = 0; k < e->keyword_count && off < (int)sizeof(searchable) - 256; k++)
        off += snprintf(searchable + off, sizeof(searchable) - (size_t)off, " %s", e->keywords[k]);

    /* Lowercase */
    for (int i = 0; searchable[i]; i++)
        searchable[i] = (char)tolower((unsigned char)searchable[i]);

    /* AND-match all words */
    for (int w = 0; w < word_count; w++) {
        if (!strstr(searchable, words[w]))
            return 0;
    }
    return 1;
}

/* --- Output --- */

static void print_entry(const entry_t *e) {
    printf("  \033[1m%s\033[0m — %s\n", e->name, e->summary);
    if (g_manifest_root[0])
        printf("    Path: %s/%s\n", g_manifest_root, e->path);
    else
        printf("    Path: %s\n", e->path);
    printf("    Use when: %s\n", e->when_to_use);
}

static const char *KIND_ORDER[] = {"Tool", "Skill", "Document", "Concept"};
static const int KIND_COUNT = 4;

static void print_grouped(const int *indices, int count, const char *kind_filter) {
    for (int ki = 0; ki < KIND_COUNT; ki++) {
        if (kind_filter && kind_filter[0]) {
            char kf[32];
            str_tolower(kf, kind_filter, sizeof(kf));
            char ko[32];
            str_tolower(ko, KIND_ORDER[ki], sizeof(ko));
            if (strcmp(kf, ko) != 0) continue;
        }

        int header = 0;
        for (int j = 0; j < count; j++) {
            const entry_t *e = &g_entries[indices[j]];
            if (strcmp(e->kind, KIND_ORDER[ki]) != 0) continue;
            if (!header) {
                printf("\n\033[1;4m%ss\033[0m\n", KIND_ORDER[ki]);
                header = 1;
            }
            print_entry(e);
            printf("\n");
        }
    }
}

/* --- Find manifest --- */

static int find_manifest(const char *argv0, char *out, size_t outsz) {
    /* Try relative to binary */
    char self[PATH_MAX];
    ssize_t rlen = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (rlen > 0) {
        self[rlen] = '\0';
        char *slash = strrchr(self, '/');
        if (slash) {
            /* Go up two levels: bin/nbs-help -> repo root */
            *slash = '\0';
            slash = strrchr(self, '/');
            if (slash) {
                *slash = '\0';
                int n = snprintf(out, outsz, "%s/MANIFEST.honest", self);
                if (n > 0 && (size_t)n < outsz && access(out, R_OK) == 0)
                    return 0;
            }
        }
    }
    /* Try cwd upward */
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        char *dir = cwd;
        while (dir[0]) {
            int n = snprintf(out, outsz, "%s/MANIFEST.honest", dir);
            if (n > 0 && (size_t)n < outsz && access(out, R_OK) == 0)
                return 0;
            char *up = strrchr(dir, '/');
            if (!up || up == dir) break;
            *up = '\0';
        }
    }
    (void)argv0;
    return -1;
}

/* --- Main --- */

int main(int argc, char **argv) {
    char manifest_path[PATH_MAX] = "";
    const char *kind_filter = NULL;
    int list_mode = 0;
    const char *query_words[MAX_QUERY_WORDS];
    int query_word_count = 0;
    char query_lower_buf[MAX_QUERY_WORDS][256];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: nbs-help <query>              Search by keyword\n"
                   "       nbs-help --kind=<kind> <query> Filter by kind (tool/skill/document/concept)\n"
                   "       nbs-help --list                List all entries grouped by kind\n"
                   "       nbs-help --list --kind=<kind>  List entries of one kind\n"
                   "\n"
                   "Searches name, summary, when_to_use, and keywords. Case-insensitive.\n");
            return 0;
        } else if (strncmp(argv[i], "--kind=", 7) == 0) {
            kind_filter = argv[i] + 7;
        } else if (strcmp(argv[i], "--list") == 0) {
            list_mode = 1;
        } else if (strncmp(argv[i], "--manifest=", 11) == 0) {
            snprintf(manifest_path, sizeof(manifest_path), "%s", argv[i] + 11);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 2;
        } else {
            /* Query words — split on spaces, lowercase each */
            char tmp[1024];
            snprintf(tmp, sizeof(tmp), "%s", argv[i]);
            char *tok = strtok(tmp, " \t");
            while (tok && query_word_count < MAX_QUERY_WORDS) {
                str_tolower(query_lower_buf[query_word_count], tok,
                            sizeof(query_lower_buf[0]));
                query_words[query_word_count] = query_lower_buf[query_word_count];
                query_word_count++;
                tok = strtok(NULL, " \t");
            }
        }
    }

    /* Find manifest */
    if (!manifest_path[0]) {
        if (find_manifest(argv[0], manifest_path, sizeof(manifest_path)) != 0) {
            fprintf(stderr, "Error: MANIFEST.honest not found\n");
            return 1;
        }
    }

    /* Derive manifest root for path display */
    {
        char *rp = realpath(manifest_path, NULL);
        if (rp) {
            char *sl = strrchr(rp, '/');
            if (sl) *sl = '\0';
            snprintf(g_manifest_root, sizeof(g_manifest_root), "%s", rp);
            free(rp);
        }
    }

    /* Load */
    if (load_manifest(manifest_path) != 0)
        return 1;

    if (list_mode) {
        int all[MAX_ENTRIES];
        for (int i = 0; i < g_entry_count; i++) all[i] = i;
        print_grouped(all, g_entry_count, kind_filter);
        return 0;
    }

    if (query_word_count == 0) {
        fprintf(stderr, "Usage: nbs-help <query>\n"
                        "       nbs-help --list\n");
        return 2;
    }

    /* Search */
    int matches[MAX_ENTRIES];
    int match_count = 0;
    for (int i = 0; i < g_entry_count; i++) {
        if (!entry_matches_kind(&g_entries[i], kind_filter)) continue;
        if (entry_matches_query(&g_entries[i], query_words, query_word_count))
            matches[match_count++] = i;
    }

    if (match_count == 0) {
        printf("No matches for:");
        for (int w = 0; w < query_word_count; w++)
            printf(" %s", query_words[w]);
        printf("\n");
        return 0;
    }

    print_grouped(matches, match_count, kind_filter);
    return 0;
}
