# nbs-md-viewer

Terminal-based markdown viewer. Reads markdown from stdin and renders it with full styling (colour, bold, italic, underline, box drawing) in the terminal. Read-only — navigate with keyboard, quit with `q`.

## Usage

```bash
nbs-md-viewer < README.md
cat document.md | nbs-md-viewer
nbs-md-viewer --width=100 < document.md
```

## What It Does

Parses markdown into an AST, renders it with styled display lines, and presents a scrollable pager in the terminal's alternate screen buffer. The terminal is fully restored on exit.

## Supported Markdown

| Element | Syntax | Rendering |
|---------|--------|-----------|
| Headings | `#` through `####` | Coloured text, H1/H2 with background band |
| Paragraphs | Plain text | Reflowed to terminal width |
| Bold | `**text**` | Bold attribute |
| Italic | `*text*` | Italic + subtle background hint |
| Bold italic | `***text***` | Both attributes |
| Inline code | `` `code` `` | Green on dark background |
| Code fences | ` ``` ` with optional language | Bordered block with syntax highlighting |
| Tables | GFM pipe tables | Unicode box drawing with alignment |
| Lists | `- item` or `1. item` | Bullets (•◦▪) or numbers, nested to 3 depths |
| Blockquotes | `> text` | Vertical bar with dimmed text |
| Links | `[text](url)` | Blue underlined text + dim URL |
| Horizontal rules | `---` | Full-width line |
| Hard breaks | Two trailing spaces | Line break within paragraph |

## Syntax Highlighting

Code fences with a language tag are syntax-highlighted:

| Language | Tags |
|----------|------|
| C | `c`, `h` |
| C++ | `cpp`, `c++`, `cc`, `cxx`, `hpp` |
| JavaScript | `js`, `javascript` |
| TypeScript | `ts`, `typescript` |
| Python | `py`, `python` |
| Pascal / Honest | `pas`, `pascal`, `delphi`, `honest` |

Highlighting covers keywords, types, strings, numbers, comments, and preprocessor directives. Multi-line comments and strings maintain state across lines.

## Key Bindings

| Key | Action |
|-----|--------|
| Up / Down | Scroll one line |
| Page Up / Down | Scroll one page |
| Home | Jump to top |
| End | Jump to bottom |
| Left / Right | Pan wide tables and code blocks |
| h / ? | Help screen |
| q | Quit |

On Mac: use `Fn` + Arrow for Page Up/Down/Home/End.

## Options

| Option | Default | Description |
|--------|---------|-------------|
| `--width=N` | auto-detected | Override terminal width (useful when PTY reports wrong size) |

## Architecture

Four-stage pipeline:

```
stdin → [Parser] → [AST] → [Renderer] → [Viewport] → terminal
```

- **Parser**: Two-pass (block structure, then inline formatting)
- **AST**: Immutable after construction, owned by document node
- **Renderer**: Produces styled display lines with paragraph reflow
- **Viewport**: Manages scrolling, horizontal panning, and terminal output

## Dependencies

Uses existing nbs-framework modules:

- `nbs_term_attr` — colour and style output (256-colour)
- `nbs_ts_wcwidth` — character display width (CJK, emoji)
- `nbs_ts_bidi` — bidirectional text reordering (RTL support)

No external libraries. Pure C, self-contained within nbs-framework.

## Build

```bash
cd src/nbs-md-viewer
make            # build binary + tests
make install    # install to bin/
make test       # run unit tests
make asan       # build with AddressSanitizer
```

## Terminal Requirements

Works on any terminal supporting:
- 256-colour mode (SGR 38;5;N / 48;5;N)
- Alternate screen buffer (DECSET 1049)
- UTF-8 with Unicode box drawing characters

Tested on macOS Terminal.app and iTerm2. On Windows via SSH, terminal resize may not propagate — use `--width=N` as a workaround.
