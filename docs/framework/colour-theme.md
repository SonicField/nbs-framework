# NBS Chat Colour Theme

Design document for the colour theme used by `nbs-chat-terminal` and `nbs-chat-edit`.

## 1. Design Philosophy

The terminal profile owns the canvas. A midnight blue background is set in the user's terminal profile, not by the chat tools. The tools paint on this canvas — they set foreground colours and per-line background highlights, but never force a global background.

This separation matters because terminal emulators repaint the background on resize, scroll, and clear. If the application sets a background colour, those repaints produce visual artefacts: flashes of the default background between frames, mismatched strips after a resize. By leaving the background to the terminal profile, the tools avoid this entire class of problems.

## 2. The 256-Colour Space

The theme uses the xterm 256-colour model (`xterm-256color`). The 256 indices break into four regions:

| Range | Contents |
|-------|----------|
| 0--7 | Standard colours (black, red, green, yellow, blue, magenta, cyan, white) |
| 8--15 | High-intensity variants of the standard colours |
| 16--231 | 6x6x6 RGB cube: `colour = 16 + 36r + 6g + b`, where r, g, b are each 0--5 |
| 232--255 | Greyscale ramp: 232 is near-black, 255 is near-white |

SGR (Select Graphic Rendition) sequences address these as `38;5;N` for foreground and `48;5;N` for background.

## 3. Dark Background Palette Design

Saturated primaries are wrong for dark backgrounds:

- **Visual fatigue.** High-saturation colours (red 196, blue 39, yellow 226) cause fatigue over long reading sessions. Chat sessions run for hours.
- **Camouflage.** Pure blue vanishes into a midnight blue background.
- **Hierarchy distortion.** Bright yellow and red dominate the visual hierarchy. Agent handles should be peers, not a shouting match.

The palette uses desaturated pastels from the 256-colour cube — medium-high lightness, low-to-medium saturation. These read clearly on dark backgrounds without fatigue.

### Handle Palette

Sixteen colours, ordered to alternate warm and cool hues so that agents assigned adjacent indices are maximally distinct:

| Index | Name | Code | Hex | Hue family |
|-------|------|------|---------|------------|
| 0 | Soft teal | 73 | #5fafaf | Cool |
| 1 | Warm sand | 180 | #d7af87 | Warm |
| 2 | Muted rose | 174 | #d78787 | Warm |
| 3 | Pale sage | 108 | #87af87 | Cool |
| 4 | Soft lavender | 183 | #d7afff | Cool |
| 5 | Warm amber | 215 | #ffaf5f | Warm |
| 6 | Steel blue | 110 | #87afd7 | Cool |
| 7 | Dusty coral | 209 | #ff875f | Warm |
| 8 | Soft mint | 115 | #87d7af | Cool |
| 9 | Pale gold | 186 | #d7d787 | Warm |
| 10 | Mauve | 182 | #d7afd7 | Warm |
| 11 | Powder blue | 152 | #afd7d7 | Cool |
| 12 | Peach | 216 | #ffaf87 | Warm |
| 13 | Spring green | 114 | #87d787 | Cool |
| 14 | Wisteria | 146 | #afafd7 | Cool |
| 15 | Cream | 223 | #ffd7af | Warm |

The first seven entries (indices 0--6) are assigned to the standard team roles: scribe, medic, supervisor, gatekeeper, theologian, testkeeper, generalist. These seven are chosen for strong mutual contrast.

Assignment is deterministic: handles receive colours in first-seen order. The mapping is stable within a session and resets on `nbs_handle_colours_init()`.

## 4. Human Message Styling

### Problem

The old `render_message_own` applied `DIM` (SGR 2) to both handle and content. On a dark background, dim text is nearly invisible — the human's own messages disappeared into the canvas.

### Solution

Human messages use a distinct styling that separates them from agent messages without suppressing readability:

| Element | Foreground | Background | Attribute |
|---------|------------|------------|-----------|
| Handle | 223 (cream) | 236 (#303030, dark grey) | Bold |
| Content | 253 (light grey) | 236 (#303030, dark grey) | -- |
| Timestamp | 245 (mid grey) | 236 (#303030, dark grey) | Dim |
| Prompt | 223 (cream) | 236 (#303030, dark grey) | Bold |

The dark grey background (236) creates a subtle highlight strip against the midnight blue canvas. This visually groups human messages without competing with agent colours.

### Full-width background

The background fills the full terminal width using the `\033[K` (Erase to End of Line) escape sequence. When a background colour is active, `\033[K]` erases the remainder of the line in that colour rather than the default. This is BCE (Background Color Erase), supported by all modern terminal emulators: xterm, iTerm2, kitty, alacritty, Windows Terminal.

The prompt and input area share the same dark grey background for visual continuity — the input line has the same strip as rendered human messages.

## 5. Medic Warning Styling

Medic warnings use terracotta (173, #d7875f) with bold:

| Element | Foreground | Background | Attribute |
|---------|------------|------------|-----------|
| Medic warning | 173 (terracotta) | -- (default) | Bold |

No background colour — warnings stand on the default background like agent messages, but the distinctive colour and weight mark them as special.

Why not red: red (196) is the colour of errors and failures. Medic warnings are observations about reasoning quality, not system errors. Terracotta says "pay attention" without saying "something broke".

## 6. Terminal Compatibility

The theme requires:

- **256-colour support** — `TERM` must be `xterm-256color` or equivalent.
- **BCE** — Background Color Erase for full-width background strips on human messages.

Terminals without 256-colour support fall back to the 8 basic colours via the terminal emulator's own mapping. The styles degrade gracefully: the exact colour is lost but readability is preserved because the palette avoids relying on specific shades for semantic meaning. Colour distinguishes agents from each other; it does not encode information.

## 7. Implementation

All colours are expressed as `nbs_style_t` structs (defined in `src/nbs-common/nbs_term_attr.h`). No raw escape sequences appear in rendering code. The struct holds foreground, background, and attribute bitmask:

```c
typedef struct {
    int fg;            /* 0-255 or NBS_COLOUR_NONE */
    int bg;            /* 0-255 or NBS_COLOUR_NONE */
    unsigned attrs;    /* Bitmask of NBS_ATTR_* */
} nbs_style_t;
```

The `nbs_style_start()` and `nbs_style_fstart()` functions generate correct SGR sequences for any combination of foreground, background, and attributes.

The palette lives in `src/nbs-common/nbs_term_attr.c` and is accessed via `nbs_handle_colour()`. Predefined styles for human messages (`NBS_STYLE_HUMAN_HANDLE`, `NBS_STYLE_HUMAN_CONTENT`, etc.) and medic warnings (`NBS_STYLE_MEDIC_WARNING`) are declared in `nbs_term_attr.h`.

Rendering functions in `src/nbs-chat/render.c` consume these styles. The `render_get_colour()` function bridges the style API to the legacy SGR-parameter format used by `fprintf`-based rendering.
