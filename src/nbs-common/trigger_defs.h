/*
 * trigger_defs.h — Shared trigger role names and task descriptions
 *
 * Used by:
 *   - nbs-sidecar/triggers.c (automatic periodic spawning)
 *   - nbs-chat/terminal.c (manual /pythia, /shepard, etc. commands)
 *
 * Single source of truth for what each ephemeral worker does.
 */

#ifndef NBS_TRIGGER_DEFS_H
#define NBS_TRIGGER_DEFS_H

#define TRIGGER_ROLE_PYTHIA    "pythia"
#define TRIGGER_ROLE_SHEPARD   "shepard"
#define TRIGGER_ROLE_FIXUP     "fixup"
#define TRIGGER_ROLE_LIBRARIAN "librarian"

#define TRIGGER_DESC_PYTHIA \
    "Load /nbs-pythia. Read the last 500 lines of .nbs/scribe/live-log.md " \
    "(do NOT read the full file). Run the checkpoint procedure. " \
    "Post assessment to chat. Then stop — do not do anything else."

#define TRIGGER_DESC_SHEPARD \
    "Load /nbs-shepard. Check agent liveness by listing tmux sessions " \
    "and capturing panes. Read the last 20 chat messages directly " \
    "(do NOT launch sub-agents). Post a brief assessment to chat. " \
    "Then stop — do not do anything else after posting."

#define TRIGGER_DESC_FIXUP \
    "Load /nbs-fixup-auto. Run /nbs-teams-fixup on all agents. " \
    "Post summary to chat. Then stop — do not do anything else after posting."

#define TRIGGER_DESC_LIBRARIAN \
    "Load /nbs-librarian. Read last 100 chat messages via nbs-chat read. " \
    "Search scribe log for answers to questions or blockers the team is " \
    "stuck on. Post findings with @team! tag. If scribe has nothing " \
    "relevant, stay silent. Then stop — do not do anything else."

#endif /* NBS_TRIGGER_DEFS_H */
