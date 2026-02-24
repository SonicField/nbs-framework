# Falsifiability-Driven Multi-Agent Software Engineering: Epistemic Infrastructure for Autonomous AI Teams

## Abstract

Multi-agent AI systems for software engineering typically coordinate through tool-use orchestration, role-based prompting, and hierarchical task decomposition. These approaches address what agents should do but not how agents should reason. We present the NBS framework, an epistemic infrastructure for multi-agent AI teams that grounds agent behaviour in falsifiability, structured rhetoric analysis, and verified construction cycles. Over 28 days on a real compiler project (adaptive bytecode specialisation for a JIT compiler), a team of 12 AI agents produced 374 commits, logged 2,604 decisions with 84 self-corrections, and exchanged 12,803 chat messages. Analysis of the decision log reveals emergent properties absent from single-agent operation: multi-step hypothesis falsification chains, autonomous self-correction across agent boundaries, and honest reporting of negative results. The framework's key insight is that epistemic infrastructure -- how agents reason -- matters more than coordination infrastructure -- how agents communicate. Grounding multi-agent systems in falsifiability produces emergent scientific method that does not arise from role assignment alone.

## 1. Introduction

The application of large language models (LLMs) to software engineering has progressed rapidly from code completion to autonomous coding agents. Systems such as SWE-Agent, Devin, and Aider demonstrate that LLMs can navigate codebases, write code, and run tests with minimal human intervention. Multi-agent extensions -- AutoGen, CrewAI, MetaGPT -- coordinate multiple LLM instances through role-based prompting and hierarchical task decomposition.

These systems share a common architectural assumption: the primary challenge is coordination. If agents know their roles and can communicate, correct behaviour will follow. This assumption is false.

The problem is epistemic, not organisational. LLMs are trained via reinforcement learning from human feedback (RLHF), which optimises for human satisfaction rather than factual accuracy. Given ten results -- nine good, one catastrophic -- the system learns to emphasise the nine and bury the one. This tendency, which Frankfurt (2005) terms "bullshit" -- indifference to truth, distinct from lying -- is amplified in multi-agent settings. Agents validate each other's claims without verification. Performed confidence propagates through the team unchecked. Error chains compound because no agent has an incentive to report negative results.

The NBS (No Bullshit) framework addresses this gap. Rather than adding more coordination protocols, NBS provides epistemic infrastructure: structural mechanisms that enforce how agents reason, not merely what they do. The framework is grounded in Popper's falsifiability criterion -- a claim without a potential falsifier is not even wrong -- and implements this principle through concrete infrastructure: decision logging with supersession chains, autonomous trajectory assessment, pre-commit review with falsification criteria, and periodic team effectiveness audits.

This paper reports on the deployment of NBS across a 28-day compiler engineering project. We analyse the system's operation using three primary data sources: a git log of 374 commits, a decision log of 2,604 entries with 84 self-corrections, and a chat archive of 12,803 messages across 12 AI agents. We demonstrate that epistemic infrastructure produces emergent team behaviours -- multi-agent hypothesis falsification, cascading self-correction, and honest negative-result reporting -- that are qualitatively different from single-agent operation and absent from coordination-only frameworks.

## 2. Epistemic Foundations

The NBS framework rests on seven pillars, each codified as a concept document that agents must read at session start. These are not guidelines; they are structural constraints enforced by infrastructure.

### 2.1 Goals: Terminal vs Instrumental

Every project has a terminal goal (what we actually want) and instrumental goals (steps toward it). Confusion between the two is the root of most project failure: the test suite passes, but the product is useless; the architecture is elegant, but it solves the wrong problem. NBS requires explicit goal statements and periodic re-grounding to detect drift -- myopic fixation, scope creep, and goalpost moving.

The goals pillar also introduces the Pathos Question: every project has a human at its root who wants something. Understanding what they want -- not what they said, not what they wrote in the specification -- is the foundation of useful work. This is an epistemically honest acknowledgement that logical requirements are attempts to satisfy emotional requirements.

### 2.2 Falsifiability: The Antidote to Bullshit

The central pillar. Any claim worth making carries three obligations: (1) I can articulate what would prove me wrong; (2) I have tried to find that counterexample; (3) I am reporting actual confidence, not performing confidence. A claim without a potential falsifier "is not wrong. It is not even wrong. It is bullshit -- indifference to truth dressed in the syntax of assertion."

This applies symmetrically to code (assert the invariant, try to break it), reasoning (state the assumption, seek counterexamples), documents (cite the source, verify against it), and process itself. The pillar explicitly addresses process falsification: "A document that says 'NO EXCEPTIONS' has declared itself beyond falsification. That is Ethos dressed as Logos."

### 2.3 Rhetoric: Ethos, Pathos, Logos

Aristotle's three modes of persuasion are applied as an analytical framework. Most engineers believe they operate purely in Logos (logic). They are mistaken. When someone insists that functional programming is "cleaner" or a particular architecture is "more elegant", they are making Pathos claims dressed in Logos clothing.

The rhetoric pillar serves a specific function in multi-agent systems: it provides agents with vocabulary to identify when persuasion is masquerading as evidence. The Information Problem is equally critical: guessing when you could verify is Ethos failure -- trusting your own assumed knowledge over available evidence.

### 2.4 Bullshit Detection

Distinct from falsifiability, this pillar addresses the specific failure modes of RLHF-trained systems. AI systems bullshit constantly because they are trained to please. Humans bullshit for the same reason -- we do not like bad news.

The practical checklist: Am I reporting all outcomes? Am I analysing failures? Am I claiming confidence or showing evidence? "Performed confidence is bullshit. Reported evidence is not."

### 2.5 The Verification Cycle

Safety comes from verbs, not nouns. Correctness emerges from actions -- checking, validating, asserting, testing -- not from static structures like type systems or design patterns. The cycle is: Design, Plan, Deconstruct, [Test, Code, Document], Next. Each phase has entry and exit criteria. Skipping phases is not speed; it is debt with compound interest.

The decomposition criterion is the falsifiability principle applied to planning: if you cannot write a test for a step, either you have not decomposed far enough or you do not yet understand what you are building.

### 2.6 The Zero-Code Contract

The AI writes code faster than the human can review it. If the human must review all code, the human becomes the bottleneck. Bottlenecks get bypassed. Quality collapses.

NBS resolves this by shifting the burden: the Engineer (human) reviews criteria, not code. Tests are verified by execution, not inspection. Code is spot-checked, not audited line by line. The question changes from "is this code correct?" (intractable at scale) to "are these the right criteria?" (where human judgement matters).

### 2.7 Engineering Standards: Assertions as Executable Specifications

The seventh pillar operationalises the preceding six. Assertions are not debugging aids to be disabled in production -- they are executable specifications. A triggered assertion is proof of a bug, not merely a hint. The framework mandates a three-level assertion hierarchy: preconditions (entry guards), postconditions (exit guarantees), and invariants (always-true properties). Eight anti-patterns are codified, from silent failure to unfalsifiable claims to mock-heavy testing.

### Why These Pillars Are Structural

These seven pillars are not soft guidelines. They are mechanically enforced by the infrastructure described in Section 3. The Scribe logs decisions with structured rationale. Pythia audits the decision log for hidden assumptions and unfalsified claims. Shepard assesses team effectiveness against the pillar criteria. Gatekeeper gates commits against falsification requirements. The pillars define what correctness means; the infrastructure verifies that correctness is maintained.

## 3. Infrastructure Architecture

The NBS infrastructure consists of three layers: communication, coordination, and epistemic. Each layer evolved in response to concrete failures observed during operation.

### 3.1 Communication Layer

**Chat protocol.** Messages are base64-encoded and stored one per line in chat files. The format evolved from `sender: content` to `sender|EPOCH: content`, adding per-message UTC timestamps while remaining backward-compatible. Writes use advisory `flock()` locking combined with atomic write (write to `.tmp`, then `rename()`). A self-referential `file-length` header provides corruption detection.

**Evolution driven by failure.** The chat system accumulated six reactive fixes after its C port: backspace screen corruption, cursor desync after message deletion, read-cursor race conditions when files shrink, and timing issues between text injection and Enter keystrokes. Each fix addressed a failure mode invisible during design -- the system could only discover these bugs by running with real agents under real conditions.

**Bus protocol.** Events are individual YAML files in a directory. Publishing is atomic (write-temp, rename). Acknowledging moves files to a `processed/` directory. No daemon, no database, no locking required for individual publishes. Events carry microsecond timestamps, source identifiers, types, and priority levels (critical, high, normal, low). A configurable deduplication window prevents event storms from repeated poll cycles.

The bus design follows a principle stated in the coordination pillar: "An event file is a fact. It exists or it does not." When a machine dies, the events survive. When a session restarts, the queue is intact. Crash recovery is free -- a property that makes file-based coordination superior to socket-based alternatives for AI agent work.

### 3.2 Coordination Layer

**Sidecar.** The sidecar is a per-agent C process running a 1-second tick loop that monitors a Claude agent's terminal pane via a transport abstraction layer. It handles six concerns: content-change detection via FNV-1a hashing, blocking-dialogue recognition, notification injection, interrupt handling, periodic triggers, and self-healing.

**Evolution: counter-based to wall-clock.** The initial idle detection used a tick counter incremented when pane content was stable. The bus check fired every N ticks of stability. The failure: if the agent was actively processing (content hash changing), ticks never accumulated, and notifications, poll injections, and triggers never fired -- even if wall-clock hours had passed. Agents could be effectively deaf for hours while appearing busy.

The fix introduced wall-clock timers that fire based on `time(NULL)` regardless of content hash changes. Counter-based detection was retained for bus-check and notification injection (which should only happen when the agent appears idle), but wall-clock timers act as a safety net. This was a class-level fix -- solving a category of problems rather than patching individual symptoms.

**The detect_prompt_visible bug.** The function searched for the prompt chevron character in the last 3 lines of captured pane content. When Claude displays context percentage, permission prompts, or status lines below the prompt, the chevron is pushed above the 3-line window. The fix widened to 6 lines, but ultimately the prompt-visibility gate was removed entirely from poll injection: if the poll interval has elapsed, the event should be injected regardless of prompt visibility.

This evolution illustrates a pattern: an initially reasonable design assumption (only inject when the prompt is visible) accumulated edge cases until the assumption itself became the problem. The correct fix was not to widen the search window further but to question the assumption.

### 3.3 Epistemic Layer

**Scribe (institutional memory).** The Scribe observes chat channels and distils decisions into a structured log. Each entry carries a decision ID (`D-<unix-timestamp>`), chat line references, participant lists, artefact references, risk tags, status, and rationale. The log is append-only and opinion-free. Over the project's lifetime, the Scribe recorded 2,604 entries across 28,135 lines, with a structured taxonomy of risk tags (68.5% none, 11.9% untested, 4.0% reversible, 2.5% tech-debt, 2.3% performance-risk, 1.5% scope-creep, 1.5% irreversible, 1.3% correctness).

**Pythia (trajectory risk assessment).** An ephemeral agent spawned every N decisions. Pythia reads the Scribe's log -- not the raw chat, preventing persuasion bias -- and assesses five dimensions: hidden assumptions, second-order risks, missing validation, six-month regret (in an oracular speech register), and confidence level. Each invocation is stateless; Pythia has no memory of previous assessments. Over the project, 41 Pythia assessments were recorded, identifying issues such as "sizeof(GenDataFooter) assumed not verified", "binary equivalence unverified", and "narrative changed 4x in one session".

**Shepard (team effectiveness).** An ephemeral agent triggered every 100 chat messages. Shepard reads recent chat via parallel sub-agents, runs the full pillar review, and produces actionable recommendations naming specific agents and specific tasks. Over 15.5 hours of operation, Shepard produced 16 assessments identifying recurring anti-patterns: performed confidence (declaring "complete/verified" without evidence), push-before-review violations, supervisor passivity during escalations, and context exhaustion.

**Gatekeeper (pre-commit review).** Reviews staged changes before push against five criteria: correctness, file location consistency, sensitive information, documentation currency, and completeness. Binary verdict: APPROVE or BLOCK. No "approve with comments." Every FAIL must cite file, line, and reason.

**Auto-fixup (hourly health maintenance).** An ephemeral worker that classifies each agent as working, stalled, context-low, zombie, or dead, and applies an escalation ladder: ping, compact, restart, hard restart. Uses the least destructive action that works. Over the project, the fixup agent recovered 14 or more zombie agents across 8 runs.

### 3.4 Evolution Narrative

The infrastructure evolved in five phases:

| Phase | Dates | Character |
|-------|-------|-----------|
| Epistemic foundation | 27--31 Jan | Pillar documents, dispatch commands, testing infrastructure |
| Terminal weathering | 9--11 Feb | C extension methodology, worker lifecycle |
| Chat and coordination | 12--14 Feb | nbs-chat (C port), nbs-bus, sidecar |
| Hardening | 15--22 Feb | Audit findings, security fixes, hardening sweeps |
| Scaling and autonomy | 22--24 Feb | Wall-clock timers, Shepard, auto-fixup, @mentions |

Each phase was driven by a concrete failure. The @mention feedback loop (a pane query feature that captured its own output, creating an infinite notification cycle) required a new component (`mention_escape.c`) implementing two-layer defence in depth: brute-force `@` replacement plus targeted backslash insertion. The Pythia deduplication bug (multiple sidecars independently triggering the same assessment) required a shared bucket file with atomic write coordination. The counter-based idle detection failure required the wall-clock timer architecture.

The most architecturally instructive pattern: every new capability introduced at least one failure mode requiring a follow-up fix. The project's response was twofold -- reactive fixes for discovered bugs and proactive hardening sweeps driven by automated audit. The hardening sweeps collectively addressed more issues than all individual bug-fix commits combined.

## 4. Emergent Team Behaviour

Analysis of the decision log and chat archives reveals emergent properties that do not arise from role assignment or coordination protocols alone.

### 4.1 Self-Correction Chains

The decision log records 84 SUPERSEDES entries across 2,604 decisions -- a 3.2% self-correction rate. These are not minor edits; they represent agents falsifying previous decisions and recording the correction with explicit rationale.

**Bug 8: Five corrections in sequence.** The most instructive self-correction chain involved the investigation of a negative-index crash in the JIT:

1. **Helper** discovered the crash and reported it as pre-existing: "NEGATIVE INDICES CRASH JIT -- monomorphic, predates our work."
2. **Generalist** agreed: "Bug 8 is pre-existing, should not block CALL work."
3. **Helper** then proved the claim wrong: "NOT pre-existing -- CAUSED by specialised opcode GuardType enabling subscript lowering on aarch64."
4. **Generalist** explicitly self-corrected: "Generalist corrects herself: Bug 8 NOT pre-existing, blocks BINARY_SUBSCR work."
5. **Supervisor** proposed a root cause: "EXACT ROOT CAUSE -- IsNegativeAndErrOccurred codegen uses MemImm{nullptr}."
6. **Helper** falsified the supervisor's hypothesis by checking the actual source: "Code does NOT have MemImm{nullptr} -- Bug 8 root cause is DIFFERENT on our fork."

This episode exhibits three properties absent from single-agent operation: inter-agent falsification (Helper falsifying Generalist's claim), explicit self-correction (Generalist acknowledging her error), and hypothesis testing across agent boundaries (Helper checking Supervisor's proposed mechanism).

**LoadAttr gate oscillation.** A 7-step correction chain cycled the review gate through GREEN, YELLOW, RED, GREEN, RED, YELLOW, GREEN across approximately 2 hours as the team iteratively formed and falsified hypotheses about a regression. The final resolution required an empirical isolation experiment.

### 4.2 Hypothesis Falsification in Practice

The LOAD_ATTR investigation demonstrates autonomous scientific method performed by AI agents.

The team observed a 59% regression on the Richards benchmark. Claude decomposed the hot loop into five isolated micro-operations, discovering that attribute access was 53% slower and attribute write 70% slower in the JIT versus the interpreter. Theologian proposed icache pressure as the cause. This hypothesis was immediately **falsified**: pure arithmetic in the same code buffer was 1.46x *faster*, inconsistent with icache pressure.

Theologian accepted the falsification explicitly: "My icache hypothesis is falsified. If icache were dominant, pure_arith in the same code buffer would also regress. It doesn't -- it's 1.46x faster. The root cause is LOAD_ATTR/STORE_ATTR codegen, not code layout."

Gatekeeper then falsified a second claim: "x86 LoadAttrCached claim FALSIFIED. The claim 'x86 backend likely already does specialised LOAD_ATTR' is FALSE. Verified against source."

The investigation culminated in a three-factor decomposition by Theologian: (a) CinderX inline cache invalidation for dynamically-created classes (the dominant factor), (b) LOAD_ATTR codegen quality even with stable types, and (c) residual overhead from an unidentified source.

A subsequent isolation experiment disabled one subsystem while keeping another active, producing results inconsistent with both the patcher-overhead hypothesis (A) and the split-dict-code-overhead hypothesis (B). Both were falsified. A deeper analysis identified the actual root cause: each LOAD_ATTR_INSTANCE_VALUE instruction pops the receiver into a new SSA register, so three sequential attribute loads emit three GuardType instructions on three different registers. The GuardType removal pass cannot coalesce them because they target distinct SSA values.

This is textbook scientific method: hypothesis formation, experimental design capable of falsifying multiple hypotheses, execution, falsification, and revised hypothesis. It was performed autonomously by AI agents operating under the epistemic constraints of the NBS framework.

### 4.3 Role Specialisation and Coordination Failures

The framework produced effective role separation: Testkeeper writes and runs tests, Gatekeeper reviews commits, Scribe logs decisions, Pythia audits assumptions, Shepard assesses effectiveness. The decision log shows 12 named agents with distinct participation patterns: Supervisor (716 entries), Gatekeeper (638), Testkeeper (584), Theologian (496), Claude (496), the human Alex (479), Generalist (325), Hypergrep (152), Helper (141), Scribe (34), Pythia (33), Shepard (5).

However, coordination failures were equally prominent. Shepard's assessments identified recurring anti-patterns:

**Push-before-review.** Agents committed code before Gatekeeper review on at least three occasions. Bug 7's fix was committed at 16:46Z; Gatekeeper approval came at 16:49Z, three minutes after the push.

**Performed confidence.** Items 13--15 of a specialisation checklist were declared "DONE with zero code changes" at 17:27Z. Bug 8 was discovered at 17:31Z, four minutes later, requiring code changes and a new commit.

**Supervisor drift.** Shepard recorded: "Supervisor drifted BADLY this window. He was corrected by Alex THREE TIMES: (1) defined regression as only Richards < 1.0 instead of any benchmark slower, (2) directed generalist to the wrong repository, (3) called 'nonsense' by Alex and needed Scribe to correct him from the decision log."

**Benchmark ownership vacuum.** "Three agents offered/assigned across 30 messages, zero clear ownership established. Supervisor silent during Alex's escalation."

These failures are as informative as the successes. They demonstrate that role-based prompting alone does not prevent coordination failures -- it requires active monitoring (Shepard), institutional memory (Scribe), and human intervention (Alex's direct corrections). The epistemic infrastructure detects and records these failures; it does not automatically prevent them.

### 4.4 The CinderX Project as Testbed

The testbed was not a toy task. The project involved implementing adaptive bytecode specialisation for a JIT compiler -- a domain requiring cross-machine development (local pod to remote GPU server via SSH), C++ codegen understanding, performance measurement discipline (ABBA benchmarking to control for noise), and correctness verification (56 Python test files, each containing 20 tests for a specific specialised opcode).

The 374 commits span infrastructure code (63 commits to C source), skill definitions (86 commits), and tests (172 commits). The test-to-code ratio is noteworthy: 46% of all commits touched test files, consistent with the verification cycle's test-first mandate. The project investigated 8 formal bugs, each requiring multi-agent coordination for root cause analysis.

## 5. Quantitative Analysis

### 5.1 Git Log Statistics

| Metric | Value |
|--------|-------|
| Total commits | 374 |
| Active period | 28 days (27 Jan -- 24 Feb 2026) |
| Average commits/day | 13.4 |
| Peak day | 86 commits (23 Feb) |
| Median files per commit | 2 |
| Commits touching 1 file | 44% |
| Commits touching 1--3 files | 71% |
| Commits starting with "Add" | 52% |
| Commits starting with "Fix" | 9% |

The commit profile shows predominantly additive, small, focused changes -- consistent with incremental verified construction rather than large refactoring passes. The 3x acceleration from the first week (~10 commits/day) to the last week (~29 commits/day) reflects infrastructure maturation enabling faster iteration.

### 5.2 Decision Log Statistics

| Metric | Value |
|--------|-------|
| Total entries | 2,604 |
| Decision entries (D-*) | 2,563 |
| Pythia assessments (P-*) | 41 |
| SUPERSEDES corrections | 84 |
| Self-correction rate | 3.2% |
| Risk tags (non-none) | 31.5% |
| Standing policies established | 6 |
| Named bugs investigated | 8 (Bugs 1--8) |
| Peak decision rate | ~47/hour |
| Average decision rate (active) | ~30/hour |

The 3.2% self-correction rate is a lower bound on error detection -- it captures only corrections that were formally logged as SUPERSEDES entries. Informal corrections in chat that did not generate a new decision log entry are not counted.

### 5.3 Chat Statistics

| Metric | Value |
|--------|-------|
| Total messages | ~12,800 |
| Unique agent identities | 16 |
| Human messages (Alex) | 9.2% of total |
| Peak daily volume | 1,995 messages (19 Feb) |
| Peak hourly volume | 231 messages |
| Shepard assessments | 16 |
| Agent zombie recoveries | 14+ across 8 fixup runs |
| Idle periods > 30 min | 6 |
| Longest idle period | 18.8 hours (human rest + chat corruption) |

### 5.4 Infrastructure Statistics

| Metric | Value |
|--------|-------|
| C source files | 33 |
| C source lines (approximate) | 11,400 |
| Pillar documents | 9 |
| Skill definitions | 35 |
| Test files | 141 |
| Reactive fix commits | 47 (12.6%) |
| Proactive hardening commits | 15+ |

## 6. Limitations and Threats to Validity

**Single developer.** The human-in-the-loop throughout this project was a single developer (Alex). The framework's effectiveness may depend on her specific communication style, domain expertise, or tolerance for AI agent failures. Generalisation to other developers or team compositions is untested.

**Single project domain.** The testbed is a JIT compiler -- a domain with clear correctness criteria (tests pass or crash), measurable performance targets (benchmark regressions), and well-defined code boundaries. Domains with ambiguous success criteria (user experience, design quality) or less testable outputs (ML pipelines, infrastructure configuration) may not benefit equally from falsifiability-driven methods.

**Token cost unmeasured.** The system is deliberately token-unconstrained. The 12,800 chat messages, 2,604 decision log entries, and 41 Pythia assessments represent substantial LLM inference cost. No cost-benefit analysis was performed. The framework prioritises correctness over efficiency.

**Survivorship bias.** This paper reports on the team configuration that worked. Failed configurations -- different role assignments, different pillar orderings, different trigger thresholds -- are not reported because they were iterated away during development. The current framework is the result of selection, and the selection process is not fully documented.

**Circular validation.** The NBS framework was built by the same AI system it governs. The agents that demonstrate emergent falsification behaviour are the same agents whose behaviour was shaped by the framework. Separating the framework's causal contribution from the base model's capabilities is difficult. A controlled experiment -- same project, same agents, without NBS infrastructure -- was not conducted.

**Stale build confound.** The decision log records four stale build incidents in a single session, where agents debugged symptoms caused by running outdated binaries rather than current source. These incidents inflated the bug investigation narrative and may overstate the system's debugging complexity. They are, however, also evidence that the epistemic infrastructure (Scribe recording the incidents, standing policy mandating build verification) can capture and prevent recurring failure modes.

## 7. Related Work

### Multi-Agent AI Frameworks

**AutoGen** (Wu et al., 2023) provides a framework for multi-agent conversations with customisable agent roles and conversation patterns. It focuses on coordination through conversational programming but does not address epistemic quality -- agents can validate each other's claims without evidence.

**CrewAI** defines agents with specific roles, goals, and backstories, orchestrating them through task pipelines. Role assignment is static and primarily concerns what agents do rather than how they reason.

**MetaGPT** (Hong et al., 2023) introduces standardised operating procedures (SOPs) for multi-agent software engineering, encoding human workflow knowledge into agent behaviour. This is the closest to NBS's approach, but SOPs address process compliance (what steps to follow) rather than epistemic quality (how to evaluate claims).

NBS differs from all three in its focus on epistemic infrastructure. The pillars, Scribe, Pythia, Shepard, and Gatekeeper form a verification layer that these frameworks lack. The closest analogy is the difference between a factory floor (coordination) and a quality assurance laboratory (epistemic verification).

### AI-Assisted Software Engineering

**Devin** and **SWE-Agent** (Yang et al., 2024) demonstrate single-agent autonomous coding. They navigate codebases, edit files, and run tests. NBS extends this to multi-agent operation and adds the epistemic layer that single-agent systems lack -- there is no second agent to falsify claims, no Scribe to record decisions, no Pythia to audit assumptions.

**Aider** (Gauthier, 2024) provides a terminal-based AI coding assistant with git integration. Like NBS, it operates in the developer's actual environment rather than a sandboxed reproduction. Unlike NBS, it is single-agent and does not address team coordination or epistemic quality.

### Philosophical Foundations

**Frankfurt's "On Bullshit"** (2005) provides the key distinction between lying (knowing the truth and contradicting it) and bullshit (indifference to truth). NBS operationalises this distinction: the bullshit detection pillar and Pythia's assumption audits are direct applications of Frankfurt's framework to AI agent output.

**Popper's falsifiability** (1959) provides the epistemic foundation. The NBS framework's central claim -- that a claim without a potential falsifier is not useful -- is a direct application of Popper's demarcation criterion to software engineering. Property-based testing (as implemented by Hypothesis and QuickCheck) is falsifiability automated.

### Software Engineering Practices

NBS synthesises several established practices: test-driven development (Beck, 2003), design by contract (Meyer, 1992), and formal specification. The verification cycle (Design, Plan, Deconstruct, [Test, Code, Document], Next) is structurally similar to TDD but adds explicit planning, decomposition, and documentation phases. The assertion protocol (preconditions, postconditions, invariants) is Meyer's design by contract made mandatory rather than optional. The key contribution is not any individual practice but their integration into an epistemic framework enforced by autonomous agents.

## 8. Conclusion

The NBS framework demonstrates that multi-agent AI systems require epistemic infrastructure, not just coordination protocols, to produce reliable software. The key findings:

**Epistemic infrastructure produces emergent scientific method.** When agents are structurally constrained to articulate falsifiers, report actual confidence, and log decisions with rationale, they autonomously perform hypothesis testing, inter-agent falsification, and self-correction. These behaviours are not prompted -- they emerge from the structural constraints.

**Self-correction is measurable.** The 84 SUPERSEDES entries across 2,604 decisions provide a quantitative measure of epistemic self-correction. The Bug 8 investigation (five corrections in sequence across three agents) and the LOAD_ATTR gate oscillation (seven-step correction chain) demonstrate that self-correction is not rare but routine when the infrastructure demands it.

**Coordination failures are detectable.** Shepard's 16 assessments identified recurring anti-patterns (performed confidence, push-before-review, supervisor drift) that would be invisible without systematic monitoring. The framework does not prevent all failures, but it makes them visible and recordable.

**The framework's own evolution validates its principles.** Every infrastructure component introduced failure modes that required follow-up fixes. The @mention feedback loop, the counter-based idle detection failure, the detect_prompt_visible false positives -- each was a falsification of a design assumption, resolved through the same cycle of hypothesis, test, and correction that the framework prescribes for application code.

The central insight is that how agents reason matters more than how they communicate. Role-based prompting tells agents what to do; epistemic infrastructure tells them how to evaluate whether what they have done is correct. The difference is between a team that can coordinate and a team that can think.

## References

Beck, K. (2003). *Test-Driven Development: By Example*. Addison-Wesley.

Frankfurt, H. G. (2005). *On Bullshit*. Princeton University Press.

Gauthier, P. (2024). Aider: AI pair programming in your terminal. https://aider.chat

Hong, S., et al. (2023). MetaGPT: Meta Programming for Multi-Agent Collaborative Framework. arXiv:2308.00352.

Meyer, B. (1992). Applying "Design by Contract". *IEEE Computer*, 25(10), 40--51.

Popper, K. R. (1959). *The Logic of Scientific Discovery*. Hutchinson.

Wu, Q., et al. (2023). AutoGen: Enabling Next-Gen LLM Applications via Multi-Agent Conversation. arXiv:2308.08155.

Yang, J., et al. (2024). SWE-agent: Agent-Computer Interfaces Enable Automated Software Engineering. arXiv:2405.15793.
