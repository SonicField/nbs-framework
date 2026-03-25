# nbs-ts: An Alternative to tmux for Claude Automation

*Dr Alex Turner and Claude Opus 4.6 — 25 March 2026*

---

## TL;DR

tmux was not designed for AI automation. It was designed for humans who want persistent terminal sessions. When you use it to manage AI agent processes, you inherit an interface built around keystrokes, pane geometry, and visual display — none of which your automation cares about. Every interaction goes through `send-keys` (simulating typing), `capture-pane` (scraping pixels), and `pipe-pane` (hoping the output lands). These are heuristic. They race. They lose data. nbs-ts replaces this with a purpose-built C library: `create` gives you a PTY with an append-only output log and a structured completion signal. `send` writes bytes to the master fd. `read` and `read-tail` pull from the log file using `pread`. `wait-complete` watches a completion log with inotify — no polling, no scraping, no guessing. A helper daemon (`nbs-ts-helper`) centralises PTY allocation via Unix socket with SCM_RIGHTS fd passing and SO_PEERCRED authentication. The result is deterministic, race-free process management for AI agents. We built it in 4.5 hours with a team of 8 AI agents. It replaced approximately 800 lines of tmux shell scripting with 1,600 lines of C that actually work.

---

## The Problem with tmux

tmux is a terminal multiplexer. It manages windows, panes, sessions, and status bars. It handles mouse events, copy mode, key bindings, and layout algorithms. It renders text to a grid and lets humans switch between virtual terminals.

None of this is what you want when launching an AI agent.

What you actually want is simple: run a process in a PTY, capture its output reliably, send it input, know when commands finish, and check whether it is alive. Five operations. tmux provides all five — buried under an interface designed for interactive use, exposed through a command-line client that communicates with a server process via a Unix socket.

The mismatch produces three categories of failure.

### Heuristic I/O

`tmux send-keys` simulates typing. It sends keystrokes to the terminal, character by character, as though a human pressed them. This means your automation must construct strings that look like terminal input, including handling special characters, escapes, and newlines. Multi-line input requires careful quoting or bracketed paste mode. There is no return value. You send the keys and hope.

`tmux capture-pane` reads the current visual content of a pane — a rectangular grid of characters. This is a snapshot of what a human would see, complete with ANSI colour codes, line wrapping artefacts, and whatever the application last drew to the screen. It is not a log. It is not append-only. If content scrolls past the visible area and you did not set up scrollback, it is gone.

`tmux pipe-pane` can redirect pane output to a file, but it captures the raw PTY stream — escape sequences, cursor movements, screen clears, and all. Parsing this reliably requires an ANSI stripping layer, and even then you are working with a stream that was never designed to be machine-readable.

### Race Conditions

tmux's client-server architecture introduces timing between command and effect. `send-keys` returns before the target process has processed the input. `capture-pane` returns whatever happened to be on screen at the instant you called it. Between sending a command and capturing its output, an arbitrary amount of time may pass, and other output may have arrived, and the pane may have scrolled.

The standard workaround is `sleep`. Sleep 0.5 seconds after sending keys, then capture. Sleep 1 second, then check if the prompt is visible. Sleep 2 seconds, then assume the command finished. Every one of these is a race condition pretending to be a solution. On a fast machine, the sleep is wasted time. On a loaded machine, the sleep is not long enough. Both failure modes are silent.

### Session Management Overhead

tmux sessions accumulate. An agent that spawns sub-agents, each in its own tmux session, generates a tree of sessions that must be tracked, listed, cleaned up, and prevented from colliding. `tmux has-session` checks existence. `tmux kill-session` removes them. `tmux list-sessions` shows the state. Each of these is a subprocess invocation, a round-trip to the tmux server, and a parse of text output.

Over 28 days of running AI agent teams with tmux, we accumulated 25 orphaned Pythia/Shepard/fixup sessions from crashed or restarted agents. The monitoring code to detect and clean these up was more complex than the code that created them.

## What nbs-ts Does Instead

nbs-ts is a C library and CLI for managing command sessions in pseudo-terminals. It does exactly five things.

### 1. Create a Session

```c
nbs_ts_session_t *s = nbs_ts_create("bash", NULL);
```

This allocates a PTY pair, forks a child process connected to the slave end, and starts a capture thread that reads the master fd and appends to an output log. The session gets a random 8-character hex handle and a directory under `~/.nbs-ts/sessions/`:

```
~/.nbs-ts/sessions/a3f7c2d1/
  output.log        — append-only stdout+stderr capture
  completion.log    — structured completion records
  pid               — child process ID
  meta              — command, start time
```

The output log is append-only and fsynced after every write. It never truncates, never scrolls, never loses data. It is a file. You can `cat` it, `tail -f` it, `grep` it, or `pread` it from any offset.

### 2. Send Input

```c
int rc = nbs_ts_send(s, "echo hello\n", 11);
```

Direct `write(2)` to the PTY master fd. No keystroke simulation. No escaping. No send-keys indirection. The bytes arrive at the child's stdin exactly as written. The return value tells you whether the write succeeded. Simple.

### 3. Read Output

```c
size_t n = nbs_ts_read_new(s, buf, sizeof(buf));
```

Reads from the output log starting at the session's read cursor, advances the cursor, returns the byte count. Because the log is append-only, `pread` from any offset is always safe — there is no window where the file is being truncated or rewritten. `read_tail` provides viewport semantics: the last N lines, read backwards from the end of the file.

No screen scraping. No capture-pane. No ANSI escape sequences to strip (though a strip mode exists in the CLI for display purposes). The output is what the process wrote, in order, complete.

### 4. Wait for Completion

```c
nbs_ts_completion_t out;
int rc = nbs_ts_wait_complete(s, 30000, &out);
// out.seq = 1, out.exit_code = 0
```

This is the feature tmux cannot provide at all. On session creation, nbs-ts injects a `PROMPT_COMMAND` into bash that appends a sequence number and exit code to the completion log after every command. `wait_complete` watches this file with `inotify(7)` and blocks until a new record appears or the timeout expires.

No polling. No sleep-and-check. No grepping for shell prompts. The completion signal is structured data — a sequence number and an exit code — written atomically by bash itself. When `wait_complete` returns, you know which command finished and whether it succeeded.

### 5. Check Status

```c
nbs_ts_status_t st = nbs_ts_status(s);
// NBS_TS_ALIVE, NBS_TS_DEAD, or NBS_TS_UNKNOWN
```

Non-blocking `waitpid` with `WNOHANG`. If the child has exited, the exit code is captured and the status transitions to `DEAD`. No tmux server query. No parsing text output. A function call.

## The Helper Daemon

Claude Code expects to be launched from a human terminal. It reads stdin, negotiates trust with the workspace, and manages its own session lifecycle. Launching it from a daemon, a cron job, or a C binary's `fork()` + `exec()` fails in subtle, reproducible, undocumented ways — Claude starts, processes a few API turns, then exits silently.

The nbs-ts-helper daemon solves this. It runs in the user's terminal context (where SSH keys, proxy credentials, and login environment are available) and listens on a Unix socket at `~/.nbs-ts/helper.sock`. When a client connects:

1. Peer credentials are verified via `SO_PEERCRED` — only the same UID can request a PTY.
2. The command string is read from the socket.
3. `openpty` + `fork` + `exec` happens in the helper's process context.
4. The PTY master fd is sent back to the caller via `SCM_RIGHTS`.
5. The child PID follows as plain text.

The caller now holds a file descriptor to the PTY master. It can read, write, and poll this fd as though it had created the PTY itself. But the child process was forked from the helper — which means it inherits the helper's environment, not the caller's. The helper runs in a real terminal. The child gets a real PTY. Claude Code cannot tell the difference between this and a human typing a command.

This is the key architectural insight. The problem was never "how do we create a PTY" — `openpty` is one function call. The problem was "how do we create a PTY in the right process context." The helper separates the decision to launch (which can come from anywhere — a C binary, a bash script, a cron job) from the act of launching (which must happen in a context that looks like a human terminal).

## What We Tried That Did Not Work

Every simpler approach was attempted before building nbs-ts. The failure table is instructive.

| Approach | Failure |
|----------|---------|
| C `fork()` + `setsid()` + `execl()` | Claude exits after a few API calls. Reproducible, cause unknown. |
| Wrapping nbs-claude inside `nbs-ts create` | Double session — nbs-claude creates its own PTY, producing a nested session with a broken sidecar. |
| Redirecting stdin to `/dev/null` | Claude exits immediately. It needs stdin open. |
| `flock` on PID files | File descriptor inherited by child processes, blocking subsequent spawns indefinitely. |
| Sleep-based coordination | Race conditions. The system must work without timing assumptions. |
| Inheriting parent environment | `CLAUDECODE=1` leaks into child agents, triggering nesting detection and 30-second death. |

The final solution is a bash function (`launch_agent`) that unsets contaminating environment variables, calls `setsid`, and backgrounds the process. The function is three lines of substance. Everything else was tried first and failed.

## Why C

This is addressed at length in a companion paper, but the short version: nbs-ts is 1,600 lines of C with assertions at every entry point, every allocation, and every state transition. The test suite covers session creation, I/O round-trips, read cursor idempotency, status transitions, and cleanup. AddressSanitizer and Valgrind run against every build.

C was chosen because the system it replaced — tmux interaction via bash — was opaque. `tmux send-keys` hides what happens between your command and the target process. `tmux capture-pane` hides what was lost to scrolling. The bash scripts that orchestrated tmux sessions hid their failure modes behind silent exit codes and swallowed stderr.

C hides nothing. `write(2)` to a file descriptor either succeeds or it does not. `pread(2)` from an append-only file is always safe. `waitpid(2)` with `WNOHANG` returns the process state without blocking. Every system call has a defined error path. Every error path is handled. The code is transparent because the language is transparent.

## The Migration

The tmux removal was a 6-phase operation tracked by a 200-line migration checklist. 64 files referenced tmux or the previous session manager. The sidecar got a new transport backend. The agent launcher switched from tmux session creation to nbs-ts. The remote tools (run, session, build, diff, edit, read) replaced their PTY interaction layer. The restart and watchdog scripts were rewritten. Then tmux was removed entirely — transport code, binary wrappers, lock files, and all associated tests.

The verification gate: a grep across the entire codebase for tmux-related terms. Zero matches means migration is complete. Any surviving reference means it is not. Binary, falsifiable, no judgement calls.

## What This Gets You

The compound effect of replacing heuristic I/O with deterministic I/O is not one improvement — it is the elimination of an entire category of bugs.

The phantom notification storms — where the sidecar counted 257 unread messages because `capture-pane` returned stale content — disappear. The correlated overnight zombie pattern — where all agents drifted to 11% context because they kept processing empty notification cycles — disappears. The orphaned session accumulation — where crashed agents left tmux sessions that no monitoring code cleaned up — disappears.

These are not hypothetical. Each one was a real failure that consumed real debugging time over the 28-day project. Each one was caused by the gap between what tmux provides (a visual terminal multiplexer) and what the automation needed (reliable programmatic process management).

nbs-ts is not a better tmux. It is a different tool for a different job. tmux is for humans who want to switch between terminals. nbs-ts is for machines that want to run processes and know what happened. The distinction matters because conflating them is what produced the bugs in the first place.

## The Falsifier

This argument fails if tmux-based automation can achieve the same reliability as nbs-ts with less total complexity. If someone demonstrates a tmux configuration — perhaps using `pipe-pane` with proper ANSI stripping, inotify-based completion detection, and atomic session tracking — that provides deterministic I/O without the failure modes described above, then the case for a custom tool collapses. The complexity of building and maintaining 1,600 lines of C would be unjustified.

We looked. We did not find it. But the absence of evidence is not evidence of absence, and anyone who has solved these problems within tmux should say so.
