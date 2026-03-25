# The Skill System

Skills are slash commands for Claude Code. Type `/nbs` or `/nbs-audit` in a Claude Code session and the skill's instructions are injected into the conversation. The agent then follows them.

## Where They Live

Skills are Markdown files in `~/.claude/commands/`. Claude Code discovers them automatically. Each file becomes a command named after its filename: `nbs-audit.md` becomes `/nbs-audit`.

## How They Get There

The framework's source skills live in `claude_tools/` at the repository root. Running `bin/install.sh` does three things:

1. **Template expansion.** Each `.md` file contains `{{NBS_ROOT}}` placeholders pointing to framework paths. The installer expands these to the actual install prefix (default `~/.nbs`), writing processed files to `~/.nbs/commands/`.

2. **Symlink creation.** Each processed file in `~/.nbs/commands/` gets a symlink in `~/.claude/commands/`, which is where Claude Code looks.

3. **Stale cleanup.** Renamed or removed skills are deleted from both locations.

The chain: `claude_tools/*.md` -> `~/.nbs/commands/*.md` -> `~/.claude/commands/*.md` (symlinks).

## Frontmatter

Every skill starts with YAML frontmatter:

```yaml
---
description: One-line summary shown in Claude Code's command list
allowed-tools: Read, Glob, Grep, Bash(git log:*), AskUserQuestion
---
```

`description` is what the human sees when browsing commands. `allowed-tools` constrains which tools the agent may use while executing the skill. This is a security boundary -- an audit skill should not need `Write`, and a fixup skill should not need `AskUserQuestion`.

Bash permissions use glob patterns: `Bash(git:*)` allows any git subcommand, `Bash(python:*)` allows running Python. `Bash` alone allows everything.

## Two Kinds of Skill

**Epistemic skills** shape how the agent thinks. They impose methodology -- falsification discipline, review structure, investigation protocols. Examples:

| Skill | Purpose |
|-------|---------|
| `/nbs` | Full NBS review: goals, falsifiability, bullshit detection |
| `/nbs-audit` | Codebase audit against engineering standards |
| `/nbs-investigation` | Hypothesis-driven exploration with falsification criteria |

These skills are heavy on reading (concepts, pillars, project state) and light on writing. Their `allowed-tools` reflect this.

**Operational skills** make things happen. They spawn agents, repair teams, bootstrap projects. Examples:

| Skill | Purpose |
|-------|---------|
| `/nbs-teams-start` | Bootstrap a multi-agent team from nothing |
| `/nbs-fixup-auto` | Diagnose and restart dead or stalled agents |
| `/nbs-teams-restart` | Kill and respawn the entire team |

These need `Bash` and `Write`. They follow rigid procedures -- step sequences, verification checks, escalation levels.

## Creating a New Skill

1. Create `claude_tools/your-skill.md` with frontmatter.
2. Write the instructions the agent should follow. Be explicit. Agents do what you write, not what you mean.
3. Use `{{NBS_ROOT}}` for any path that references the framework install directory.
4. Run `bin/install.sh` to process templates and create symlinks.
5. Restart Claude Code. The new `/your-skill` command appears.

Constrain `allowed-tools` to the minimum the skill needs. A skill that only reads and reports should not have `Write`. This is not bureaucracy -- it is a falsifiable boundary. If the skill works without `Write`, you know it cannot silently modify files.
