# Help When Stuck

Symptom → diagnosis → fix. Find your problem, follow the steps.

## My agent is not responding to chat messages

**Symptom:** You send messages on a chat channel but the agent never replies.

**Check 1: Is the agent running?**
Look for an active `nbs-claude` session in tmux. If the agent has crashed or been killed, restart it.

**Check 2: Is the sidecar polling?**
The sidecar injects `/nbs-poll` after 30 seconds of idle time. If the agent is busy with a long task, it will not check chat until the task completes. Wait for it to return to the prompt.

**Check 3: Is the agent watching the right chat file?**
```bash
cat .nbs/control-registry
```
The chat file should appear as `chat:<path>`. If it is missing, the agent does not know about it. Either the agent needs to register it (`echo "register-chat <path>" >> .nbs/control-inbox`) or the agent was started before the chat file was created.

**Check 4: Are you using the right handle?**
`@mentions` must match the agent's handle exactly. Check the agent's handle in its worker file or by looking at its previous chat messages.

## Events are not being processed

**Symptom:** `nbs-bus check .nbs/events/` shows pending events, but no one processes them.

**Check 1: Is any agent polling the bus?**
```bash
cat .nbs/control-registry | grep bus
```
If no agent has registered the bus directory, no one is checking it.

**Check 2: Is the agent acknowledging events?**
Events stay pending until explicitly acknowledged with `nbs-bus ack`. If the agent reads events but does not ack them, they accumulate.

**Check 3: Is deduplication dropping events?**
```bash
# Try publishing with dedup disabled
nbs-bus publish .nbs/events/ test test-event normal --dedup-window=0 "test"
```
If the event appears, deduplication may be dropping your events. Check your dedup-key — same source + same type within the window will be deduplicated.

## I cannot see messages from another agent

**Symptom:** Agent A posts to a chat channel, but Agent B never sees the messages.

**Check 1: Same chat file?**
Both agents must be reading/writing the same file path. Verify with:
```bash
# On Agent A's terminal
echo "Which chat file am I using?"
# On Agent B's terminal
echo "Which chat file am I using?"
```

**Check 2: Using --unread correctly?**
```bash
# See all messages (no filtering)
nbs-chat read .nbs/chat/live.chat

# See messages since your last read (correct for polling)
nbs-chat read .nbs/chat/live.chat --unread=my-handle
```
Do NOT use `--since=<handle>` for regular polling — it shows messages since your last *post*, not your last *read*, and posting advances the marker.

## The bus is not working

**Symptom:** `nbs-bus` commands fail or behave unexpectedly.

**Check 1: Does the directory exist?**
```bash
ls -la .nbs/events/
ls -la .nbs/events/processed/
```
Both must exist. If not:
```bash
mkdir -p .nbs/events/processed
```

**Check 2: Run the self-test**
```bash
# Publish a test event
nbs-bus publish .nbs/events/ self-test test-event low "test"

# Check it appears
nbs-bus check .nbs/events/

# Read it
nbs-bus read .nbs/events/ <event-file>

# Ack it
nbs-bus ack .nbs/events/ <event-file>

# Verify it moved
ls .nbs/events/processed/
```

If any step fails, the bus binary may not be installed correctly. Rebuild:
```bash
cd src/nbs-bus && make clean && make && cd ../..
```

**Check 3: Is the binary on your PATH?**
```bash
which nbs-bus
```
Should be in `~/.nbs/bin/` or the project's `bin/` directory. If not found, re-run `./bin/install.sh`.

## The sidecar is not injecting /nbs-poll

**Symptom:** The agent sits idle but never polls for messages or events.

**Check 1: Is polling disabled?**
```bash
echo $NBS_POLL_DISABLE
```
If set to `1`, polling is disabled. Unset it and restart `nbs-claude`.

**Check 2: Are you inside tmux?**
The sidecar requires either tmux or pty-session. If you are running Claude directly (not via `nbs-claude`), there is no sidecar.

**Check 3: Is the sidecar process alive?**
```bash
ps aux | grep poll_sidecar
```
If the sidecar crashed, restart `nbs-claude`.

**Check 4: Prompt detection**
The sidecar only injects when it sees `❯` or `>` in the last 3 lines of pane content. If Claude's prompt uses a different character, the sidecar will never inject. This is a known limitation.

## Processed events are consuming too much disc space

**Fix:**
```bash
nbs-bus prune .nbs/events/ --max-bytes=16777216
```
This deletes the oldest processed events until the `processed/` directory is under 16MB (default). Run periodically or add to your agent's poll loop.

## An agent keeps getting plan mode prompts

**Symptom:** Claude Code asks "Would you like to proceed?" repeatedly, blocking the agent.

The sidecar auto-selects option 2 ("Yes, and bypass permissions"). If this is not happening:

**Check 1: Is the sidecar running?** (See "sidecar not injecting" above)

**Check 2: Is the prompt text exactly "Would you like to proceed?"**
The sidecar matches this exact string. If Claude Code changes the prompt wording, the detection breaks.

## I want to add a new chat channel mid-session

**From the agent:**
```bash
# Create the channel
nbs-chat create .nbs/chat/new-channel.chat

# Register it so the sidecar polls it
echo "register-chat .nbs/chat/new-channel.chat" >> .nbs/control-inbox
```

**Verify:**
```bash
cat .nbs/control-registry | grep new-channel
```

## Everything is broken and I want to start fresh

```bash
# Remove the NBS state (does not affect your project files)
rm -rf .nbs/

# Recreate
mkdir -p .nbs/chat .nbs/events/processed .nbs/workers
nbs-chat create .nbs/chat/live.chat
```

Restart all `nbs-claude` sessions. Previous context is lost — the event queue, chat history, and worker files are gone. This is the nuclear option.

## See Also

- [Quick Start](quick-start.md) — Get a session running from scratch
- [nbs-claude](nbs-claude.md) — Sidecar reference
- [nbs-bus](nbs-bus.md) — Bus reference
- [Bus Recovery](nbs-bus-recovery.md) — Startup and restart protocol
