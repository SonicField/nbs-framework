# Sidecar Notifications

The sidecar is a monitoring process that runs alongside each agent session. It checks for unread chat messages and bus events, detects when the agent is idle at a prompt, and injects notification text to wake the agent up.

## Honest Type Definitions

```pascal
type
  { Sidecar configuration — monitoring-relevant subset.
    The full C struct (sidecar.h) has 21 fields including transport,
    session, and remote SSH fields not shown here. All intervals
    in seconds. A value of 0 for an interval means "disabled". }
  SidecarConfig = record
    handle               : String;    { Agent handle, max 63 chars }
    nbs_root             : String;    { Absolute path to project root }
    bus_check_interval   : LongInt;   { Seconds between bus/chat checks, default 3 }
    notify_cooldown      : LongInt;   { Min seconds between notifications, default 15 }
    startup_grace        : LongInt;   { Seconds before first notification, default 30 }
    notify_fail_threshold: LongInt;   { Failures before self-heal, default 5 }
    flush_interval       : LongInt;   { Seconds between Enter flushes }
    poll_interval        : LongInt;   { Seconds between /nbs-poll injections, 0=disabled }
    fixup_interval       : LongInt;   { Seconds between auto-fixup, default 3600 }
    librarian_interval   : LongInt;   { Seconds between librarian checks, default 900 }
    pythia_interval      : LongInt;   { Seconds between pythia checks, default 1800 }
    shepard_interval     : LongInt;   { Seconds between shepard checks, default 1200 }
  end;

  { Mutable runtime state — monitoring-relevant subset.
    The full C struct (sidecar.h) has 26 fields including timestamps,
    retry counters, and summary buffers not shown here. }
  SidecarState = record
    idle_seconds         : LongInt;   { Seconds since last content change }
    bus_check_counter    : LongInt;   { Ticks since last bus/chat check }
    last_content_hash    : LongInt;   { FNV-1a hash of last captured content }
    notify_fail_count    : LongInt;   { Consecutive notification failures }
    mention_detected     : Boolean;   { True if @mention pending injection }
    mention_payload      : String;    { Payload from the mention event }
    bus_event_count      : LongInt;   { Pending bus events }
    chat_unread_count    : LongInt;   { Unread chat messages }
  end;
```

## The Tick Loop

The sidecar runs a 1-second tick loop (`sleep(1)` at the top of each iteration). Each tick performs checks in this order:

1. **Pause check** — if `.nbs/control-pause` exists, skip all work and sleep 4 additional seconds.

2. **State invariant check** — verify internal state consistency.

3. **Control inbox processing** — read new commands from the inbox file (register/unregister chat and bus resources).

4. **Query check (`@handle?`)** — check for query bus events. If found, capture current pane content and post it to chat as the agent's status.

5. **Initial prompt injection** — if the agent hasn't received its initial prompt yet, check for prompt readiness and inject it. Has a 60-second deadline.

6. **Interrupt check (`@handle!`)** — check for interrupt bus events. If found, initiate the interrupt protocol (see below).

7. **Mention check (`@handle`)** — check for mention bus events. If found, set the mention flag and store the payload for injection during the next notification.

8. **Transport alive check** — verify the terminal session is still running.

9. **Content capture** — capture the current pane content and compute its FNV-1a hash.

10. **Content stability and idle tracking** — if the hash changed, reset `idle_seconds` and `bus_check_counter` to 0. If stable, increment `idle_seconds`.

11. **Bus/chat event check** — when `bus_check_counter >= bus_check_interval` (default 3 ticks), check for pending bus events and unread chat messages.

12. **Notification injection** — if there are events or unreads, and the agent is idle at a prompt, inject a notification.

13. **Periodic triggers** — check if any periodic actions (fixup, librarian, pythia, shepard) are due.

## Notification Format

```
[NBS-CHAT-NOTIFICATION] You have unread messages. Read them with nbs-chat read <chat-path> --unread=<handle> and respond if needed. Return to prompt when done. [THIS MESSAGE WAS MACHINE GENERATED]
```

The notification deliberately contains NO chat content. Including summaries caused agents to misinterpret content as human instructions (e.g., a summary mentioning "session end" was read as a command to shut down). The agent reads actual messages via `nbs-chat read --unread`.

### Suppression Rules

- **Sidecar-only messages:** If all pending chat messages are from the sidecar itself, no notification is sent.
- **Cooldown:** At least `notify_cooldown` seconds (default 15) between notifications, unless a @mention is pending.
- **Startup grace:** No notifications for `startup_grace` seconds (default 30) after sidecar start.

## Prompt Detection

The sidecar uses four detection functions to determine agent state:

| Function | Detects | Used For |
|----------|---------|----------|
| `detect_prompt_idle` | UTF-8 `❯` (U+276F) | Notification injection |
| `detect_prompt_ready` | `❯` OR "What should Claude do" | Interrupt delivery |
| `detect_prompt_not_trust` | `❯` AND NOT "trust this folder" | Initial prompt injection |

The prompt character `❯` indicates Claude Code is waiting for input. "What should Claude do" appears when Claude has been interrupted and is waiting for new instructions.

## Interrupt Protocol

When a `@handle!` interrupt arrives:

1. Send Escape key every 10 seconds for up to 60 seconds.
2. After each Escape, poll for prompt readiness every 1 second for 10 seconds.
3. When prompt detected: inject the notification text, send Enter.
4. If 60 seconds elapse without reaching a prompt: post `URGENT: @supervisor - agent unresponsive <handle>` to chat.
5. After 3 failed interrupt attempts on the same event, ack the event to clear it.

The interrupt protocol is designed to pull an agent out of active processing (tool execution, long responses) and back to the prompt where it can read the chat.

## Control Registry Discovery

The sidecar discovers chat files and event directories through the control registry:

```
<nbs-root>/.nbs/control-registry-<handle>
```

At startup, `registry_seed` scans `.nbs/chat/` for `.chat` files and checks for `.nbs/events/`. During operation, the sidecar processes new entries from the control inbox (see [Control Files](control-files.md)).

## Periodic Triggers

| Trigger | Default Interval | Purpose |
|---------|-----------------|---------|
| Fixup | 3,600s (1 hour) | Auto-repair cursor desync, state inconsistencies |
| Librarian | 900s (15 min) | Institutional memory maintenance |
| Pythia | 1,800s (30 min) | Trajectory and risk assessment |
| Shepard | 1,200s (20 min) | Team effectiveness assessment |

Triggers use shared timestamp files with locking for cross-sidecar deduplication — if multiple sidecars are running, only one fires the trigger. Each trigger is disabled when its interval is set to 0.

## Configuration Defaults

| Environment Variable | Default | Description |
|---------------------|---------|-------------|
| `NBS_BUS_CHECK_INTERVAL` | 3 | Seconds between bus/chat checks |
| `NBS_NOTIFY_COOLDOWN` | 15 | Min seconds between notifications |
| `NBS_STARTUP_GRACE` | 30 | Seconds before first notification |
| `NBS_NOTIFY_FAIL_THRESHOLD` | 5 | Failures before self-heal |
| `NBS_POLL_INTERVAL` | 0 | Seconds between poll injections (disabled) |
| `NBS_FIXUP_INTERVAL` | 3,600 | Seconds between fixup runs |
| `NBS_LIBRARIAN_INTERVAL` | 900 | Seconds between librarian checks |
| `NBS_PYTHIA_INTERVAL` | 1,800 | Seconds between pythia checks |
| `NBS_SHEPARD_INTERVAL` | 1,200 | Seconds between shepard checks |

## See Also

- [Bus Events](bus-events.md) — the events the sidecar monitors
- [Cursor System](cursors.md) — how unread counts are determined
- [Control Files](control-files.md) — registry, pause, and PID files
