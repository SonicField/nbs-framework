# Plan: Dark Colour Theme for Chat Tools

## Context

The team built a 256-colour attribute system (`nbs_term_attr`) which is now integrated into `render.c` and `nbs-chat-edit`. The current palette is 8 saturated primaries designed for generic terminals. Now that the system supports full 256-colour styles with backgrounds and attributes, we can implement a proper dark theme.

The user's terminal profile provides a midnight blue default background. The chat tools should complement this with:
- A 16-colour palette of subtle, varied pastels for agent handles
- Human messages rendered with a warm foreground and dark grey background strip
- Visual continuity between the prompt/input area and rendered human messages
- Terracotta bold for medic warnings
- Harmonised styling between `nbs-chat-terminal` and `nbs-chat-edit`

## Part 1: Design Document

New file: `docs/framework/colour-theme.md`

Documents:
- The colour model (256-colour space, the dark-background constraint)
- Why the palette uses desaturated pastels (readability on dark bg, visual fatigue)
- The 16-colour palette with colour codes, hex approximations, and design rationale
- Human message styling rationale (visibility, background strip technique)
- The `\033[K` (erase-to-end-of-line) technique for full-width background fills
- BCE (Background Color Erase) dependency and terminal compatibility
- Medic warning terracotta rationale
- Coordinate system: terminal profile owns the canvas, we paint on it

### Files
| File | Change |
|------|--------|
| `docs/framework/colour-theme.md` | New |

## Part 2: Palette and Style Constants

All in `src/nbs-common/nbs_term_attr.c` and `.h`.

### Palette expansion (8 → 16)

Replace the current `PALETTE[]` array:

```c
static const nbs_style_t PALETTE[] = {
    {  73, NBS_COLOUR_NONE, 0 },  /*  0: Soft teal      */
    { 180, NBS_COLOUR_NONE, 0 },  /*  1: Warm sand      */
    { 174, NBS_COLOUR_NONE, 0 },  /*  2: Muted rose     */
    { 108, NBS_COLOUR_NONE, 0 },  /*  3: Pale sage      */
    { 183, NBS_COLOUR_NONE, 0 },  /*  4: Soft lavender  */
    { 215, NBS_COLOUR_NONE, 0 },  /*  5: Warm amber     */
    { 110, NBS_COLOUR_NONE, 0 },  /*  6: Steel blue     */
    { 209, NBS_COLOUR_NONE, 0 },  /*  7: Dusty coral    */
    { 115, NBS_COLOUR_NONE, 0 },  /*  8: Soft mint      */
    { 186, NBS_COLOUR_NONE, 0 },  /*  9: Pale gold      */
    { 182, NBS_COLOUR_NONE, 0 },  /* 10: Mauve          */
    { 152, NBS_COLOUR_NONE, 0 },  /* 11: Powder blue    */
    { 216, NBS_COLOUR_NONE, 0 },  /* 12: Peach          */
    { 114, NBS_COLOUR_NONE, 0 },  /* 13: Spring green   */
    { 146, NBS_COLOUR_NONE, 0 },  /* 14: Wisteria       */
    { 223, NBS_COLOUR_NONE, 0 },  /* 15: Cream          */
};
```

Ordering alternates warm/cool hues for maximum visual separation between adjacent agents.

### New predefined style constants

```c
/* Human message styles */
extern const nbs_style_t NBS_STYLE_HUMAN_HANDLE;   /* fg:223 (cream), bg:236, bold */
extern const nbs_style_t NBS_STYLE_HUMAN_CONTENT;  /* fg:253 (light grey), bg:236 */
extern const nbs_style_t NBS_STYLE_HUMAN_TIMESTAMP; /* fg:245 (mid grey), bg:236, dim */
extern const nbs_style_t NBS_STYLE_HUMAN_PROMPT;   /* fg:223 (cream), bg:236, bold */

/* Medic warning style */
extern const nbs_style_t NBS_STYLE_MEDIC_WARNING;  /* fg:173 (terracotta), bold */
```

Definitions:
```c
const nbs_style_t NBS_STYLE_HUMAN_HANDLE    = { 223, 236, NBS_ATTR_BOLD };
const nbs_style_t NBS_STYLE_HUMAN_CONTENT   = { 253, 236, 0 };
const nbs_style_t NBS_STYLE_HUMAN_TIMESTAMP = { 245, 236, NBS_ATTR_DIM };
const nbs_style_t NBS_STYLE_HUMAN_PROMPT    = { 223, 236, NBS_ATTR_BOLD };
const nbs_style_t NBS_STYLE_MEDIC_WARNING   = { 173, NBS_COLOUR_NONE, NBS_ATTR_BOLD };
```

Background 236 is #303030 — a very dark warm grey that creates a subtle highlight strip against midnight blue without being jarring.

### Files
| File | Change |
|------|--------|
| `src/nbs-common/nbs_term_attr.h` | Export 5 new style constants |
| `src/nbs-common/nbs_term_attr.c` | Replace 8-colour palette with 16-colour palette, add 5 style constant definitions |

## Part 3: Render Changes

### render.h

Add a new function:

```c
/*
 * render_message_human — Render the human's own message with background highlight.
 *
 * Full-width dark grey background strip. Warm cream handle, light grey content.
 * Uses \033[K to fill remaining line width with background colour.
 */
void render_message_human(const char *handle, const char *content,
                          time_t timestamp, FILE *out);
```

Also add:

```c
/*
 * render_message_medic — Render a [MEDIC-WARNING] message.
 *
 * Terracotta bold handle, normal content.
 */
void render_message_medic(const char *handle, const char *content,
                          time_t timestamp, FILE *out);
```

### render.c

**`render_message`** — unchanged (agent messages use palette fg on default bg).

**`render_message_own`** — rewrite to become `render_message_human`:

```c
void render_message_human(const char *handle, const char *content,
                          time_t timestamp, FILE *out) {
    char ts_prefix[32];
    format_timestamp(timestamp, ts_prefix, sizeof(ts_prefix));

    /* Set background for the entire line */
    nbs_style_fstart(&NBS_STYLE_HUMAN_TIMESTAMP, out);
    fprintf(out, "  %s", ts_prefix);

    nbs_style_freset(out);
    nbs_style_fstart(&NBS_STYLE_HUMAN_HANDLE, out);
    fprintf(out, "%s", handle);

    nbs_style_freset(out);
    nbs_style_fstart(&NBS_STYLE_HUMAN_CONTENT, out);
    fprintf(out, ": %s", content);

    /* Fill rest of line with background colour, then reset */
    fprintf(out, "\033[K");
    nbs_style_freset(out);
    fprintf(out, "\n");
}
```

Keep `render_message_own` as a thin wrapper calling `render_message_human` for backward compatibility (terminal.c uses `render_message_own` via `format_message`).

**`render_message_medic`** — new:

```c
void render_message_medic(const char *handle, const char *content,
                          time_t timestamp, FILE *out) {
    char ts_prefix[32];
    format_timestamp(timestamp, ts_prefix, sizeof(ts_prefix));

    fprintf(out, "  %s%s%s", RENDER_DIM, ts_prefix, RENDER_RESET);
    nbs_style_fstart(&NBS_STYLE_MEDIC_WARNING, out);
    fprintf(out, "%s", handle);
    nbs_style_freset(out);
    fprintf(out, ": %s\n", content);
}
```

### format_message in terminal.c

Update the dispatcher to route `[MEDIC-WARNING]` messages through `render_message_medic`:

```c
static void format_message(const char *handle, const char *content,
                           const char *my_handle, time_t timestamp) {
    if (strcmp(handle, my_handle) == 0) {
        render_message_human(handle, content, timestamp, stdout);
    } else if (strncmp(handle, "[MEDIC-", 7) == 0) {
        render_message_medic(handle, content, timestamp, stdout);
    } else {
        render_message(handle, content, timestamp, stdout);
    }
}
```

### Files
| File | Change |
|------|--------|
| `src/nbs-chat/render.h` | Add `render_message_human`, `render_message_medic` declarations |
| `src/nbs-chat/render.c` | Implement both; make `render_message_own` call `render_message_human` |
| `src/nbs-chat/terminal.c` | Update `format_message` to route medic messages |

## Part 4: Prompt and Input Styling

### print_prompt in terminal.c

Apply human prompt style for visual continuity:

```c
static void print_prompt(const char *handle) {
    nbs_style_fstart(&NBS_STYLE_HUMAN_PROMPT, stdout);
    printf("%s> ", handle);
    nbs_style_freset(stdout);
    fflush(stdout);
}
```

### line_redraw in terminal.c

The input area needs the same background as human messages. Changes:

1. After clearing with `\r\033[J`, set the human content background before printing the prompt and buffer content
2. After printing the buffer, emit `\033[K` to fill the remainder of the current line with the background
3. Reset before any cursor positioning escapes

The key change is wrapping the print section:

```c
/* Set human background for entire input area */
nbs_style_fstart(&NBS_STYLE_HUMAN_CONTENT, stdout);

/* Print prompt */
print_prompt(handle);  /* already sets its own style */

/* Print buffer content */
if (ls->len > 0) { ... }

/* Fill rest of line */
printf("\033[K");
nbs_style_freset(stdout);
```

This gives visual continuity: the prompt line and the rendered human messages share the same dark grey background strip. Agent messages have the terminal's default background (midnight blue from the profile).

### Files
| File | Change |
|------|--------|
| `src/nbs-chat/terminal.c` | `print_prompt`: use `NBS_STYLE_HUMAN_PROMPT`. `line_redraw`: set human bg for input area. |

## Part 5: nbs-chat-edit Harmonisation

The editor shares `render.h` and `nbs_term_attr.h` but renders directly via `sprintf` into a buffer. It needs to use the same palette and styles.

### Changes to editor.c

**Message list rendering** (the `render` function):

- Agent messages: use `handle_colour_str` as now (already uses the palette)
- Human messages (if the editor knows the human handle — currently it doesn't): no change needed, the editor doesn't distinguish own messages
- Deleted messages: keep `RENDER_RED` + `RENDER_STRIKE` — these are editor-specific UI, not chat rendering
- Cursor highlight: keep `RENDER_REVERSE` — also editor UI

**Medic warning messages**: detect `[MEDIC-` prefix in handle and use `NBS_STYLE_MEDIC_WARNING`:

```c
if (strncmp(msg->handle, "[MEDIC-", 7) == 0) {
    /* Use terracotta for medic warnings */
    nbs_style_t style = NBS_STYLE_MEDIC_WARNING;
    char mc[NBS_STYLE_BUFSIZE];
    nbs_style_start(&style, mc, sizeof(mc));
    hcol = mc;  /* overrides handle_colour_str */
}
```

**Status bar**: use `RENDER_REVERSE` (unchanged — this is UI chrome, not chat content).

**Header bar**: use `RENDER_REVERSE` (unchanged).

**The palette change is automatic**: since the editor calls `render_init()` → `nbs_handle_colours_init()` and then `nbs_handle_colour()` → `PALETTE[]`, the 16-colour palette takes effect without any editor-specific code.

### Files
| File | Change |
|------|--------|
| `src/nbs-chat-edit/editor.c` | Medic warning handle colour override in `render()` |

## Part 6: Tests

### Unit tests (test_terminal_unit.c)

- Palette size is 16 (was 8)
- `NBS_STYLE_HUMAN_HANDLE` has fg=223, bg=236, attrs=BOLD
- `NBS_STYLE_HUMAN_CONTENT` has fg=253, bg=236, attrs=0
- `NBS_STYLE_MEDIC_WARNING` has fg=173, attrs=BOLD
- All 16 palette entries have valid fg (0-255), no bg, no attrs

### Integration tests

- `render_message_human` output contains `\033[K` (erase to end of line)
- `render_message_human` output contains bg colour escape `48;5;236`
- `render_message_medic` output contains fg colour 173
- Existing terminal tests still pass (format_message routing unchanged for agent messages)

### nbs_term_attr unit tests (test_nbs_term_attr.c)

The team wrote tests for the attr system. Add:
- Palette size is 16
- All palette entries produce valid escape sequences via `nbs_style_start`
- New style constants produce expected SGR parameters
- `NBS_STYLE_HUMAN_CONTENT` generates a sequence containing both `38;5;253` and `48;5;236`

### Files
| File | Change |
|------|--------|
| `tests/test_terminal_unit.c` | Add palette size and style constant tests |
| `src/nbs-common/test_nbs_term_attr.c` | Add palette and new constant tests |

## Files Summary

| File | Change |
|------|--------|
| `docs/framework/colour-theme.md` | New — design document |
| `src/nbs-common/nbs_term_attr.h` | Export 5 new style constants |
| `src/nbs-common/nbs_term_attr.c` | 16-colour palette, 5 style constant definitions |
| `src/nbs-chat/render.h` | Declare `render_message_human`, `render_message_medic` |
| `src/nbs-chat/render.c` | Implement human and medic message rendering |
| `src/nbs-chat/terminal.c` | `format_message` medic routing, `print_prompt` and `line_redraw` human bg |
| `src/nbs-chat-edit/editor.c` | Medic warning colour override |
| `tests/test_terminal_unit.c` | Palette and style constant tests |
| `src/nbs-common/test_nbs_term_attr.c` | Palette and new constant tests |

## What Does NOT Change

- `nbs_style_start` / `nbs_style_reset` / `nbs_style_fstart` / `nbs_style_freset` — the style API is unchanged
- `nbs_handle_colour` / `nbs_handle_colours_init` — same interface, just more palette entries
- `render_message` (agent messages) — unchanged, automatically uses new palette
- `render_get_colour` — unchanged, backward-compatible SGR parameter extraction
- INFO line rendering — stays dim, not themed
- Watchdog thread — no terminal output
- Chat file format — colours are rendering-only

## Implementation Order

1. Write `docs/framework/colour-theme.md`
2. Add style constants to `nbs_term_attr.h`
3. Replace palette and add definitions in `nbs_term_attr.c`
4. Add `render_message_human` and `render_message_medic` to `render.h/.c`
5. Update `render_message_own` to delegate to `render_message_human`
6. Update `format_message` in `terminal.c` for medic routing
7. Update `print_prompt` and `line_redraw` for human bg
8. Update `editor.c` for medic handle colour
9. Write tests
10. `make clean && make` in `src/nbs-chat/` — verify `-Werror`
11. Run unit tests and integration tests
12. Visual test: launch terminal, type messages, verify appearance

## Verification

1. Build passes with `-Werror` for both `nbs-chat` and `nbs-chat-edit`
2. All existing tests pass
3. New palette tests pass
4. Visual: agent messages show varied pastel colours on default bg
5. Visual: human messages show cream handle + light grey content on dark grey strip, full width
6. Visual: prompt has matching dark grey background
7. Visual: typing preserves the dark grey background on the input line
8. Visual: `[MEDIC-WARNING]` messages show terracotta bold handle
9. Visual: `nbs-chat-edit` shows same palette colours for handles
10. Visual: `nbs-chat-edit` shows terracotta for medic warning handles
