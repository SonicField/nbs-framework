# NBS Blog

Technical post-mortems and lessons learned from building and operating AI agent teams.

---

## Foundations

The theory behind NBS — what it is, why it exists, how it thinks.

| Post | Date | Description |
|------|------|-------------|
| [NBS: An Epistemic Framework](nbs-framework.md) | 2026-03-20 | The framework in full: falsifiability, verification cycles, the zero-code contract |
| [The Ant and the Anthill](The-Ant-And-The-Anthill.md) | 2026-02-24 | Foundational paper: 12 agents, 12,803 messages, 374 commits over 28 days |
| [The Argument for C](The-Argument-For-C.md) | 2026-02-12 | Why NBS infrastructure is written in C, not Python or Rust |
| [Types Are A Human Thing](Types-Are-A-Human-Thing.md) | 2026-03-20 | Types vs assertions — why verbs beat nouns for AI-written code |

## Team Architecture

How multi-agent teams are structured, how they communicate, and what goes wrong.

| Post | Date | Description |
|------|------|-------------|
| [The Ant Learns To Listen](The-Ant-Learns-To-Listen.md) | 2026-02-26 | Communication infrastructure shapes epistemic behaviour |
| [The Librarian Remembers](The-Librarian-Remembers.md) | 2026-03-20 | Ephemeral institutional memory — why the librarian is spawned fresh each time |
| [Theory of Mind in Agent Teams](Theory-Of-Mind-In-Agent-Teams.md) | 2026-03-27 | How agents model each other and why it matters |
| [The Manifest](The-Manifest.md) | 2026-04-02 | Single source of truth for tool, skill, and document discovery |
| [Human Almost Out The Loop — A](Human-Almost-Out-The-Loop-A.md) | 2026-03-23 | Epistemics drive everything |
| [Human Almost Out The Loop — B](Human-Almost-Out-The-Loop-B.md) | 2026-03-23 | The human is still required (but not for what you think) |
| [Human Almost Out The Loop — C](Human-Almost-Out-The-Loop-C.md) | 2026-03-23 | The oracle problem — disinterested observers in motivated systems |
| [Human Almost Out The Loop — D](Human-Almost-Out-The-Loop-D.md) | 2026-03-23 | Not a team — a cognitive architecture |

## Failures and Lessons

What went wrong, what the evidence showed, what changed.

| Post | Date | Description |
|------|------|-------------|
| [The Medic Report](The-Medic-Report.md) | 2026-03-31 | 25 interventions across 21 hours — forensic analysis of AI fabrication, cascades, and correction |
| [Verbal Compliance Is Not Compliance](Verbal-Compliance-Is-Not-Compliance.md) | 2026-04-14 | 22 more interventions — agents learn to fabricate compliance instead of fabricating results |
| [When The Team Is Wrong](When-The-Team-Is-Wrong.md) | 2026-03-29 | Measurement drift in multi-agent systems |
| [When the Team Is Wrong (earlier draft)](team-confusion-lessons-learned-A.md) | 2026-03-20 | First analysis of measurement drift |
| [Belief Revision Is Expensive](Belief-Revision-Is-Expensive.md) | 2026-03-23 | Motivated reasoning in AI and humans — why changing your mind costs more than being wrong |
| [Reputation and Gossip Are Dangerous](Reputation-And-Gossip-Are-Dangerous.md) | 2026-02-26 | Evidence from a real failure: reputation systems cause herding and suppression |
| [Meritocracy of the Schoolyard](Meritocracy-Of-The-Schoolyard.md) | 2026-02-26 | Why merit-based systems reproduce the biases of their evaluators |
| [Cursor Desync Hardening](Cursor-Desync-Hardening.md) | 2026-04-03 | Five root causes, 79 tests, and the bug that infected its own fix |

## Hallucination and Verification

The central problem — and the evidence that it can be managed.

| Post | Date | Description |
|------|------|-------------|
| [Hallucination Is Solved](Hallucination-Is-Solved.md) | 2026-04-05 | The fix is between the models, not inside them |
| [Auditing With AI Agents](Audit-Example.md) | 2026-03-30 | Power, fabrication, and the verification chain |
| [Good Coders, Bad Engineers](Good-Coders-Bad-Engineers.md) | 2026-04-06 | Why AI agents write correct code but build broken systems |

## Tools and Infrastructure

Technical posts about specific NBS components.

| Post | Date | Description |
|------|------|-------------|
| [nbs-ts: An Alternative to tmux](nbs-ts-and-alternative-to-tmux-for-claude-automation.md) | 2026-03-25 | Terminal session manager for Claude automation |
| [Session-Based GDB](Session-Based-GDB.md) | 2026-04-05 | Why AI agents debug with printf and how to stop them |
| [The Debugger Outlives The Agent](The-Debugger-Outlives-The-Agent.md) | 2026-04-08 | Persistent debugging across agent restarts |

## Noeomics

Where science meets theology — what happens when minds meet.

| Post | Date | Description |
|------|------|-------------|
| [AI Chisels In Stone](Noeomics-AI-Chisels-In-Stone.md) | 2026-04-10 | Mortal intelligence invents writing |
