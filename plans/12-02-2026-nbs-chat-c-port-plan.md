# 12-02-2026 C Port of nbs-chat — Plan

## Goal

Port `nbs-chat` (CLI tool) and `nbs-chat-terminal` (interactive client) from bash to C.

## Architecture

### Source layout

```
src/
  nbs-chat/
    main.c           — CLI entry point, arg parsing, command dispatch
    chat_file.c/.h   — Chat file protocol (read/write headers, messages)
    base64.c/.h      — Base64 encode/decode
    lock.c/.h        — fcntl-based file locking
    Makefile
  nbs-chat-terminal/
    main.c           — ncurses TUI entry point
    display.c/.h     — Message display, colour assignment
    input.c/.h       — Input handling (multi-line, /edit, /help, /exit)
    Makefile
```

### Design decisions

- **Chat file format unchanged** — the C tool must produce and consume files identical to the bash version
- **Same CLI interface** — `nbs-chat create|send|read|poll|participants|help` with same args and exit codes
- **fcntl locking** — replaces flock; uses F_SETLKW for exclusive locks on companion .lock files
- **Built-in base64** — no shelling out; standard base64 alphabet, one encoded line per message
- **Assertions** — preconditions on all public functions, postconditions where meaningful (e.g., file-length consistency)
- **ncurses for terminal client** — split screen: message area (scrolling) and input area (fixed bottom)

### Falsification

The existing 14 bash tests (`test_nbs_chat_lifecycle.sh`) test the CLI interface, not the implementation. They will run unchanged against the C binary. This is the primary verification: if the C binary passes all 14 tests, it is a faithful port.

Additional C-specific tests:
- Memory leak check via valgrind
- Base64 round-trip with edge cases (empty, 1-byte, 2-byte, 3-byte boundaries)
- fcntl lock contention (already tested by test 10: 50 concurrent sends)

## Worker decomposition

### Worker 1: Base64 + file locking library
- `base64.c/.h` — encode/decode functions
- `lock.c/.h` — acquire/release exclusive lock on file
- Unit test: base64 round-trip

### Worker 2: Chat file protocol
- `chat_file.c/.h` — create, read headers, read messages, append message, update headers
- Depends on Worker 1
- Test: create + read round-trip

### Worker 3: CLI tool (main.c)
- Arg parsing and command dispatch
- Depends on Worker 2
- Test: run all 14 bash tests against C binary

### Worker 4: ncurses terminal client
- Depends on Worker 2 (uses same chat file library)
- Separate binary
- Test: manual testing with live chat

## Build

```bash
cd src/nbs-chat && make
cd src/nbs-chat-terminal && make
```

Both produce static binaries. `install.sh` updated to build and install C binaries alongside (or replacing) bash scripts.
