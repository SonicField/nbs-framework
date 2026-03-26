# nbs-chat-edit

Interactive terminal editor for nbs-chat files. View, search, and delete messages without touching the binary format by hand.

## Usage

```bash
nbs-chat-edit <file>
```

Opens the chat file in a full-screen terminal view. Messages are decoded and displayed with coloured handles, timestamps, and content previews. Changes are staged (marked) before writing — nothing touches the file until you press `w`.

## Navigation

| Key | Action |
|-----|--------|
| Up/Down, j/k | One message |
| Page Up/Down | One screen |
| Home, g | First message |
| End, G | Last message |
| / | Search forward (regex) |
| n | Next match |
| N | Previous match |
| Enter, v | View full message |

## Editing

| Key | Action |
|-----|--------|
| d | Mark/unmark message for deletion |
| t | Truncate — mark this message and everything after |
| u | Undo last action |
| Ctrl-R | Redo |

Marked messages show in red with strikethrough. No changes are applied until you write.

## File Operations

| Key | Action |
|-----|--------|
| w | Write changes (atomic — backs up, recreates, re-sends surviving messages) |
| q | Quit (warns if unsaved changes) |
| Q | Force quit without saving |
| h, ? | Help screen |

## Write Safety

On write, the editor:

1. Renames the original file to `<file>.edit-backup`
2. Creates a fresh chat file
3. Re-sends each surviving message via `chat_send`
4. Deletes the backup on success
5. Restores the backup if anything fails

The header (participant counts, last-writer, file-length) is recalculated automatically.

## Common Tasks

**Delete shutdown noise** (sidecar URGENT messages at the end):

1. Open the file
2. Press `G` to go to the end
3. Navigate up to the last real message
4. Press `t` on the first junk message (marks everything after for deletion)
5. Press `w` to write

**Find and delete specific messages:**

1. Press `/` and type a regex (e.g. `sidecar`)
2. Press `n` to cycle through matches
3. Press `d` on each one to mark for deletion
4. Press `w` when done

**View a long message in full:**

1. Navigate to the message
2. Press Enter or `v`
3. Any key to return

## Build

```bash
cd src/nbs-chat-edit
make && make install
```

Depends on `chat_file.c`, `base64.c`, and `lock.c` from `src/nbs-chat/` (compiled directly, no library dependency).
