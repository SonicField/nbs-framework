# nbs-file-browser: Terminal File Browser

Full-screen TUI for navigating directories, viewing files with syntax highlighting, and opening an editor. Integrated into `nbs-chat-terminal` as the `/file` command.

## Usage

```bash
nbs-file-browser [--state-file=PATH] [directory]
```

Without arguments, opens the current working directory. With a path, opens that directory. The `--state-file` flag writes the final directory to a file on exit — used by `nbs-chat-terminal` for directory memory.

## Keys

| Key | Action |
|-----|--------|
| Up/Down | Navigate files |
| Left/Right | Jump between columns |
| Enter | View file (bat for code, nbs-md-viewer for markdown) |
| `e` | Edit file in `$EDITOR` |
| Tab / Shift-Tab | Jump 10 files forward/back |
| Page Up/Down | Jump one screenful |
| Home/End | First/last file |
| `r` | Refresh directory listing |
| Escape | Exit |

## File Viewing

| File type | Viewer |
|-----------|--------|
| `.md` | `nbs-md-viewer` — NBS markdown rendering with styled headings and tables |
| Other text | `bat --paging=always --style=numbers` — syntax highlighting for ~200 languages |
| Binary | Status bar message — press `e` to open in editor |

Binary detection scans the first 512 bytes for null characters.

## Display

Multi-column layout with automatic column count based on terminal width (up to 4 columns). Files are sorted directories-first, then alphabetical. Colour coding by type:

| Type | Colour |
|------|--------|
| Directory | Bold blue |
| Markdown | Magenta |
| Source code | Green |
| Data/config | Cyan |
| Executable | Bold red |
| Hidden (dotfiles) | Dim |
| Other | Yellow |

Source extensions recognised: `.c`, `.h`, `.py`, `.sh`, `.js`, `.ts`, `.rs`, `.go`, `.cpp`, `.hpp`.
Data extensions recognised: `.json`, `.yaml`, `.yml`, `.toml`, `.honest`, `.xml`, `.csv`, `.cfg`, `.ini`, `.conf`.

## Integration with nbs-chat-terminal

The `/file` command in `nbs-chat-terminal` launches `nbs-file-browser` with directory memory:

```
/file              Open last directory (or cwd on first use)
/file docs/tools   Open a specific path
```

The browser writes its final directory to a temporary state file on exit. The terminal reads it back, so subsequent `/file` invocations resume where you left off. This memory persists for the lifetime of the terminal process.

## Prerequisites

- **bat** — syntax-highlighted file viewing (build-time requirement, checked by Makefile)
- **nbs-md-viewer** — markdown rendering (build-time requirement)

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Normal exit |
| 1 | Cannot resolve path or read directory |

## See Also

- [nbs-chat-terminal](nbs-chat-terminal.md) — the chat terminal that hosts `/file`
- [nbs-md-viewer](nbs-md-viewer.md) — markdown viewer used for `.md` files
