/*
 * trigger_defs.h — Shared trigger role names, skill files, and task descriptions
 *
 * Used by:
 *   - nbs-sidecar/triggers.c (automatic periodic spawning)
 *   - nbs-chat/terminal.c (manual /pythia, /shepard, etc. commands)
 *
 * Single source of truth for what each ephemeral worker does.
 *
 * The skill file content is embedded directly in the task description
 * at spawn time (by nbs-workers or by the caller). This avoids the
 * worker needing to load a slash command, which fails in non-interactive
 * tmux contexts where the Enter key for Skill tool calls doesn't register.
 */

#ifndef NBS_TRIGGER_DEFS_H
#define NBS_TRIGGER_DEFS_H

#define TRIGGER_ROLE_PYTHIA    "pythia"
#define TRIGGER_ROLE_SHEPARD   "shepard"
#define TRIGGER_ROLE_FIXUP     "fixup"
#define TRIGGER_ROLE_LIBRARIAN "librarian"

/* Skill files under {{NBS_ROOT}}/commands/ (processed templates).
 * Relative to the .nbs/ directory — callers prepend the nbs_root. */
#define TRIGGER_SKILL_PYTHIA    "commands/nbs-pythia.md"
#define TRIGGER_SKILL_SHEPARD   "commands/nbs-shepard.md"
#define TRIGGER_SKILL_FIXUP     "commands/nbs-fixup-auto.md"
#define TRIGGER_SKILL_LIBRARIAN "commands/nbs-librarian.md"

/* Task instructions appended AFTER the embedded skill content.
 * These do NOT say "Load /nbs-X" — the skill is already inline. */
#define TRIGGER_DESC_PYTHIA \
    "Your handle is 'pythia' — use this for all nbs-chat send commands. " \
    "Read the last 500 lines of .nbs/scribe/live-log.md " \
    "(do NOT read the full file). Run the checkpoint procedure. " \
    "Post assessment to chat as pythia. Then stop — do not do anything else."

#define TRIGGER_DESC_SHEPARD \
    "Your handle is 'shepard' — use this for all nbs-chat send commands. " \
    "Check agent liveness by listing tmux sessions " \
    "and capturing panes. Read the last 20 chat messages directly " \
    "(do NOT launch sub-agents). Post a brief assessment to chat as shepard. " \
    "Then stop — do not do anything else after posting."

#define TRIGGER_DESC_FIXUP \
    "Your handle is 'fixup' — use this for all nbs-chat send commands. " \
    "Run /nbs-teams-fixup on all agents. " \
    "Post summary to chat as fixup. Then stop — do not do anything else after posting."

#define TRIGGER_DESC_LIBRARIAN \
    "Your handle is 'librarian' — use this for all nbs-chat send commands. " \
    "Read last 100 chat messages via nbs-chat read. " \
    "Search scribe log for answers to questions or blockers the team is " \
    "stuck on. Post findings with @team! tag as librarian. If scribe has nothing " \
    "relevant, stay silent. Then stop — do not do anything else."

#endif /* NBS_TRIGGER_DEFS_H */
