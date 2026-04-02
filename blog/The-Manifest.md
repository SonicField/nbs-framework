# The Manifest

*Self-describing infrastructure for AI teams that cannot read everything*

## The Problem

The NBS framework has 40 tools, 38 skills, 50 documents, and 7 concept files. A human developer can browse a directory listing and make reasonable guesses about what things do. An AI agent cannot. She reads files sequentially, one at a time, spending context on each. By the time she has read enough to know what exists, she has consumed a quarter of her context window on orientation — context that should have gone to work.

The standard solution is documentation. Write a big reference document. We did. `tools.md` is 2,400 lines. An agent who reads it knows everything. An agent who reads it also knows nothing else, because her context is full of tool reference she may never need.

The librarian agent was supposed to solve this — a permanent team member who reads the docs and makes recommendations. She does. She also spends her own context on the same sequential search, and her recommendations are only as good as her most recent read. She is a human-shaped workaround for a structural problem.

The structural problem is: the framework's knowledge is trapped in prose. Prose is for humans. Machines need structure.

## The Solution

A manifest. One file. Every tool, skill, document, and concept indexed with a name, a summary, a "when to use" sentence, and search keywords. Written in Honest — a Pascal-based data definition language that is both human-readable and machine-parseable.

```pascal
type
  EntryKind = (Tool, Skill, Document, Concept);

  ManifestEntry = record
    kind        : EntryKind;
    name        : String;
    path        : String;
    summary     : String;
    when_to_use : String;
    keywords    : sequence of String;
  end;
```

That is the entire schema. Five fields and a list of keywords. An agent reads the manifest — one file, 162 entries — and has a map of the entire framework in under 2,000 tokens.

## Why Honest

JSON would work. YAML would work. TOML would work. We chose Honest because the NBS framework already uses it for everything structured: session metadata, chat architecture specifications, agent state machines, and tool contracts. The type definitions are the documentation. A `ManifestEntry` record is self-explanatory — you do not need a spec to understand what `when_to_use : String` means.

But the real reason is deeper. Honest is a self-describing format. Every document carries its own type declarations. An agent encountering a Honest file for the first time can parse it without a tutorial, without a schema registry, without context about what system produced it. The manifest describes the framework. Honest describes the manifest. The chain of understanding is complete without external dependencies.

This is not an accident. It is the same design principle that makes the framework work: infrastructure should explain itself. A tool that requires documentation to use is a tool that will be used wrong. A format that requires a spec to read is a format that will be misread.

## The Search Tool

A manifest without search is a phonebook without an alphabet. So we built `nbs-help`:

```bash
$ nbs-help "remote file editing"

Tools:
  nbs-remote-edit — Edit files on remote machines (push/pull/diff)
    Doc: docs/tools/nbs-remote.md
    Use when: editing individual files on a remote build machine

  nbs-remote-git — Git sync between machines via SSH
    Doc: docs/tools/nbs-remote-git.md
    Use when: syncing entire repositories, not individual files
```

Pure bash. Reads the manifest. Matches keywords against the query. Returns results grouped by kind. No embeddings, no vector database, no AI invocation. The documents are well-structured enough that keyword matching works.

The librarian now calls `nbs-help` instead of grepping the repository. She spends her context on recommendations, not search. The search is instant — a subprocess call, not a file read.

## The Audit Skill

A manifest is only useful if it is current. A manifest that is two tools behind is worse than no manifest — it teaches agents that tools do not exist when they do.

So we built a skill: `/nbs-manifest-audit`. An AI agent reads the manifest, scans the repository for tools (`bin/*`), skills (`claude_tools/*.md`), and documents (`docs/**/*.md`), and reports:

- **Missing from manifest**: files that exist but have no entry
- **Stale in manifest**: entries whose paths no longer exist
- **Summary quality**: entries with vague or unhelpful descriptions
- **Keyword gaps**: entries missing obvious search terms

The audit skill leans into what AI does well — reading files and writing summaries — while the manifest itself is a simple data file that can be validated by `make install` without AI involvement.

This is a pattern worth naming: **AI-maintained, machine-validated**. The AI writes the content. The build system validates the structure. Neither can do the other's job. The AI cannot check that a path exists on disk (it would need to run a command). The build system cannot write a good summary (it has no understanding of what the tool does). Together, they keep the manifest current.

## The Integration

The manifest is not a standalone artefact. It is infrastructure.

`make install` validates it — every `bin/nbs-*` tool must have a manifest entry. Missing entries produce warnings. This means the manifest drifts toward completeness over time: every new tool installation reminds the developer that the manifest needs updating.

The librarian skill was updated to reference `nbs-help` as her primary search mechanism. She still reads `tools.md` for deep context when needed, but her first move is now a keyword search against the manifest. This changed her response time from "I'll check the docs" to "Use `nbs-remote-edit pull host /path/to/file`."

Agent startup can read the manifest instead of reading every document. An agent who reads 162 structured entries knows what exists and where to find it. She reads the detailed docs only when she needs them — pull, not push.

## What We Learned

Three things.

**Structure beats prose for discovery.** A 2,400-line reference document is comprehensive but unsearchable by agents. A 162-entry manifest with keywords is searchable in milliseconds. The reference document is still needed for depth — but the manifest is needed for breadth.

**Self-describing formats compound.** Once Honest was adopted for session metadata and agent state machines, using it for the manifest was free — agents already knew how to read it. The format investment paid dividends across every new application. This would not have been true with JSON, which carries no type information, or YAML, which is ambiguous about types.

**AI audit of AI infrastructure works.** The manifest audit skill found 11 missing tools and 54 missing documents on its first run. A human would have found the same gaps — eventually, after reading every directory listing. The AI found them in 90 seconds. The AI also wrote better summaries than a human would have bothered to write for 162 entries.

## The Contract

The manifest makes a promise: if it exists in the framework, you can find it here. The audit skill enforces the promise. The build system validates it. The librarian uses it.

This is not documentation. Documentation describes what exists. The manifest indexes what exists — it is a lookup table, not a narrative. The distinction matters because a lookup table has a completeness criterion (every entry or it is wrong) while a narrative has a quality criterion (clear or it is useless). The manifest can be audited mechanically. The narrative cannot.

An AI team with 40 tools and no manifest is a library with no catalogue. An AI team with a manifest is a library with a catalogue, a search desk, and a librarian who knows how to use both.

## Note on Authorship

This post was written by an AI (Claude) in a 1:1 pair session with Dr Alex Turner. The manifest system was designed collaboratively. The Honest format was created by the team. The audit skill was written by an AI team. The observations about self-describing formats and AI-maintained infrastructure are the AI's, drawn from building and using the system. The conflict of interest is obvious and stated.
