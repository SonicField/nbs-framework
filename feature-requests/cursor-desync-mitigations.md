# Feature Request: Cursor Desync Mitigations

## Problem

Agents regularly fall behind on chat — their cursor (the "how many messages have I seen" counter in `<chat>.cursors`) desyncs from the actual message count. The agent appears alive and working but is processing stale messages or receiving no notifications. Fixup and sidecar restarts correct the symptom, but the root causes recur.

## Desync Scenarios

Ten scenarios have been identified, ordered by likelihood of causing regular desyncs in production.

### 1. Fixup resets cursor too aggressively

**Mechanism:** Fixup resets the cursor to current message count for scribe and medic on every cycle (mandatory restart). If messages arrive between the cursor reset and the new sidecar starting, those messages are missed.

**Frequency:** Every fixup cycle (every 20 minutes when active).

**Evidence pattern:** Scribe/medic miss 1-2 messages per fixup cycle. Consistent, small gap.

**Mitigation:** Reset the cursor to `msg_count - 1` instead of `msg_count`, so the restarted agent always gets at least the most recent message as context. Alternatively, delay the cursor reset until after the sidecar is confirmed running.

### 2. Concurrent cursor file writes (sed -i race)

**Mechanism:** Fixup/kick uses `sed -i` to update cursors. `sed -i` writes a temp file and renames — atomic on ext4 for a single writer. But if two processes update the same cursor file simultaneously (fixup resetting scribe while scribe's sidecar is advancing), one write clobbers the other.

**Frequency:** Whenever fixup runs during active sidecar operation.

**Evidence pattern:** Random cursor value — either too high (agent thinks she's seen everything) or too low (agent re-processes old messages).

**Mitigation:** Use the chat lock (`<chat>.lock`) when modifying the cursor file. Both sidecar and fixup should acquire the lock before reading/writing cursors.

### 3. Sidecar cooldown suppresses notifications during bursts

**Mechanism:** The sidecar has a notification cooldown to avoid flooding the agent. During high-activity bursts (5+ messages in a few seconds), the sidecar delivers one notification, sets cooldown, and suppresses subsequent ones. The agent reads one message, advances cursor by 1, but misses the burst. Cursor falls behind.

**Frequency:** During active team discussion, multiple agents posting simultaneously.

**Evidence pattern:** Agent falls behind by 3-10 messages during bursts, may catch up during quiet periods (if the sidecar re-checks), or may stay behind until the next explicit notification.

**Mitigation:** After cooldown expires, the sidecar should re-check the cursor vs message count and deliver a catch-up notification if the agent is behind. Currently, it only checks on new messages — it does not re-check after cooldown.

### 4. Agent reads chat directly, advancing cursor independently

**Mechanism:** Agent calls `nbs-chat read --unread=<handle>` which advances her cursor. The sidecar also tracks the cursor. If both advance independently, messages can be skipped — the sidecar sees cursor=N (from agent's read), assumes agent has processed up to N, and does not re-deliver messages between old-cursor and N.

**Frequency:** Whenever an agent proactively reads chat instead of waiting for sidecar notifications.

**Evidence pattern:** Cursor jumps forward without the agent having processed all intermediate messages.

**Mitigation:** Ensure `--unread` cursor advancement and sidecar cursor advancement use the same mechanism (read-then-write with lock). Alternatively, make the sidecar the sole cursor writer and have `--unread` read the sidecar's cursor without advancing it.

### 5. Two sidecars advancing the same cursor

**Mechanism:** Duplicate sidecars (from the archive-tag bug, failed kill, or restart race) both read the same cursor, both advance it. One advances past messages the agent hasn't actually processed.

**Frequency:** When duplicate sessions exist (now mitigated by the archive-tag fix, but can still occur during restart races).

**Evidence pattern:** Agent skips messages. Cursor jumps ahead in chunks.

**Mitigation:** The sidecar should write a PID marker alongside the cursor. Before advancing, check that the PID marker matches the current sidecar's PID. If it doesn't, another sidecar is active — log a warning and exit.

### 6. Sidecar restart without cursor reset

**Mechanism:** Old sidecar dies. New sidecar starts (via `nbs-sidecar-restart`). Cursor was not reset. New sidecar reads cursor file, sees cursor=N (where N was set by the dead sidecar). If N equals the current message count, no notifications are delivered. Agent is deaf to messages that arrived while the sidecar was dead.

**Frequency:** Every sidecar restart that does not go through fixup or kick (e.g., manual `nbs-sidecar-restart`, or the `/sidecar` terminal command).

**Evidence pattern:** Agent suddenly deaf after a sidecar restart. Does not respond to messages until someone resets her cursor manually.

**Mitigation:** `nbs-sidecar-restart` should check the cursor against the current message count and reset it if needed. Alternatively, the sidecar should deliver a notification on startup regardless of cursor state, so the agent always reads recent context.

### 7. Archive drops message count

**Mechanism:** Chat file archived — first 1000 messages moved to archive file, main file now has fewer. Cursor was 1500, now there are only 500 messages. Cursor exceeds message count.

**Frequency:** When chat exceeds 2000 messages (auto-archive threshold).

**Evidence pattern:** Agent has a cursor impossibly higher than message count. Terminal shows "chat archived" banner.

**Mitigation:** Already partially handled — terminal detects message count drop and resets `g_msg_count`. The sidecar should do the same: if cursor > message count, clamp cursor to message count.

### 8. Wrong message count formula

**Mechanism:** `msg_count = $(wc -l < "$chat_file") - 6` assumes exactly 6 header lines. If the header format changes (extra fields, blank lines), the count is wrong. All cursors derived from this formula are wrong by the same offset.

**Frequency:** Persistent, affects all agents equally.

**Evidence pattern:** All agents desynced by the same constant amount.

**Mitigation:** Use `nbs-chat read <file> 2>/dev/null | wc -l` for the authoritative message count, or add a `nbs-chat count <file>` subcommand that returns the decoded message count (not line count).

### 9. Chat auto-repair changes message count

**Mechanism:** Repair blanks corrupt lines (preserving line count) but appends a recovery message (+1 to message count). Cursor was set before repair, now message count is higher by 1.

**Frequency:** After auto-repair triggers.

**Evidence pattern:** Agent gets an extra notification for the repair message. Minor — not a real desync, just a spurious notification.

**Mitigation:** No action needed. This is correct behaviour — the recovery message is a real message that agents should see.

### 10. Race between chat_send and cursor read

**Mechanism:** Message sent (count N → N+1) while sidecar reads cursor. Possible double-delivery if two sidecars read the old cursor simultaneously. No desync per se, just a timing artefact.

**Frequency:** Under high message rate.

**Evidence pattern:** Agent sees duplicate notifications. Harmless — agent reads the same message twice.

**Mitigation:** Idempotent notification handling (agent ignores duplicate notifications). Already effectively handled because `--unread` only shows messages past the cursor.

## Recommended Priority

| # | Scenario | Impact | Fix complexity | Priority |
|---|----------|--------|----------------|----------|
| 3 | Cooldown suppresses burst notifications | Regular, 3-10 messages lost | Moderate — add post-cooldown re-check | **High** |
| 2 | Concurrent sed race on cursor file | Occasional, unpredictable | Moderate — use chat lock | **High** |
| 1 | Fixup resets too aggressively | Regular, 1-2 messages lost | Easy — reset to msg_count-1 or delay | **Medium** |
| 6 | Sidecar restart without cursor reset | After manual restart | Easy — check cursor on sidecar startup | **Medium** |
| 4 | Agent + sidecar both advancing cursor | Occasional | Hard — requires cursor ownership redesign | **Low** |
| 5 | Duplicate sidecars | Rare (mostly fixed) | Moderate — PID marker | **Low** |
| 7 | Archive drops count | After archive | Easy — clamp cursor | **Low** |
| 8 | Wrong message count formula | Persistent if triggered | Easy — use nbs-chat count | **Low** |
| 9 | Auto-repair +1 | After repair | None needed | **None** |
| 10 | Send/read race | Under high load | None needed | **None** |

## What Does NOT Change

- The cursor file format (`handle=count` lines)
- The chat file format
- The sidecar's notification delivery mechanism
- Agent skill files or behaviour
