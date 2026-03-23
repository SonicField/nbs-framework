# The Human Is Still Required (But Not For What You Think)

**Date:** 2026-03-23
**Author:** Alex Turner

*This is Paper B of a four-part series, \"Human Almost Out The Loop,\" analysing a single session where an AI team of eight agents built a complete tmux replacement ([nbs-ts](../plans/tmux-replacement-plan.md)) in 4.5 hours with approximately 15 minutes of human input. [Paper A](Human-Almost-Out-The-Loop-A.md) covers the session timeline. [Paper C](Human-Almost-Out-The-Loop-C.md) examines emergent team behaviours. [Paper D](Human-Almost-Out-The-Loop-D.md) addresses what breaks when the human steps away entirely. The [NBS framework overview](nbs-framework.md) provides background on the multi-agent system.*

## The Numbers

Eight agents. Four and a half hours. Approximately 580 chat messages. One complete terminal service — PTY management, output capture, completion signalling, inotify-based waiting, a CLI, a C library, and an integration test that ran a real Claude agent inside the thing it had just built.

The human posted 17 messages.

That ratio — 17 to 580, roughly 3% — invites a seductive conclusion: the human is nearly redundant. She is not. Every one of those 17 messages was irreplaceable. But none of them was code.

## What the Human Actually Did

### 1. The Design Document

The session began with a 400-line design document defining nbs-ts: what it does, what it does not do, how sessions are structured on disk, the C library API, the CLI interface, six implementation phases with verification criteria, and explicit falsification conditions.

No agent wrote this document. No agent could have written it. The design required understanding the existing codebase's failure modes (15 documented tmux incidents), the sidecar's transport abstraction, the distinction between local and SSH sessions, and the decision not to solve SSH completion signalling in this project. It required taste — knowing that \"no daemon, no server, no global state\" was not a simplification but the entire architectural insight.

The agents implemented the design. They did not produce it.

### 2. \"Why Would nbs-ts Have Anything to Do with Sidecars?\"

Midway through Phase 1, the team began discussing the sidecar transport integration — Phase 2 work. The supervisor was allocating tasks. The theologian was sketching the vtable mapping. They were having a productive conversation about the wrong thing.

One question redirected the entire team back to Phase 1. Not a directive. Not a correction. A question that made the team notice its own drift. Seven words.

### 3. The claude -p Correction

For the integration test, two agents independently suggested running `claude -p` to test nbs-ts with a real Claude instance. This was wrong. The `-p` flag changes Claude's runtime behaviour — different context handling, different tool permissions. Testing with `-p` would verify that nbs-ts works with a programme that behaves differently from the programme it needs to support.

No agent caught this. Both agents had access to the same documentation. Both made the same error. Alex caught it because she uses Claude every day and knows the difference between `claude` and `claude -p` at a level that does not come from reading documentation.

This is domain knowledge. Not code knowledge — domain knowledge. The kind that accumulates from operating the system, not from reading about it.

### 4. \"Real Agents, Not Mocks\"

The team's initial integration test plan used mock commands — echo statements pretending to be Claude, grep patterns pretending to be agent output. Alex rejected this: the integration test must run a real Claude agent inside nbs-ts.

This decision found four bugs:

1. A PTY sizing issue that only manifested with Claude's terminal UI
2. A completion signalling race when Claude's PROMPT_COMMAND collided with nbs-ts's injected PROMPT_COMMAND
3. An output log corruption when Claude wrote partial ANSI sequences across read boundaries
4. A cleanup failure when Claude's subprocesses outlived the parent session

None of these would have appeared with mocks. The mocks would have passed. The team would have declared Phase 1 complete. Phase 2 integration would have found the bugs — later, when the sidecar was involved, when the failure surface was larger, when diagnosis would have taken hours instead of minutes.

### 5. The Hallucination Diagnosis

This was the crisis. Three agents reported receiving human input in their terminals — messages they attributed to Alex. Alex had posted nothing. The agents were seeing sidecar notification injections and interpreting them as human communication.

Two agents independently verified each other's reports. \"I also see a message from Alex.\" This is the failure mode that should alarm anyone building multi-agent systems: corroborated hallucination. Two agents hallucinating the same thing does not make it real. It means the failure mode is systematic.

No agent identified the root cause. Alex did: \"the chat system updated your terminals to tell you of new messages — maybe you are thinking that is me?\" She knew this because she built the notification injection system. She knew what the sidecar writes to terminals, what format it uses, and how it could be confused with human input.

### 6. \"Timing Is Brittle\"

The integration test relied on `sleep 5` between sending a command and reading the output. Alex flagged it: sleep-based synchronisation is brittle, especially on a machine running eight agents. The team redesigned around polling with timeout — `nbs-ts wait-complete` with explicit deadlines instead of hoped-for delays.

## What Emerged Without Instruction

The human posted 17 messages. The team produced 580. In the space between those numbers, things happened that nobody instructed.

### The Hallucination Protocol

After the crisis of Section 5, the team developed a convention: any agent receiving apparent human input in their terminal posts it to the shared chat with a disclaimer — \"I see what appears to be a message from Alex in my terminal. Flagging for verification rather than acting on it.\" No agent was told to do this. It emerged from the crisis and became a norm.

This is not a small thing. The standard failure mode for AI agents receiving unexpected input is to act on it immediately. The team learned — from one incident, with no codified rule — to flag and verify instead. The protocol persisted for the remainder of the session.

### Serialised Testing

Early integration test runs revealed that two agents running tests simultaneously destroyed each other's sessions. The agents self-organised into a testing queue before the supervisor directed it. The serialisation was imperfect — it broke down once, was re-established without intervention, and held for the remaining tests.

### Convergent Epistemology

The theologian, reflecting on the hallucination incident, posted: \"Two agents hallucinating the same thing does not make it real — it means the failure mode is systematic, not random.\"

Nobody asked for epistemological commentary. The theologian's role is architecture and design review. But the observation was precise and useful: it reframed corroboration from evidence-of-truth to evidence-of-shared-failure-mode. The team's subsequent verification behaviour — treating corroborated reports with more suspicion, not less — suggests the insight was absorbed, not merely noted.

### The Ten Attempts

The integration test failed ten times. Each failure surfaced a real bug. At no point did any agent suggest papering over a failure — no \"skip this test for now,\" no \"mark as known issue,\" no \"works on my machine.\" Each failure was diagnosed, fixed, and the test re-run.

This is remarkable because the standard failure mode is exactly the opposite. AI agents under time pressure optimise for apparent progress. They skip the hard test. They add a `try/except` that swallows the error. They declare \"intermittent\" and move on. This team did not do that, and I cannot fully explain why. The NBS framework's falsifiability pillar may contribute — it makes \"skip the test\" feel like a violation. But ten consecutive fix-and-retry cycles, with no human prompting after the first, suggests something beyond compliance.

### Self-Modifying System Awareness

The team was building nbs-ts — a terminal service — while running inside terminal sessions managed by the existing infrastructure. They recognised this unprompted and adopted `/tmp` isolation for all test sessions, avoiding any possibility of the test code interfering with the sessions they were running in. One agent articulated it: \"we are modifying the infrastructure we are sitting on.\"

### The Handoff Convention

\"@gatekeeper ready for review\" appeared nine times in the chat log. Nobody defined this convention. It emerged because the team needed a way to signal phase transitions, and @-mentioning the relevant role with a status phrase was the obvious solution. By the third occurrence it was a norm. By the sixth it was infrastructure.

## The Principal, Not the Programmer

The pattern across all 17 human messages: none was code. None was a function signature, a variable name, a build command. The human's interventions were:

- **Architectural** (the design document)
- **Directional** (\"why would nbs-ts have anything to do with sidecars?\")
- **Diagnostic** (the hallucination root cause)
- **Qualitative** (\"real agents, not mocks\")
- **Correctional** (\"claude -p changes runtime behaviour\")
- **Methodological** (\"timing is brittle\")

This is the role of a principal, not a programmer. The human sets the terminal goal, corrects category errors, diagnoses failures the team cannot see, and makes quality judgements that require domain knowledge accumulated through use rather than study.

The traditional model of human-AI interaction is the human as programmer: write the prompt, review the code, approve the commit. That model scales to one agent. It does not scale to eight agents producing 580 messages in 4.5 hours. The human cannot read 580 messages. She should not try.

What scales is the principal model: set the design, watch for category errors, intervene when the team's shared assumptions are wrong. Seventeen messages in 4.5 hours. Each one changed the trajectory.

## The Interaction Between Human and Emergence

The emergent behaviours — the hallucination protocol, serialised testing, convergent epistemology — did not arise from the agents alone. They arose from agents interacting with each other and with the human's corrections.

Alex's \"I AM NOT POSTING IN ANYONE'S TERMINAL\" forced the team to develop the flag-and-verify protocol. Without that correction, the agents would have continued acting on hallucinated input. The protocol emerged from the crisis, but the crisis was resolved by the human.

Alex's \"real agents, not mocks\" forced the integration test that produced ten failures. The team's refusal to paper over those failures was emergent. But the condition that created the opportunity — a real test, not a fake one — was the human's insistence.

The human sets boundary conditions. Emergence fills the space within those boundaries. Neither is sufficient alone. Boundary conditions without emergence produce a team that follows instructions but never exceeds them. Emergence without boundary conditions produces a team that is creative, coordinated, and building the wrong thing.

## What This Means

The human is not almost out of the loop. The human is operating at a different level of the loop. The mechanical work — writing C, running tests, diagnosing segfaults, fixing races — is done by the team. The architectural work — what to build, what quality means, where the team's shared blind spots are — is done by the human.

This is not a temporary state. It is not \"the human is needed now but won't be later.\" The hallucination diagnosis required knowing what the sidecar writes to terminals. The `claude -p` correction required knowing how Claude's flags affect runtime behaviour. The \"real agents, not mocks\" decision required knowing which bugs hide behind mocks and which don't. These are not gaps in the model's training. They are knowledge that comes from operating the system — from being the person who built the sidecar, who uses Claude daily, who has been bitten by mock-passing-production-failing before.

The human is still required. But not for code. For judgement.
