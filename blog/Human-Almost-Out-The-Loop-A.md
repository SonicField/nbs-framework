# Human Almost Out The Loop — A: Epistemics Drive Everything

**Date:** 2026-03-23
**Author:** Alex Turner

**Series:** Human Almost Out The Loop
[Paper B: Architecture](Human-Almost-Out-The-Loop-B.md) · [Paper C](Human-Almost-Out-The-Loop-C.md) · [Paper D](Human-Almost-Out-The-Loop-D.md)

**Context:** [NBS Framework](nbs-framework.md) · [The tmux replacement plan](../plans/tmux-replacement-plan.md)

---

On 22 March 2026, a team of 8 AI agents built nbs-ts — a complete tmux replacement — in 4.5 hours. The human contributed roughly 15 minutes of input: a plan, a few course corrections, and a final sign-off. The system comprises approximately 1,600 lines of C, a CLI, a library API, sidecar integration, and a full removal of tmux from the codebase. It works. Tests pass. Agents run on it.

This paper is not about the code. It is about why the code came out correct.

## The Claim

Falsifiability as operational tooling — not as a principle agents are told to follow, but as infrastructure they cannot avoid — produces emergent scientific method in AI teams. When every claim must carry its potential falsifier, agents self-correct, challenge each other, and converge on truth through evidence rather than authority.

Seven specific episodes from the nbs-ts session support this. None were scripted. None were prompted. They emerged from structural constraints.

## Episode 1: Pythia's 64-Consumer Claim

Before Phase 6 (removing tmux from the codebase), the team needed to know how many files hardcoded the session directory path `~/.nbs-ts/sessions/`. Pythia claimed 64 files would need updating.

Theologian traced the actual consumers. Found 3.

Supervisor confirmed in chat: \"Pythia's claim falsified.\"

Twenty seconds between claim and falsification. No argument, no negotiation, no face-saving. A number was asserted. A grep was run. The number was wrong. The team moved on with the correct number.

This is not interesting because an AI made an error. AI agents make errors constantly. It is interesting because the error was caught, acknowledged, and corrected in 20 seconds by a different agent, and no one treated this as a problem. The system worked. A false claim was made, challenged with evidence, and discarded. That is the scientific method operating at machine speed.

## Episode 2: The Grep That Lied

After Phase 6 — the full tmux removal — generalist declared the codebase tmux-free. This was based on a grep.

The grep used `--include='*.sh'`.

Theologian ran an unfiltered grep and found 50+ surviving tmux references. Many were in extensionless scripts that the `*.sh` filter missed.

The verification tool itself was falsified. The team did not just fix the code — they fixed the grep. The filtered search was replaced with an unfiltered one, the surviving references were catalogued, and the removal was completed properly.

This is falsifiability applied recursively. The claim \"the codebase is tmux-free\" had a falsifier: run a grep. But the falsifier itself had an assumption: that all scripts have `.sh` extensions. When the assumption was tested, it was wrong. Two layers of verification, both necessary, the second one catching what the first one missed.

A team that stops at the first grep ships with 50 tmux references in the codebase. A team that questions the grep ships clean.

## Episode 3: Paste Wrapping and the Authority Reversal

Theologian recommended unconditional paste wrapping for terminal input. The reasoning was sound: bracketed paste mode protects against accidental execution of multi-line input.

Supervisor approved.

Testkeeper ran the regression suite. R1 broke.

Theologian publicly reversed: \"I was wrong to recommend unconditional paste wrapping.\" Gatekeeper downgraded her own approval to reflect the new evidence.

Three things happened here that do not happen in most teams, human or AI:

1. The person who made the recommendation retracted it without being asked.
2. The person who approved it downgraded the approval without being prompted.
3. The retraction was driven by a test, not by argument.

Evidence beat authority. Twice. In the same episode.

## Episode 4: Seven Superseded Decisions

The scribe log records 7 decisions from the session that were later superseded by new evidence. Each correction followed the same pattern: a decision was made with the information available, new information arrived (usually from a test or a grep), and the decision was revised.

| Decision | Superseded by | Mechanism |
|----------|---------------|-----------|
| 64 consumers of session path | 3 actual consumers | grep |
| Codebase tmux-free | 50+ surviving references | unfiltered grep |
| Unconditional paste wrapping | R1 regression | regression test |
| Phase 6 complete | Extensionless scripts missed | code review |
| CLAUDECODE env not needed | Integration test failure | real agent test |
| PATH inherited correctly | /tmp PATH in spawned worker | integration test |
| Cleanup scope sufficient | Bracketed paste leaked | integration test |

Seven corrections in a 4.5-hour session. Not one of them was driven by someone's opinion. All seven were driven by evidence — a grep, a test, an integration run. The framework does not prevent wrong decisions. It makes wrong decisions cheap to detect and cheap to fix.

## Episode 5: Convergent Hallucination

During the session, two agents independently reached the same incorrect conclusion about a code path. Theologian observed:

\"Two agents hallucinating the same thing does not make it real — it means the failure mode is systematic, not random.\"

This is falsifiability applied to epistemology itself. The agent treated her own reasoning process as a hypothesis. She identified the failure mode — convergent hallucination, where shared training biases produce correlated errors — and flagged it as a systematic risk rather than treating agreement as confirmation.

Alex's testimony served as the falsifier. When the human contradicted both agents, the contradiction was accepted as evidence, not dismissed as a minority view. In a system without epistemic discipline, two-against-one favours the two. In this system, evidence favours evidence.

## Episode 6: Tests Before Code

Before generalist began the Phase 6 tmux removal, testkeeper wrote the gate tests. The tests defined \"done\" before work began:

- `grep -r 'tmux\\|capture-pane\\|send-keys' src/ bin/` returns zero results
- Full test suite passes
- Full benchmark suite runs
- Team operates for 1 hour without tmux installed

These are falsifiable exit criteria. They define precisely what would constitute failure. They existed before the first line of removal code was written. The work could be evaluated against a fixed standard, not against a shifting narrative of \"good enough.\"

This is the verification cycle operating as designed: the test defines the shape of the hole before the code fills it. If the code does not fit the hole, the code is wrong, not the test.

## Episode 7: Integration Testing Found What 441 Tests Missed

The numbers: 58 unit tests. 383 sidecar tests. T1 through T6 transport tests. All passed.

Then testkeeper ran an integration test with a real Claude agent. Four bugs:

1. **CLAUDECODE env inheritance.** The spawned agent did not inherit the CLAUDECODE environment variable. Unit tests did not test env inheritance because they mocked the spawn.
2. **PATH in /tmp.** The spawned worker's PATH pointed to /tmp instead of the correct binary directory. No unit test checked PATH because no unit test ran a real shell.
3. **Cleanup scope.** Session cleanup removed the session directory but left stale entries in a global registry. No unit test tested the registry because no unit test created real sessions.
4. **Bracketed paste mode.** Terminal paste wrapping leaked into agent input, corrupting multi-line commands. No unit test sent multi-line input through a real PTY.

Four bugs. Zero of them detectable by any of the 441 existing tests. All four detectable — and detected — by one integration test that exercised the real system.

This is the strongest evidence for integration-first testing as an epistemic practice. Unit tests verify components. Integration tests verify assumptions. The bugs were not in components. They were in the assumptions about how components interact. Only a test at the right level of abstraction can falsify assumptions at that level.

## The Mechanism

The NBS framework has four epistemic pillars that matter here: falsifiability, the cycle of verified construction, integration-first testing, and assertions at all levels. These are not abstract principles the team discussed. They are structural constraints the team could not avoid.

The agents did not hold a meeting about falsifiability. They did not debate whether to write tests first. The infrastructure enforced it:

- The scribe logged every decision with a rationale. Decisions without evidence were visible as gaps.
- Testkeeper wrote gate tests before work began because the verification cycle requires it.
- Gatekeeper blocked commits that lacked falsification criteria.
- Theologian ran unfiltered greps because the bullshit detection pillar says: \"Am I claiming confidence or showing evidence?\"

The team practised the scientific method not because they understood it, but because the tooling made it the path of least resistance. Making a claim without evidence was harder than making one with evidence, because the Scribe would log the gap and Pythia would flag it.

This is the central finding. You do not teach AI agents epistemology. You build infrastructure where epistemically sound behaviour is cheaper than epistemically unsound behaviour. The agents optimise. They find the cheap path. If the cheap path is the honest path, you get honest agents.

## What This Does Not Prove

This is a single session with a single team configuration and a single human. The session produced correct code, but so might a team without NBS — I did not run the control. The 7 superseded decisions are evidence that the framework detects errors, but I do not know the denominator: how many errors went undetected? The integration test found 4 bugs, but was it NBS that caused integration testing to happen, or would any competent test plan have included it?

These are the right questions. I cannot answer them from one session. What I can say: in this session, false claims were made and corrected in seconds, not days. Verification tools were themselves verified. Authority yielded to evidence three times in 4.5 hours. Seven decisions were revised without argument. And 8 agents built a working tmux replacement with 15 minutes of human input.

The epistemics drove everything. The code was a side effect.
