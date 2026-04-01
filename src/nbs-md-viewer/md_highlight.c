/*
 * md_highlight.c — Syntax highlighting engine.
 *
 * Implements the language registry lookup and token-to-style mapping.
 * Each language file (md_lang_c.c, etc.) registers itself as a static
 * md_lang_t with a self-contained tokenise callback.
 */

#include "md_highlight.h"
#include "md_style.h"

#include <string.h>
#include <strings.h>

/* ── language registry ───────────────────────────────────────────── */

extern const md_lang_t md_lang_c;
extern const md_lang_t md_lang_cpp;
extern const md_lang_t md_lang_js;
extern const md_lang_t md_lang_ts;
extern const md_lang_t md_lang_py;
extern const md_lang_t md_lang_pas;

static const md_lang_t *lang_registry[] = {
    &md_lang_c, &md_lang_cpp, &md_lang_js, &md_lang_ts,
    &md_lang_py, &md_lang_pas, NULL
};

/* ── public API ──────────────────────────────────────────────────── */

const md_lang_t *md_highlight_find_lang(const char *name) {
    if (!name || !*name) return NULL;
    for (int i = 0; lang_registry[i]; i++) {
        const md_lang_t *lang = lang_registry[i];
        /* Check canonical name */
        if (strcasecmp(name, lang->name) == 0)
            return lang;
        /* Check aliases */
        if (lang->aliases) {
            for (int j = 0; lang->aliases[j]; j++) {
                if (strcasecmp(name, lang->aliases[j]) == 0)
                    return lang;
            }
        }
    }
    return NULL;
}

const nbs_style_t *md_highlight_token_style(md_hl_token_t token) {
    switch (token) {
    case MD_HL_KEYWORD:  return &MD_STYLE_HL_KEYWORD;
    case MD_HL_TYPE:     return &MD_STYLE_HL_TYPE;
    case MD_HL_STRING:   return &MD_STYLE_HL_STRING;
    case MD_HL_NUMBER:   return &MD_STYLE_HL_NUMBER;
    case MD_HL_COMMENT:  return &MD_STYLE_HL_COMMENT;
    case MD_HL_PREPROC:  return &MD_STYLE_HL_PREPROC;
    case MD_HL_OPERATOR: return &MD_STYLE_HL_OPERATOR;
    case MD_HL_NORMAL:   return &MD_STYLE_CODE_FENCE;
    }
    return &MD_STYLE_CODE_FENCE;
}
