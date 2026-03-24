# NBS Teams Getting Started Guide

A guide to running multi-agent AI projects with the NBS framework.

## Audience

You have Claude Code installed. You are comfortable with the terminal. You want to run projects where multiple AI agents collaborate on real work -- writing code, running tests, making architectural decisions -- with a human in the loop.

## How to Read This Guide

Chapters 1-3 get you running. Read them in order. By the end of Chapter 3 you will have launched a session and watched agents complete a task.

Chapters 4-6 explain how things work. Read them when you want to understand what you saw in Chapter 3, or when something does not behave as expected.

Chapter 7 is a reference. Go there when something breaks.

## Running Example

Every chapter uses the same project: a C11 interpreter written in Python. The interpreter has four components -- lexer, parser, type checker, evaluator -- that map naturally to concurrent work streams. When the guide says "spawn a worker to implement the parser," it means this project.

## Chapters

1. [What NBS Is and Why](01-what-nbs-is.md) -- The problem, the approach, the running example.
2. [Setup](02-setup.md) -- Install, verify, create your project directory.
3. [Running Your First Session](03-first-session.md) -- Launch, observe, shut down. The hello world.
4. [How the Team Works](04-how-the-team-works.md) -- Roles, the 3Ws, self-check, task scoping.
5. [Communication](05-communication.md) -- Chat commands, @mentions, the event bus, slash commands.
6. [Oracles](06-oracles.md) -- Librarian, Pythia, Shepard, Fixup. Periodic assessment workers.
7. [Troubleshooting](07-troubleshooting.md) -- Common problems, diagnosis, fixes.
