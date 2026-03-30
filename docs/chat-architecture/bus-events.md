# Bus Events

The NBS Bus is a file-based event queue in `.nbs/events/`. Each event is a single YAML file that flows through a publish → check → ack → prune lifecycle. The bus is the coordination backbone for @mentions, interrupts, and cross-agent signalling.

## Honest Type Definitions

```pascal
type
  { Priority levels, lowest number = highest urgency.
    critical: system failures, unresponsive agents
    high: @mentions, queries
    normal: routine coordination
    low: housekeeping }
  Priority = (Critical, High, Normal, Low);

  { A bus event as stored in its YAML file.
    dedup_key is "source:type" and is used for deduplication.
    payload is optional free-form text (multi-line). }
  BusEvent = record
    source     : String;     { Handle of the publishing agent }
    event_type : String;     { e.g. "chat-mention", "chat-interrupt" }
    priority   : Priority;
    timestamp  : String;     { ISO 8601 }
    dedup_key  : String;     { "source:type" }
    payload    : String;     { Optional free-form text }
  end;

  { The in-memory representation used by the C API.
    timestamp_us is microseconds for sub-second ordering.
    All LongInt fields are stored as 64-bit integers internally
    (int64_t in C, not 32-bit Pascal LongInt). }
  BusEventParsed = record
    filename     : String;     { Just the filename, not the full path }
    source       : String;
    event_type   : String;     { C field name: "type" (bus.h:100); renamed
                                  here because "type" is a Pascal reserved word }
    priority     : LongInt;    { 0=critical, 1=high, 2=normal, 3=low; int in C }
    timestamp_us : LongInt;    { Unix microseconds (~1.7e15), requires 64-bit }
  end;

  { Bus configuration from events_dir/config.yaml. }
  BusConfig = record
    retention_max_bytes : LongInt;   { Max bytes in processed/ before pruning, default 16 MB }
    dedup_window_s      : LongInt;   { Default dedup window in seconds, 0 = disabled }
    ack_timeout_s       : LongInt;   { Stale event threshold in seconds, 0 = disabled }
  end;
```

## Event File Format

### Filename Convention

```
<unix-timestamp-us>-<source-handle>-<event-type>-<pid>.event
```

Example: `1711881137000000-supervisor-chat-mention-12345.event`

The timestamp is Unix microseconds, ensuring unique filenames even for events published in rapid succession from the same source.

### File Content

```yaml
source: supervisor
type: chat-mention
priority: high
timestamp: 2026-03-30T14:52:17+0000
dedup-key: supervisor:chat-mention
payload: |
  @gatekeeper from supervisor: Please review the pending commit.
```

**source** — handle of the agent that published the event. Non-empty, no whitespace.

**type** — event type string. Non-empty, no whitespace. Common types:
- `chat-mention` — a normal @mention in chat
- `chat-interrupt` — an @handle! interrupt request
- `chat-query` — an @handle? status query

**priority** — one of `critical`, `high`, `normal`, `low`.

**timestamp** — ISO 8601 timestamp of publication.

**dedup-key** — `source:type`. Used by `bus_publish_dedup` to prevent duplicate events within a configurable time window.

**payload** — optional multi-line text. Uses YAML literal block scalar (`|`). Truncated to 2,048 bytes for mention payloads; the full message remains in the chat file.

## Directory Structure

```
.nbs/events/
  <event1>.event          — pending events
  <event2>.event
  processed/
    <event3>.event        — acknowledged events
  config.yaml             — optional bus configuration
```

## Event Lifecycle

### 1. Publish

`bus_publish` creates an event file atomically (write to temp, then rename). The filename is printed to stdout. Events are created in `.nbs/events/`.

### 2. Check

`bus_check` lists pending events sorted by priority (critical first), then by timestamp (oldest first). The sidecar checks for pending events every `BUS_CHECK_INTERVAL` seconds (default: 3).

### 3. Ack

`bus_ack` moves a single event from `.nbs/events/` to `.nbs/events/processed/` via atomic rename. `bus_ack_all` acknowledges all pending events for a given handle.

The sidecar acks bus events automatically after injecting a notification. Agents do not need to ack manually.

### 4. Prune

`bus_prune` deletes the oldest processed events when the total size of `processed/` exceeds `retention_max_bytes` (default: 16 MB).

## Deduplication

`bus_publish_dedup` scans pending events before publishing. If an event with the same dedup-key (`source:type`) exists within `dedup_window_us` microseconds, the new event is dropped (returns `BUS_EXIT_DEDUP = 5`).

Per-target dedup keys prevent cross-agent deduplication: `@gatekeeper?` and `@supervisor?` in the same message produce different dedup keys (`chat-query-gatekeeper` vs `chat-query-supervisor`).

## @Mention Extraction

The `bus_extract_mentions` function in `bus_bridge.c` scans decoded chat messages for @mentions:

**Pattern:** `@` followed by `[a-zA-Z0-9_-]+`

**Email exclusion:** If the character before `@` is an email-prefix character (`[a-zA-Z0-9._+-]`), the mention is skipped. This prevents `user@example.com` from being parsed as `@example`.

**Deduplication:** Duplicate mentions within the same message are silently removed.

**Maximum:** 16 mentions per message (`MAX_MENTIONS`).

### Mention Types

| Syntax | Flag | Event Type | Priority |
|--------|------|------------|----------|
| `@handle` | 0 (normal) | `chat-mention` | high |
| `@handle!` | 1 (interrupt) | `chat-interrupt` | critical |
| `@handle?` | 2 (query) | `chat-query` | high |

The `!` and `?` suffixes also accept LLM-escaped forms (`\!` and `\?`), since language models sometimes escape markdown special characters.

### Payload Format

```
@handle from sender: full_message_text
```

### @team Expansion

When the mention target is `team`, the bus bridge publishes one event per participant in the chat file — excluding the sender and the `sidecar` handle. Each expanded event inherits the interrupt/query flag from the original `@team!` or `@team?` mention.

Participants are read from the chat file header (`participants:` line).

## Limits

| Constant | Value | Description |
|----------|-------|-------------|
| `BUS_MAX_HANDLE` | 128 | Maximum handle length |
| `BUS_MAX_TYPE` | 128 | Maximum event type length |
| `BUS_MAX_PAYLOAD` | 16,384 | Maximum payload size (bus API) |
| `BUS_MAX_EVENTS` | 256 | Maximum pending events |
| `MAX_MENTIONS` | 16 | Maximum mentions per message |
| `MAX_PAYLOAD_LEN` | 2,048 | Mention payload truncation limit |

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error |
| 2 | Events directory not found |
| 3 | Event not found |
| 4 | Invalid arguments |
| 5 | Deduplicated (event dropped) |

## Bracket Handles

Some chat messages use bracket-delimited handles (e.g. `[MEDIC-WARNING]`, `[SIDECAR-ERROR]`) instead of normal agent handles. These are system message types with distinct visual rendering.

| Handle | Subcommand | Style | Purpose |
|--------|-----------|-------|---------|
| `[MEDIC-WARNING]` | `nbs-chat warn` | Terracotta bold (fg 173) | Medic agent warnings |
| `[SIDECAR-ERROR]` | `nbs-chat error` | Dusty red bold (fg 167) | Sidecar failures (query errors, escalations) |

**Enforcement:** `nbs-chat send` rejects brackets in handles at the binary level. Only dedicated subcommands can create bracket-handle messages. This prevents agents from spoofing system message types.

**Routing:** All dispatch points (terminal.c, main.c export, editor.c) use `handle_style_lookup()` from `handle_styles.h` — a table of exact-match handle-to-style mappings. Adding a new bracket handle type requires one row in the table and one style constant in `nbs_term_attr`. Unregistered bracket handles fall through to the default palette renderer.

## See Also

- [Sidecar Notifications](sidecar.md) — how the sidecar consumes bus events
- [Chat File Format](chat-file.md) — the source of @mention messages
- [Control Files](control-files.md) — the registry that maps handles to event directories
