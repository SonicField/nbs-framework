# nbs-ts-render

Virtual terminal renderer. Reads raw PTY output from stdin, processes it through a full terminal emulator, and outputs the final screen state as plain UTF-8 text.

## Usage

```bash
cat ~/.nbs-ts/sessions/<handle>/output.log | nbs-ts-render
nbs-chat export .nbs/chat/poem.chat | nbs-ts-render
nbs-ts-render --width=120 --height=40 < output.log
```

## What It Does

Terminal sessions produce raw output containing ANSI escape sequences — cursor movement, colours, scrolling, erase commands. This is what `output.log` contains. `nbs-ts-render` processes all of it through a headless terminal emulator and outputs what a human would see on screen.

All decoration (colour, bold, italic, underline) is stripped. The output is plain text.

## Options

| Option | Default | Description |
|--------|---------|-------------|
| `--width=N` | 80 | Screen width in columns |
| `--height=N` | 24 | Screen height in rows |

The defaults match the PTY size used by `nbs-ts-helper`.

## Supported Escape Sequences

- **Cursor**: CUP, CUU, CUD, CUF, CUB, CNL, CPL, CHA, VPA, HVP
- **Erase**: ED, EL, ECH
- **Scroll**: SU, SD, DECSTBM (scroll regions)
- **Insert/Delete**: IL, DL, ICH, DCH
- **Tabs**: HT, HTS, TBC
- **Cursor save**: DECSC (ESC 7), DECRC (ESC 8)
- **Line control**: LF, CR, BS, IND, NEL, RI
- **Reset**: RIS (ESC c)
- **UTF-8**: Full multi-byte character support
- **Wide characters**: CJK and emoji (wcwidth-aware)
- **Bidi**: UAX #9 RTL support with bracket mirroring

SGR (colour/style), OSC, and DCS sequences are silently consumed.

## Newline Mode

Bare `\n` (LF without preceding CR) resets the cursor to column 0 before moving down. This matches terminal newline mode (LNM) and ensures Unix text (which uses bare `\n`) renders correctly without staircase-stepping across the screen.

## Common Uses

```bash
# See what an agent's terminal looks like right now
cat ~/.nbs-ts/sessions/$(nbs-ts find nbs-supervisor-poem)/output.log | nbs-ts-render

# Render a chat export as plain text
nbs-chat export .nbs/chat/poem.chat | nbs-ts-render

# Render with a wider terminal
cat output.log | nbs-ts-render --width=160 --height=50
```

## Build

```bash
cd src/nbs-ts-render
make && make install
```
