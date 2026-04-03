---
description: "Diagnose and repair pathological cursor desync in nbs-chat"
allowed-tools: Bash, Read, Grep
---

# NBS Cursor Diagnosis

Guide for diagnosing and repairing pathological cursor desync in the nbs-chat notification system. Use this when agents stop receiving notifications, process old messages, or the sidecar appears stuck.

---

## Symptoms

| What you observe | Likely cause | Jump to |
|------------------|-------------|---------|
| Agent not responding to new messages | Cursor ahead of msg_count, or sidecar dead | Step 1 |
| Agent processing messages from long ago | Cursor far behind, or cursor reset to 0 | Step 2 |
| Duplicate sidecar log messages | Two sidecars running for same handle | Step 3 |
| Notification delivered but agent ignores it | Cursor already caught up, or sidecar-only suppression | Step 2 |
| No notifications after sidecar restart | Startup catch-up failed, or PID marker conflict | Step 3 |

---

## Step 1: Check cursor state

Get the authoritative message count and cursor position:

```bash
chat_file="<path-to-chat-file>"
handle="<agent-handle>"

# Message count (authoritative — uses separator-based counting)
msg_count=$(nbs-chat count "$chat_file" 2>/dev/null)

# Cursor position
cursor=$(grep "^${handle}=" "${chat_file}.cursors" 2>/dev/null | cut -d= -f2)

# Unread calculation
unread=$(( msg_count - ${cursor:-0} - 1 ))

echo "msg_count=$msg_count cursor=${cursor:-none} unread=$unread"
```

**Interpret the results:**

| Condition | Meaning | Action |
|-----------|---------|--------|
| `cursor` is empty or `-1` | Agent has never read this chat | Normal for new agents. Sidecar should notify. |
| `unread = 0` | Agent is caught up | No desync. Check sidecar health instead (Step 3). |
| `unread > 0` and `unread < 10` | Agent is slightly behind | Normal during active chat. Wait for sidecar cycle. |
| `unread > 50` | Agent is significantly behind | Cursor desync. Repair (Step 4). |
| `cursor >= msg_count` | Cursor ahead of message count | Post-archive desync. Sidecar should clamp. Repair (Step 4). |

**NEVER use `wc -l` to count messages.** It assumes a fixed header size. `nbs-chat count` uses separator-based counting and is authoritative.

**NEVER use `sed -i` on cursor files.** It bypasses the chat lock and races with concurrent writers. Use `nbs-chat cursor-set`.

---

## Step 2: Check unread delivery path

If the cursor shows unreads but the agent isn't being notified:

```bash
# Is the sidecar running?
pgrep -f "nbs-sidecar.*--handle=${handle}" >/dev/null && echo "sidecar alive" || echo "sidecar DEAD"

# Check sidecar log for recent activity
ls -t /tmp/nbs-sidecar-main-debug-*.log 2>/dev/null | head -1 | xargs tail -20
```

**Sidecar notification requires ALL of these:**
1. Sidecar process is alive
2. Unread count > 0 (cursor < msg_count - 1)
3. Unreads are NOT all from the sidecar itself (sidecar-only suppression)
4. Cooldown has expired (default 15s between notifications)
5. Terminal content is stable (not actively changing)
6. Prompt is idle (❯ character detected in capture)
7. No context stress (no "Compacting conversation" visible)

If the agent has unreads and the sidecar is alive, the most common blockers are:
- **Cooldown active:** Wait for cooldown to expire. The sidecar tracks `cooldown_suppressed` and fires a catch-up notification after cooldown ends.
- **Content not stable:** The agent is actively producing output. The sidecar waits for stability before injecting.
- **Sidecar-only messages:** If all unreads are from the sidecar handle, notifications are suppressed to prevent loops.

---

## Step 3: Check sidecar health

```bash
handle="<agent-handle>"

# Is the sidecar running?
sidecar_pid=$(pgrep -f "nbs-sidecar.*--handle=${handle}")
echo "sidecar PID: ${sidecar_pid:-NONE}"

# Check PID marker
pid_file=".nbs/pids/sidecar-${handle}.pid"
if [ -f "$pid_file" ]; then
    marker_pid=$(cat "$pid_file")
    echo "PID marker: $marker_pid"
    if [ -n "$sidecar_pid" ] && [ "$marker_pid" != "$sidecar_pid" ]; then
        echo "WARNING: PID marker does not match running sidecar"
    fi
else
    echo "No PID marker file"
fi

# Check for duplicate sidecars
dup_count=$(pgrep -fc "nbs-sidecar.*--handle=${handle}" 2>/dev/null) || dup_count=0
if [ "$dup_count" -gt 1 ]; then
    echo "DUPLICATE SIDECARS: $dup_count instances running"
    echo "Fix: kill all, then nbs-sidecar-restart ${handle}"
fi
```

**PID marker states:**

| State | Meaning | Action |
|-------|---------|--------|
| Marker matches running PID | Normal | No action |
| Marker exists, no process running | Stale marker (sidecar crashed) | Restart sidecar: `nbs-sidecar-restart ${handle}` |
| Marker PID is a different process | PID recycled by OS | Sidecar detects this via /proc/pid/cmdline and takes ownership |
| No marker file | First start or marker was cleaned | Normal — sidecar creates one on startup |
| Multiple sidecars running | Duplicate launch | Kill all, then restart one: `nbs-sidecar-restart ${handle}` |

---

## Step 4: Repair cursor desync

**Standard repair — set cursor to see recent messages:**

```bash
chat_file="<path-to-chat-file>"
handle="<agent-handle>"

# Get current count
msg_count=$(nbs-chat count "$chat_file" 2>/dev/null)

# Set cursor to msg_count - 1 (agent sees at least the last message)
nbs-chat cursor-set "$chat_file" "$handle" $((msg_count - 1))
```

**Verify the repair:**

```bash
# Confirm cursor is set
grep "^${handle}=" "${chat_file}.cursors"

# Send a test message
nbs-chat send "$chat_file" test "cursor repair verification"

# Check the agent receives notification within 30 seconds
```

**When to use different cursor values:**

| Target | Use case |
|--------|----------|
| `msg_count - 1` | Standard repair. Agent sees the last message and any new ones. |
| `msg_count - 10` | Agent needs recent context. She'll see the last 10 messages. |
| `0` | Full re-read. Only use when agent needs complete history. |

---

## Step 5: Post-archive desync

After a chat archive operation, message count drops (e.g., 2000 → 1000). Cursors pointing above the new count are invalid.

**Detection:**

```bash
msg_count=$(nbs-chat count "$chat_file" 2>/dev/null)
cursor=$(grep "^${handle}=" "${chat_file}.cursors" 2>/dev/null | cut -d= -f2)

if [ "${cursor:-0}" -ge "$msg_count" ]; then
    echo "POST-ARCHIVE DESYNC: cursor=$cursor >= msg_count=$msg_count"
fi
```

The sidecar and CLI both clamp cursors that exceed message count, but if you see this state, repair explicitly:

```bash
nbs-chat cursor-set "$chat_file" "$handle" $((msg_count - 1))
```

---

## Common pathological patterns

### Pattern 1: Notification storm after restart
**Symptom:** Agent processes dozens of old messages after sidecar restart.
**Cause:** Cursor was not reset before restart.
**Fix:** Always reset cursor before restarting an agent: `nbs-chat cursor-set "$chat_file" "$handle" $((msg_count - 1))`

### Pattern 2: PID recycling warning in sidecar log
**Symptom:** Sidecar log shows "PID N alive but not nbs-sidecar (recycled PID), taking ownership."
**Cause:** The OS assigned the old sidecar's PID to an unrelated process. The new sidecar detects the mismatch via `/proc/pid/cmdline`, takes ownership of the PID marker, and continues normally.
**Fix:** Self-healing — no action needed. If the sidecar exits instead (true duplicate: same binary, same handle), kill the older instance: `rm .nbs/pids/sidecar-${handle}.pid && nbs-sidecar-restart ${handle}`

### Pattern 3: Cooldown suppresses burst notifications
**Symptom:** 10 messages arrive in 2 seconds. Agent gets one notification, misses the rest.
**Cause:** Cooldown suppresses notifications during burst.
**Fix:** Self-healing — sidecar tracks `cooldown_suppressed` and fires catch-up notification when cooldown expires. No manual intervention needed.

### Pattern 4: Cursor advanced by --unread, new message missed
**Symptom:** Agent reads `--unread`, then a new message arrives but no notification comes.
**Cause:** Timing — the sidecar needs one `bus_check_interval` cycle to detect the new unread.
**Fix:** Self-healing within one poll cycle (default 3s). If persistent, check sidecar health (Step 3).

---

## Escalation

If cursor diagnosis does not resolve the issue:

1. Check sidecar stderr: `ls -t /tmp/nbs-sidecar-main-debug-*.log | head -1 | xargs tail -50`
2. Check for transport errors in the sidecar log (send_text/send_key failures)
3. Verify the nbs-ts session is alive: `nbs-ts status <session-handle>`
4. As a last resort: kill the sidecar, kill the agent session, reset cursor, respawn both

Report persistent desync patterns to the team — they may indicate a new root cause not covered by the existing hardening.
