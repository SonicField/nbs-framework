# Feature Request: Dashboard Sidecar Detail View

## Problem

The dashboard overview shows sidecar status as OK or MISSING. This tells you the process exists. It does not tell you whether the sidecar is functioning — whether it is processing events, responding to queries, or stuck in a loop. A live process that has stopped doing useful work looks the same as a healthy one.

The operator has no way to inspect sidecar behaviour without leaving the dashboard to find and tail the sidecar's log file manually.

## Proposal

### Sidecar detail view — press `s` on a selected agent

A new view in the dashboard that shows the tail of the selected agent's sidecar log. Same navigation pattern as the agent detail view: scrollable, horizontally pannable, Escape to return.

### What it shows

The last 8KB of the sidecar's log file, rendered as plain text (no VT processing — sidecar logs are plain text, not terminal output).

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  supervisor sidecar — PID 42381                            04 Apr 16:12:03  │
╟──────────────────────────────────────────────────────────────────────────────╢
│                                                                              │
│  [16:11:58] poll: 0 events, 0 chat-query, cursor 1752/1752                  │
│  [16:12:00] poll: 1 event (chat-message), delivered                         │
│  [16:12:02] handle_query: rendering 64KB from output.log (no screen clear)  │
│  [16:12:03] poll: 0 events, 0 chat-query, cursor 1752/1752                  │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
  Esc: back to overview  r: refresh  f: follow (auto-scroll)
```

### Sidecar log discovery

Sidecars write logs to a path specified by the `--log=PATH` flag in their command line. The dashboard finds this by:

1. `pgrep -af 'nbs-sidecar.*--handle=<agent>'` — get PID and full command line.
2. Parse `--log=<path>` from the command line.
3. `tail -c 8192 <path>` — bounded read, last 8KB.

If no `--log=` flag is found, fall back to the debug log at `/tmp/nbs-sidecar-debug-<pid>.log`. If neither exists, display "No sidecar log available."

### Navigation

Same as the agent detail view:

| Key | Action |
|-----|--------|
| Up/Down | Scroll vertically |
| Left/Right | Pan horizontally |
| Page Up/Down | Scroll by page |
| Home/End | Jump to top/bottom |
| Escape / q | Return to overview |
| r | Force refresh |
| f | Toggle follow mode |

### What does not change

- The dashboard remains read-only. It never signals or modifies sidecar processes.
- The agent detail view (Enter) is unchanged.
- The overview screen gains no new columns — sidecar status remains OK/MISSING.

### Implementation

Reuse the detail view scaffolding. The sidecar view is simpler than the agent view: no backward scan (plain text, no VT sequences), no `nbs-ts-render` (no escape processing needed), bounded read only (8KB tail).

Approximately 50 lines of new code in `dashboard.c`, plus the `s` key binding in the input handler.
