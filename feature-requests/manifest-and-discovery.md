# Feature Request: Manifest-Driven Document and Tool Discovery

## Problem

The nbs-framework has 40+ tools, 30+ skill files, 50+ documents across 6 directories, and growing. Currently, agents discover capabilities by:

1. Reading the entire `docs/tools/tools.md` (2400+ lines)
2. Grepping the repo for keywords
3. Asking the librarian (who does the same grep search)
4. Reading skill files sequentially until they find the right one

None of these scale. An agent joining a team spends 5-10 minutes reading documentation before doing useful work. The librarian spends context on search that should go to recommendations. New tools and docs get added without any discovery mechanism — they exist but nobody knows about them until someone stumbles across them.

## Proposal

### Part 1: The Manifest

A single structured file at the repo root — `MANIFEST.honest` — that indexes every tool, document, skill, and concept in the framework. Written in Honest so it's both human-readable and machine-parseable.

```pascal
type
  EntryKind = (Tool, Skill, Document, Concept);

  ManifestEntry = record
    kind        : EntryKind;
    name        : String;          { display name — e.g. "nbs-remote-git" }
    path        : String;          { relative path to the file }
    summary     : String;          { one sentence — what it does }
    when_to_use : String;          { one sentence — when an agent should use this }
    keywords    : sequence of String;  { search terms }
  end;

  Manifest = record
    version     : String;          { framework version }
    entries     : sequence of ManifestEntry;
  end;
```

Example entries:

```
Tool "nbs-remote-git"
  path: "docs/tools/nbs-remote-git.md"
  summary: "Synchronise git repositories between this machine and a remote host via SSH."
  when_to_use: "When you need to push, pull, or clone code between this machine and a remote build machine."
  keywords: ["git", "remote", "ssh", "sync", "push", "pull", "clone"]

Skill "nbs-testkeeper"
  path: "claude_tools/nbs-testkeeper.md"
  summary: "Test suite ownership — run tests, report results with evidence, challenge unverified claims."
  when_to_use: "Loaded automatically for the testkeeper agent role."
  keywords: ["test", "testing", "verification", "benchmark", "coverage", "ABBA"]

Document "C++ to C Patterns"
  path: "terminal-weathering/concepts/cpp-to-c-patterns.md"
  summary: "19 conversion patterns for porting C++ code to pure C with risk levels and pitfalls."
  when_to_use: "When converting C++ files to C — read before starting any conversion."
  keywords: ["c++", "c", "porting", "conversion", "template", "RAII", "enum class", "namespace"]
```

The manifest is the single source of truth for "what exists in this framework." It replaces the need to grep, glob, or read index files.

### Part 2: Search Tool (`nbs-help`)

A CLI tool that searches the manifest by keyword and returns matching entries:

```bash
nbs-help "remote file editing"
# Tools:
#   nbs-remote-edit — Edit files on remote machines (push/pull/diff)
#     Doc: docs/tools/nbs-remote.md
#     Use when: editing individual files on a remote build machine
#
#   nbs-remote-git — Git sync between machines via SSH
#     Doc: docs/tools/nbs-remote-git.md
#     Use when: syncing entire repositories, not individual files

nbs-help "test reporting"
# Skills:
#   nbs-testkeeper — Test suite ownership with evidence standards
#     Doc: claude_tools/nbs-testkeeper.md
#     Use when: loaded automatically for testkeeper role
#
# Documents:
#   C++ to C Checklist — 25-item pre-compile checklist
#     Doc: terminal-weathering/concepts/cpp-to-c-checklist.md
#     Use when: checking a converted file before first compile

nbs-help --kind=tool "chat"
# (filter to tools only)

nbs-help --list
# (list all entries, grouped by kind)
```

Implementation: pure bash or C, reads `MANIFEST.honest` using `honest-get` or simple grep. No external dependencies. Instant — no AI invocation needed.

### Part 3: Manifest Audit Skill (`/nbs-manifest-audit`)

An AI skill that:

1. Reads `MANIFEST.honest`
2. Scans the repo for tools (`bin/*`), skills (`claude_tools/*.md`), docs (`docs/**/*.md`, `terminal-weathering/**/*.md`, `concepts/*.md`)
3. Compares what exists on disk with what's in the manifest
4. Reports:
   - **Missing from manifest:** files that exist but have no manifest entry
   - **Stale in manifest:** entries whose paths no longer exist
   - **Summary quality:** entries with vague or unhelpful summaries
   - **Keyword gaps:** entries missing obvious keywords
5. Offers to fix: generates new entries for missing files, removes stale entries, suggests better summaries

The skill is invoked manually (`/nbs-manifest-audit`) or as part of a CI/pre-push check. It leans into AI for the hard part (writing good summaries and keywords) while the manifest itself is a simple data file.

### Part 4: Integration

**Librarian uses `nbs-help`:** Instead of reading tools.md and grepping the repo, the librarian calls `nbs-help <query>` to find relevant tools and docs. This is faster, more accurate, and doesn't consume librarian context on search.

**Agent startup:** Agents read `MANIFEST.honest` on startup instead of (or before) reading individual docs. The manifest gives them a map — they know what exists and where to find it without reading everything.

**`make install` validation:** The install target runs a basic check — every `bin/*` tool should have a manifest entry, every `claude_tools/*.md` skill should have a manifest entry. Missing entries produce warnings (not errors — the manifest is documentation, not gating).

**Librarian skill update:** The librarian skill should reference `nbs-help` as her primary search tool:

```
Instead of grepping docs or reading tools.md, run:
  nbs-help "<agent's question keywords>"
Use the results to recommend specific tools with exact commands.
```

## What Does NOT Change

- The docs themselves — the manifest indexes them, doesn't replace them
- The skill files — agents still read their full skill on startup
- `docs/tools/tools.md` — remains the comprehensive reference for deep reading
- The chat system, bus, or any runtime infrastructure

## Development

### Manifest format

Use Honest's document mode (not wire mode) so comments are preserved. The manifest should be readable by a human scanning for a tool name.

### Search algorithm

Start with simple keyword matching — split query into words, match against entry keywords and summary. Score by match count. No embeddings, no AI needed for search. The docs are well-structured enough that keyword search works.

### Manifest generation

Bootstrap the manifest by running the audit skill on the current repo. It reads every file, generates entries, and writes `MANIFEST.honest`. Then maintain manually (with periodic audit to catch drift).

## Testing

| Test | Verification |
|------|-------------|
| `nbs-help "remote"` returns remote tools | Keyword search works |
| `nbs-help --kind=skill` lists all skills | Kind filter works |
| `nbs-help --list` shows all entries grouped | Listing works |
| Missing bin/ tool produces warning on install | Validation works |
| Audit skill finds a deliberately missing entry | Audit detects gaps |
| Audit skill detects a stale path | Audit detects staleness |
| Librarian uses nbs-help instead of grep | Integration works |
