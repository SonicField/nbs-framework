# Control Files

Control files are simple format files that support sidecar resource discovery, team coordination, and process tracking. They do not use Honest notation — their formats are intentionally minimal.

## Control Registry

### Purpose

The control registry tells the sidecar which chat files and event directories to monitor. Each agent has its own registry file.

### File Location

```
<nbs-root>/.nbs/control-registry-<handle>
```

Example: `.nbs/control-registry-supervisor`

### Format

Line-oriented, `type:path` format:

```
chat:/project/.nbs/chat/team.chat
chat:/project/.nbs/chat/work.chat
bus:/project/.nbs/events
```

**Entry types:**
- `chat:` — a chat file to monitor for unread messages
- `bus:` — an event directory to monitor for pending events

### Honest Type Definition (for reference)

```pascal
type
  { A single registry entry mapping a resource type to a path. }
  RegistryEntryType = (Chat, Bus);

  RegistryEntry = record
    entry_type : RegistryEntryType;
    path       : String;
  end;

  { The complete registry: one entry per monitored resource. }
  ControlRegistry = sequence of RegistryEntry;
```

### Operations

**Seed:** At sidecar startup, `registry_seed` scans the project for existing resources:
- `.nbs/chat/*.chat` files (excluding archive files with `-archive.` in the name)
- `.nbs/events/` directory

**Mutation:** The registry is only modified by append or remove of complete lines, written atomically via temp file + rename.

### Control Inbox

Each agent also has a control inbox for receiving commands:

```
<nbs-root>/.nbs/control-inbox-<handle>
```

The sidecar reads new lines from the inbox using a forward-only line offset (monotonically non-decreasing). Supported commands: `register-chat`, `unregister-chat`, `register-bus`, `unregister-bus`.

## Pause File

### Purpose

The pause file halts all sidecar activity without killing agents. Agents keep their context and session alive, but receive no notifications, triggers, or polling.

### File Location

```
<nbs-root>/.nbs/control-pause
```

### Format

Existence-only — the file content does not matter. If the file exists, the system is paused. If it does not exist, the system is running.

### Behaviour

**Sidecar:** When the pause file exists, the sidecar skips all work and sleeps for 5 seconds per cycle (1s tick + 4s additional). No content capture, no bus checks, no query/interrupt/mention processing, no periodic triggers.

**Terminal:** The terminal watchdog skips restart checks while paused.

**Created by:** `/pause` command.

**Deleted by:** `/resume` command.

## PID Files

### Purpose

PID files track active worker processes (oracle triggers, periodic workers).

### File Location

```
<nbs-root>/.nbs/pids/<slug>.pid
```

Example: `.nbs/pids/pythia-20260330-142530.pid`

### Format

Contains the worker process PID as a decimal integer, newline-terminated.

### Lifecycle

- **Created:** When a worker is spawned.
- **Deleted:** When the worker exits or on team restart.

### Cleanup on Restart

`nbs-chat-terminal-restart.sh` deletes all PID files:

```bash
rm -f "${PROJECT_ROOT}/.nbs/pids/"*.pid
```

## See Also

- [Sidecar Notifications](sidecar.md) — the sidecar that uses the control registry
- [Session Metadata](session-metadata.md) — per-session metadata files
