# Bus Recovery: Startup and Restart

How an agent recovers context after starting or restarting. This document must be precise enough that an agent reading it with no prior context can resume work correctly.

## On Fresh Start (no bus exists)

If `.nbs/events/` does not exist, there is no bus. Two options:

### Option A: Create the bus

```bash
mkdir -p .nbs/events/processed
```

Optionally create `config.yaml` with non-default settings. The bus works without configuration — defaults apply.

### Option B: Skip the bus

Fall back to legacy `nbs-poll` behaviour: scan `.nbs/chat/*.chat` and `.nbs/workers/*.md` directly. This is correct for projects that do not use the bus.

**Decision criterion:** if the project uses multiple agents that need to coordinate, create the bus. If it is a single-agent project, skip.

## On Restart (bus exists, events pending)

This is the critical path. An agent that restarts mid-project needs to recover.

### Step 1: Identify yourself

Determine your handle. Check in order:
1. Environment variable `NBS_HANDLE` (if set)
2. Your worker task file (`.nbs/workers/<name>.md` — the name is your handle)
3. Your role: `supervisor` if you are the supervisor, your worker name otherwise
4. Default: `claude`

### Step 2: Read configuration

```bash
cat .nbs/events/config.yaml 2>/dev/null
```

If the file does not exist, defaults apply:
- `dedup-window: 300` (planned — not yet enforced in MVP)
- `retention-max-bytes: 16777216` (16MB — controlled via `--max-bytes` on `prune` in MVP)
- `notify: inotifywait` (planned — falls back to poll)
- `poll-interval: 5` (planned)

Note: in the current MVP, `config.yaml` is not read by the bus binary. Configuration is via command-line arguments. This step can be skipped until config file support is implemented.

### Step 3: Scan pending events

```bash
nbs-bus check .nbs/events/
```

This returns all pending events sorted by priority (highest first), then by timestamp (oldest first). Read the output carefully — it tells you what happened while you were away.

### Step 4: Process events in order

For each pending event:

1. **Read it**: `nbs-bus read .nbs/events/ <event-file>`
2. **Act on it**: respond to the event as appropriate for its type
3. **Acknowledge it**: `nbs-bus ack .nbs/events/ <event-file>`

Processing rules by event type:

| Event type | Action |
|------------|--------|
| `task-blocked` | Read the worker's task file. Determine if you can unblock. If not, escalate. |
| `task-complete` | Read the worker's results. Capture 3Ws if you are the supervisor. |
| `task-failed` | Read the worker's task file and log. Determine cause. Decide whether to retry or escalate. |
| `chat-mention` | Read the chat channel. Respond to the mention. |
| `human-input` | Read the chat channel. A human is waiting — prioritise this. |
| `chat-message` | Read the chat channel for context. Respond if addressed. |
| `config-change` | Re-read `config.yaml`. Adjust behaviour. |
| `heartbeat` | Note the source is alive. No action required unless the heartbeat is unexpectedly old. |

### Step 5: Register presence

After processing pending events, publish a heartbeat to signal you are online:

```bash
nbs-bus publish .nbs/events/ <your-handle> heartbeat low "Online and recovered"
```

### Step 6: Resume normal operation

Begin your work. The bus will accumulate new events for your next check.

## On Restart (bus exists, no events pending)

Clean state. Either nothing happened while you were away, or someone already processed the events.

1. Read `config.yaml` for settings
2. Publish a heartbeat
3. Check chat channels directly for context (the bus may not have been the only communication path)
4. Resume normal operation

## Diagnosing Problems

### No events arriving

**Symptoms:** `nbs-bus check` always returns empty despite known activity.

**Checks:**
1. Is the publisher writing to the correct directory? `ls -la .nbs/events/*.event`
2. Is the publisher running? Check worker status: `nbs-worker list`
3. Is deduplication too aggressive? Check `dedup-window` in config
4. Are events being acknowledged before you see them? Check `.nbs/events/processed/` for recent entries

### Events accumulating, never processed

**Symptoms:** `.nbs/events/` fills with `.event` files. No one is reading them.

**Checks:**
1. Is the consumer running `nbs-bus check` in its poll loop?
2. Is the consumer acknowledging events after processing? Check for `nbs-bus ack` calls
3. Is the consumer's handle correct? `nbs-bus check --handle=<name>` filters by relevance

### Duplicate events

**Symptoms:** Same event appears multiple times in the queue.

**Checks:**
1. Is `dedup-key` set correctly in the publishing code?
2. Is `dedup-window` too short? Increase it in `config.yaml`
3. Is the publisher calling `nbs-bus publish` multiple times for the same logical event?

### Stale processed events consuming disc space

**Solution:**
```bash
nbs-bus prune .nbs/events/
```

Prunes oldest processed events when the `processed/` directory exceeds the configured size limit (default 16MB). Run periodically. Consider adding to the poll loop.

## Self-Test

Run these commands to verify the bus is working:

```bash
# 1. Verify directory structure
ls -la .nbs/events/
ls -la .nbs/events/processed/

# 2. Publish a test event
nbs-bus publish .nbs/events/ self-test test-event low "Bus self-test"

# 3. Verify it appears in the queue
nbs-bus check .nbs/events/
# Should show: [low] <timestamp>-self-test-test-event.event

# 4. Read it
nbs-bus read .nbs/events/ <the-event-file>
# Should show YAML with source: self-test, type: test-event

# 5. Acknowledge it
nbs-bus ack .nbs/events/ <the-event-file>

# 6. Verify it moved to processed
ls .nbs/events/processed/
# Should contain the acknowledged event file

# 7. Check status
nbs-bus status .nbs/events/
# Should show 0 pending, last activity timestamp
```

If all 7 steps succeed, the bus is operational.

## See Also

- [nbs-bus](nbs-bus.md) — Complete bus reference
- [Coordination](../concepts/coordination.md) — Why event-driven coordination matters
- [NBS Teams](nbs-teams.md) — Supervisor/worker pattern overview
