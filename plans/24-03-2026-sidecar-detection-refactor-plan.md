# Plan: Sidecar Detection Refactor

## Problem

`detect_prompt_visible` is a single function serving five callers with different requirements. Every hardening change for one caller breaks another. This session alone produced four regressions from detection changes:

| Change | Intended for | Broke |
|--------|-------------|-------|
| Added "bypass" check | Init-wait (avoid trust dialog) | All notification injection |
| Capture window 5→30 lines | Notification (prompt too far back) | Nothing (but was reverted then re-applied twice) |
| Removed "bypass" check | Fix notification injection | Init-wait (trust dialog false positive returns) |
| Added "What should Claude do" | Interrupt handler | Nothing yet — but increases false positive surface |

Root cause: detection and action logic are tangled. One function, five callers, three different questions.

## Current state

### Callers of detect_prompt_visible

| Location | Purpose | What it actually needs to know |
|----------|---------|-------------------------------|
| Init-wait (sidecar.c ~640) | Inject initial prompt | Is Claude at `❯` AND NOT showing the trust dialog? |
| Notification injection (sidecar.c ~880) | Inject `[NBS-CHAT-NOTIFICATION]` | Is Claude idle at `❯`? (trust dialog irrelevant — long past startup) |
| TOCTOU re-check (sidecar.c ~890) | Confirm prompt still visible before injecting | Same as notification — is `❯` still there? |
| Injection verification (sidecar.c ~923) | Check if injected text was consumed | Is `❯` back after injection? |
| Interrupt handler (sidecar.c ~135) | Inject interrupt message after Escape | Is Claude at `❯` OR at the interrupted prompt ("What should Claude do")? |
| Query handler (sidecar.c ~667) | Capture and post output | **Nothing** — query doesn't interact with the agent |

### The three actual questions

1. **Is Claude idle at its prompt?** — `❯` visible. Used by notification injection and its TOCTOU/verification checks. Trust dialog doesn't matter here (only appears once at startup, long before notifications fire).

2. **Is Claude ready for input after Escape?** — `❯` visible OR "What should Claude do" visible. Used only by the interrupt handler.

3. **Is Claude at its prompt AND NOT in the trust dialog?** — `❯` visible AND "trust this folder" NOT visible. Used only by init-wait.

## Design

### New functions in detect.c

```c
/* Is Claude idle at its prompt? Checks for ❯ anywhere in content. */
int detect_prompt_idle(const char *content);

/* Is Claude ready for input after interruption?
 * Checks for ❯ OR "What should Claude do" in content. */
int detect_prompt_ready(const char *content);

/* Is Claude at its real prompt (not the trust dialog)?
 * Checks for ❯ AND absence of "trust this folder". */
int detect_prompt_not_trust(const char *content);
```

Each function:
- Takes the full captured content (caller controls window size)
- Searches the entire content (no 6-line tail restriction — already removed)
- Returns 0 or 1
- Has a postcondition assert on the return value
- Has a precondition assert on content != NULL

### Caller changes in sidecar.c

| Caller | Currently calls | Should call |
|--------|----------------|-------------|
| Init-wait | `detect_prompt_visible` + trust dialog strstr | `detect_prompt_not_trust` |
| Notification injection | `detect_prompt_visible` | `detect_prompt_idle` |
| TOCTOU re-check | `detect_prompt_visible` | `detect_prompt_idle` |
| Injection verification | `detect_prompt_visible` | `detect_prompt_idle` |
| Interrupt handler | `detect_prompt_visible` | `detect_prompt_ready` |
| Query handler | nothing (correct) | nothing (no change) |

### Delete

`detect_prompt_visible` is removed entirely. No callers remain after refactor.

## Capture window

All callers use `tp->capture(tp, 30)`. This is correct and unchanged. The capture window is the caller's responsibility, not the detection function's.

## Falsification

| Criterion | Test |
|-----------|------|
| Init-wait doesn't fire on trust dialog | Start agent in untrusted dir, verify sidecar log shows no init prompt injection until trust dialog is dismissed |
| Notification works when agent is idle | Post a chat message, verify `[NBS-CHAT-NOTIFICATION]` appears in agent output within 5 seconds |
| Notification works when agent returns to prompt after work | Agent finishes a tool call, returns to `❯`, verify notification fires |
| Interrupt works after Escape | Send `@name!`, verify interrupt message appears in agent output |
| Interrupt works when "What should Claude do" is shown | Agent is interrupted, verify message injection into the interrupted prompt |
| Query works regardless of agent state | Agent mid-work, send `@name?`, verify response posted to chat |
| No function called by wrong caller | grep codebase: `detect_prompt_idle` only in notification/verification paths, `detect_prompt_ready` only in interrupt handler, `detect_prompt_not_trust` only in init-wait |

## Files to modify

1. `src/nbs-sidecar/detect.h` — declare three new functions, remove `detect_prompt_visible`
2. `src/nbs-sidecar/detect.c` — implement three new functions, remove `detect_prompt_visible`
3. `src/nbs-sidecar/sidecar.c` — replace all `detect_prompt_visible` calls with the appropriate function
4. `src/nbs-sidecar/Makefile` — no change (same files)

## Implementation order

1. Write the three new functions in detect.c alongside `detect_prompt_visible` (don't delete yet)
2. Replace callers one at a time
3. Verify build compiles
4. Delete `detect_prompt_visible` and its declaration
5. Verify build still compiles
6. Run existing tests

## Effort

Small. Three simple functions (5-10 lines each), five call site changes (one-line each), one deletion. The logic already exists — it's being disentangled, not rewritten.
