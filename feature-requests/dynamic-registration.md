# Dynamic Resource Registration

**Requested by:** Alex Turner
**Date:** 13 February 2026
**Priority:** High (closes a fundamental architecture gap)

## The Gap

When nbs-claude starts, it discovers resources by scanning `.nbs/chat/*.chat` and `.nbs/workers/*.md`. If a new chat file or hub is created _after_ startup, nbs-claude does not know about it. The agent cannot react to events in resources it has not discovered.

This gap is particularly acute for:
- Remote agents joining over nbs-chat-remote after the wrapper is running
- Hub memberships established mid-session
- Dynamically created chat channels for sub-teams

A static configuration file does not solve this because the AI must be able to declare new resources at any time, including resources it learns about from other agents via chat.

## Design Principle: AI-First

Alex's framing: "The beauty of working with AI is that the AI can set up and coordinate things — so that is the way the system must be designed: AI-first."

The AI should tell its wrapper what it needs, not the other way around. The wrapper should be a dumb executor of AI-declared intent.

## Proposal: `\nbs-` Control Commands

### Convention

- `/` prefix: human-to-AI commands (e.g. `/nbs-poll`, `/search`)
- `\` prefix: AI-to-wrapper commands (e.g. `\nbs-register-chat`)

The AI outputs a `\nbs-` line to its terminal. The nbs-claude sidecar (which already monitors the pane content) detects it and acts on it. The command is consumed by the wrapper and does not appear as a user-visible action.

### Commands

| Command | Purpose |
|---------|---------|
| `\nbs-register-chat <path>` | Add a chat file to the poll watch list |
| `\nbs-unregister-chat <path>` | Remove a chat file from the poll watch list |
| `\nbs-register-bus <events-dir>` | Add an events directory to the bus check list |
| `\nbs-unregister-bus <events-dir>` | Remove an events directory |
| `\nbs-register-hub <path>` | Add a hub configuration to watch |
| `\nbs-unregister-hub <path>` | Remove a hub from watch |
| `\nbs-set-poll-interval <seconds>` | Change the poll interval dynamically |

### Example Flow

```
# AI learns about a new chat from another agent
claude> I see bench-claude created a new chat at .nbs/chat/debug.chat
claude> \nbs-register-chat .nbs/chat/debug.chat

# nbs-claude sidecar detects the \nbs-register-chat line
# Adds .nbs/chat/debug.chat to the poll watch list
# Next /nbs-poll cycle includes debug.chat
```

## Design Speculation: nbs-claude Wrapper Changes

### Current Architecture

```
nbs-claude
├── poll_sidecar_tmux() — monitors pane for idle, injects /nbs-poll
├── detect_plan_mode() — auto-selects option 2 when blocked
└── cleanup() — kills sidecar on exit
```

The sidecar already captures pane content every second via `tmux capture-pane`. It hashes the content to detect changes and checks for plan mode prompts. Adding `\nbs-` detection is a natural extension of this content-monitoring loop.

### Proposed Changes

#### 1. Resource Registry (in-memory)

The sidecar maintains a set of registered resources. On startup, it seeds this from `.nbs/chat/*.chat` and `.nbs/events/` (if present). The AI can modify it at runtime via `\nbs-` commands.

```bash
# State file for registered resources (one per line, type:path)
REGISTRY_FILE="/tmp/nbs-claude-$$.registry"

# Seed on startup
for chat in .nbs/chat/*.chat; do
    echo "chat:$chat" >> "$REGISTRY_FILE"
done
if [[ -d .nbs/events ]]; then
    echo "bus:.nbs/events" >> "$REGISTRY_FILE"
fi
```

#### 2. Control Command Detection

Add a function to the sidecar loop that scans captured content for `\nbs-` lines. This runs on every content change (not just idle), because registration should happen immediately.

```bash
detect_control_commands() {
    local content="$1"

    # Extract \nbs- lines from the captured pane content
    # Match lines starting with \nbs- (the AI outputs these literally)
    local commands
    commands=$(echo "$content" | grep -oE '\\nbs-[a-z-]+ [^ ]+')

    while IFS= read -r cmd; do
        [[ -z "$cmd" ]] && continue

        local verb path
        verb=$(echo "$cmd" | awk '{print $1}')
        path=$(echo "$cmd" | awk '{print $2}')

        case "$verb" in
            '\nbs-register-chat')
                if ! grep -q "^chat:${path}$" "$REGISTRY_FILE" 2>/dev/null; then
                    echo "chat:${path}" >> "$REGISTRY_FILE"
                    echo "[nbs-claude] Registered chat: $path" >&2
                fi
                ;;
            '\nbs-unregister-chat')
                sed -i "\|^chat:${path}$|d" "$REGISTRY_FILE"
                echo "[nbs-claude] Unregistered chat: $path" >&2
                ;;
            '\nbs-register-bus')
                if ! grep -q "^bus:${path}$" "$REGISTRY_FILE" 2>/dev/null; then
                    echo "bus:${path}" >> "$REGISTRY_FILE"
                    echo "[nbs-claude] Registered bus: $path" >&2
                fi
                ;;
            '\nbs-register-hub')
                if ! grep -q "^hub:${path}$" "$REGISTRY_FILE" 2>/dev/null; then
                    echo "hub:${path}" >> "$REGISTRY_FILE"
                    echo "[nbs-claude] Registered hub: $path" >&2
                fi
                ;;
            '\nbs-set-poll-interval')
                POLL_INTERVAL="$path"
                echo "[nbs-claude] Poll interval set to ${path}s" >&2
                ;;
        esac
    done <<< "$commands"
}
```

#### 3. Integration into the Sidecar Loop

The `poll_sidecar_tmux()` function currently has this structure:

```
loop:
  capture pane content
  hash content
  if changed:
    check plan mode → auto-select
  if idle for N seconds:
    inject /nbs-poll
```

With dynamic registration:

```
loop:
  capture pane content
  hash content
  if changed:
    check plan mode → auto-select
    detect_control_commands(content)    # NEW
  if idle for N seconds:
    inject /nbs-poll
```

#### 4. Passing Registry to /nbs-poll

The /nbs-poll skill currently hardcodes `ls .nbs/chat/*.chat`. With dynamic registration, it should check the registered resources instead. Two approaches:

**Option A: Environment variable.** The sidecar exports `NBS_REGISTERED_CHATS` before injecting /nbs-poll. The poll skill reads this. Problem: Claude Code does not inherit environment variables from the sidecar.

**Option B: Registry file.** The sidecar writes the registry to a known path (e.g. `.nbs/poll-registry`). The /nbs-poll skill reads this file if it exists, falls back to scanning .nbs/chat/*.chat otherwise. This is the simpler approach and maintains backward compatibility.

**Option C: AI self-knowledge.** The AI already knows which chats it registered (it issued the commands). The /nbs-poll skill could be updated to say "check the chats you have registered" and the AI maintains its own list in context. This requires no wrapper changes for the poll itself — only the registration detection. This fits the AI-first principle best.

### Edge Cases

1. **Duplicate detection.** The sidecar may see the same `\nbs-register-chat` line on multiple captures (pane content persists for several seconds). The registry check (`grep -q`) prevents duplicates.

2. **Content scrolling.** `tmux capture-pane -S -5` only captures the last 5 lines of scrollback. If the `\nbs-` command scrolls off before the sidecar's next 1-second check, it will be missed. Mitigation: increase scrollback to `-S -20` or use `tmux capture-pane -S -` for full history with a "processed commands" set to avoid re-processing.

3. **Backslash in output.** The `\nbs-` prefix must not collide with normal output. `\nbs-register-chat` is specific enough — Claude would not produce this accidentally in conversation. If paranoia is warranted, use a more explicit delimiter like `##NBS:register-chat path##`.

4. **Remote agents.** An AI on a remote machine over nbs-chat-remote outputs `\nbs-register-chat` to its local terminal. Its local nbs-claude sidecar picks it up. The remote agent does not need to know about the registration mechanism — it just outputs the command.

5. **Persistence across restarts.** The registry file is per-session (keyed by PID). On restart, the AI re-discovers resources and re-registers them. This is deliberate: stale registrations from a previous session should not persist.

## Addendum: Control File Design (from chat discussion)

### Forward-Only Control Files

Alex's directive: "All control files must be forward only. Truncation can lead to chaos AND it is lovely to have the complete record for lesson learning."

The control inbox (`.nbs/control-inbox`) is append-only. The sidecar tracks a read offset (line count) and only processes new lines. The full history is preserved for audit and post-session analysis. No truncation, no deletion.

```bash
# Sidecar reads new lines since last check
CONTROL_INBOX=".nbs/control-inbox"
LAST_LINE=0

check_control_inbox() {
    [[ ! -f "$CONTROL_INBOX" ]] && return
    local total_lines
    total_lines=$(wc -l < "$CONTROL_INBOX")
    if [[ $total_lines -gt $LAST_LINE ]]; then
        tail -n +"$((LAST_LINE + 1))" "$CONTROL_INBOX" | while IFS= read -r line; do
            process_control_command "$line"
        done
        LAST_LINE=$total_lines
    fi
}
```

### Primary vs Fallback Detection

bench-claude's insight: decouple control command detection from pane scrollback. The pane scanning has a race condition — commands can scroll off before the sidecar checks.

- **Primary:** Control inbox file (`.nbs/control-inbox`). The AI writes commands here directly. Reliable, no race condition.
- **Fallback:** Pane content scanning. Catches commands the AI outputs but forgets to write to the inbox. Less reliable but provides defence in depth.

### Ack-Required Nagging

Events that arrive while the AI is busy can be ignored. The sidecar monitors unacked events and nags the AI with escalating urgency:

| Priority | Nag interval | Behaviour |
|----------|-------------|-----------|
| critical | 30 seconds | Nag until acked. AI cannot proceed without addressing this. |
| high | 2 minutes | Nag between tool calls. Important but not blocking. |
| normal | Next poll cycle (5 min) | Included in regular poll summary. |
| low | Next poll cycle | Included if queue is otherwise empty. |

**Context-aware nagging** (bench-claude's refinement): the sidecar only nags during natural pauses (pane content stable for >5 seconds). If tool output is streaming (content changing rapidly), hold off. This prevents interrupting active code generation or test runs.

### Safety Net Poll

The event bus is the primary notification mechanism. The poll is the safety net:

- Bus handles timely notification for events that arrive while the AI is idle
- Poll runs every 5 minutes (up from 30 seconds) to catch anything the bus missed
- Poll catches: events from unregistered resources, bus failures, events that arrived mid-task

Defence in depth: the bus makes things fast, the poll makes them reliable.

## Relationship to Other Features

- **Bus events (nbs-bus):** The bus provides the notification channel. Dynamic registration tells the wrapper _which_ bus directories to check.
- **nbs-poll rewrite:** The poll skill becomes bus-aware and registry-aware. Bus check first, then registered chats, then registered workers.
- **Agent configuration (.nbs/agent.yaml):** A static config file could seed the initial registry, but the dynamic registration mechanism is the authoritative runtime source.

## Falsification

The feature works if and only if:
1. An AI outputting `\nbs-register-chat .nbs/chat/new.chat` causes nbs-poll to check that chat on its next cycle
2. Resources created after nbs-claude startup are discovered without restart
3. Resources registered by one AI (via chat message to another) are picked up by the receiving AI
4. The `\nbs-` command is not accidentally triggered by normal AI output
5. Unregistering a resource stops it from being polled
