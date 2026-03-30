# NBS Chat System Architecture

A comprehensive reference for the NBS chat system. Every data format, protocol, and file structure is documented here. Data structures are defined using Honest notation — the prose explains *why*, the Honest definitions specify *what*.

## Reading Order

1. **[Chat File Format](chat-file.md)** — the `.chat` file: header fields, base64-encoded messages, wire formats, atomic writes, locking.

2. **[Cursor System](cursors.md)** — per-agent read tracking via `.chat.cursors` files, sender auto-advance, cursor adjustment during archiving.

3. **[Bus Events](bus-events.md)** — the `.nbs/events/` directory: event files, priority levels, deduplication, the publish → ack → prune lifecycle. @mention extraction, @team expansion, interrupt and query flags.

4. **[Session Metadata](session-metadata.md)** — the `.nbs/sessions/<handle>.honest` file: fields, who writes, who reads, when deleted. Specification for the Honest format.

5. **[Sidecar Notifications](sidecar.md)** — the sidecar monitoring loop: tick model, prompt detection, notification injection, interrupt handling, periodic triggers.

6. **[Control Files](control-files.md)** — the control registry, pause file, PID files. Simple formats that support sidecar resource discovery and team coordination.

7. **[Archive and Truncation](archive.md)** — auto-archiving when chat files grow large, archive file naming, cursor survival, terminal detection of archive events.

## Honest Notation Convention

Honest type definitions appear in `` ```pascal `` code blocks. They describe the *logical structure* of data stored in various ad-hoc formats (key=value, base64, flat files). They do NOT imply that these formats should be migrated to Honest — only session metadata is being migrated. The Honest notation is used because it is self-describing and unambiguous, making it a better documentation tool than prose tables.

Comments in Honest blocks explain constraints that the type system alone cannot express:

```pascal
type
  { Timestamp is Unix epoch seconds.
    A value of 0 means "no timestamp" (legacy message). }
  ChatMessage = record
    timestamp : LongInt;
    handle    : String;
    content   : String;
  end;
```

## Source Files

| Component | Primary Sources |
|-----------|----------------|
| Chat file format | `src/nbs-chat/chat_file.h`, `src/nbs-chat/chat_file.c` |
| Cursor system | `src/nbs-chat/chat_file.h`, `src/nbs-chat/chat_file.c` |
| Bus events | `src/nbs-bus/bus.h`, `src/nbs-bus/bus.c` |
| Mention extraction | `src/nbs-chat/bus_bridge.c`, `src/nbs-common/nbs_mention.h` |
| Session metadata | `bin/nbs-claude`, `src/nbs-workers/worker.c` |
| Sidecar | `src/nbs-sidecar/sidecar.c`, `src/nbs-sidecar/sidecar.h` |
| Control files | `src/nbs-sidecar/registry.h`, `src/nbs-sidecar/registry.c` |
| Archive/truncation | `src/nbs-chat/chat_file.c`, `src/nbs-chat/terminal.c` |
