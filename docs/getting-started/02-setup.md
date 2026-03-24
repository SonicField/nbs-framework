# Chapter 2: Setup

By the end of this chapter you will have a working NBS installation and a project directory ready for your first session.

## Prerequisites

You need:

- **Claude Code** installed and working
- **GCC** (C11 support) or **Clang**
- **GNU Make**

Verify each:

```bash
claude --version
gcc --version       # or: clang --version
make --version
```

## Install the Framework

```bash
git clone https://github.com/SonicField/nbs-framework.git
cd nbs-framework
make && make install
./bin/install.sh
```

This does three things:

1. **Builds C binaries** from `src/`. Components include: `nbs-bus` (event queue), `nbs-chat` and `nbs-chat-terminal` (messaging), `nbs-sidecar` (background monitor), `nbs-workers` (worker lifecycle), `nbs-ts` (session management), `nbs-scribe-log` (decision logging), and `nbs-hub` (process enforcement). All are compiled with `-Wall -Wextra -Wshadow -Werror -std=c11`.

2. **Installs binaries** to `bin/`.

3. **Creates `~/.nbs/`** with processed command templates, symlinks to binaries, and symlinks in `~/.claude/commands/` for Claude Code slash commands.

For a custom install location:

```bash
./bin/install.sh --prefix=/path/to/location
```

Restart Claude Code after installing so it picks up the new slash commands.

## Verify the Install

Run the self-tests:

```bash
make test-unit     # 70+ unit tests across bus, chat, sidecar
```

Check the binaries are on your PATH:

```bash
which nbs-chat
which nbs-bus
which nbs-workers
which nbs-chat-terminal
which nbs-sidecar
```

Each should resolve to `~/.nbs/bin/` or the project's `bin/` directory. If `which` cannot find them, re-run `./bin/install.sh` and check that `~/.nbs/bin` is on your PATH.

## Create Your Project Directory

For the running example, create a C11 interpreter project:

```bash
mkdir -p ~/c11-interp
cd ~/c11-interp
```

## Write a Goal File

The goal file tells the team what you are building. Create `goal.md`:

```bash
cat > goal.md << 'EOF'
# Terminal Goal

Build a C11 interpreter in Python that passes the C11 conformance subset
for expressions, declarations, and control flow.

## Components

- Lexer: tokenise C11 source into a token stream
- Parser: build an AST from the token stream
- Type checker: validate types, resolve declarations, catch errors
- Evaluator: execute the AST, handle memory model, produce output

## Success Criteria

- All lexer tests pass (tokenisation of keywords, operators, literals, identifiers)
- All parser tests pass (expressions, declarations, statements, control flow)
- Type checker catches type mismatches, undeclared variables, incompatible assignments
- Evaluator correctly executes arithmetic, control flow, function calls, pointers
- Integration test: compile and run a 50-line C11 program that exercises all features
EOF
```

This file is not NBS-specific -- it is just a markdown file that describes what you want to build. In the next chapter, you will pass it to `nbs-chat-terminal` using `--goal-file`, and it will be posted to the team chat as the session's starting context.

## Bootstrap the NBS Structure

Use `nbs-chat-init` to create the `.nbs/` directory and all team infrastructure:

```bash
nbs-chat-init --name=c11-interp
```

This creates everything the team needs in one step:

```
~/c11-interp/
├── goal.md
└── .nbs/
    ├── chat/
    │   └── c11-interp.chat  # Team chat channel
    ├── events/              # Event bus queue
    ├── scribe/              # Decision logs will be written here
    ├── workers/             # Worker task files will appear here
    ├── pids/                # Agent PID tracking
    ├── commands/            # Skill files (symlinked from ~/.nbs/commands/)
    └── docs/                # Tool reference (symlinked from ~/.nbs/docs/)
```

Useful options:

| Option | Purpose |
|--------|---------|
| `--root=PATH` | Use a different project directory (default: current directory) |
| `--spawn-scribe` | Also launch the Scribe in an nbs-ts session |
| `--spawn-all` | Launch Scribe + Pythia + main Claude instance |
| `--dry-run` | Print what would be created without doing it |

To restart after a reboot (re-spawn agents without re-creating infrastructure):

```bash
nbs-chat-init --name=c11-interp --spawn-only --spawn-all
```

You can also run `/nbs-teams-start` inside Claude Code, which does the same thing interactively and asks you for your terminal goal.

## What You Have Now

- NBS framework installed and verified
- A project directory with `goal.md` defining your terminal goal
- An `.nbs/` structure with chat, events, scribe, and workers directories
- A chat channel (`c11-interp.chat`) ready for team communication

## Next

[Chapter 3: Running Your First Session](03-first-session.md) -- Launch the team, watch them work, shut it down.
