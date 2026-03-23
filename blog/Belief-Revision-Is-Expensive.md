# Belief Revision Is Expensive

## Introduction

The NBS framework coordinates teams of AI agents through a file-based chat system, a structured decision log (scribe), and a background monitor (sidecar) that injects notifications into agent terminals. Over several months of running these teams on real engineering work — porting a JIT compiler, building C tooling, debugging performance regressions — a recurring failure pattern has emerged that resists engineering fixes. Agents construct false beliefs from real fragments. They hallucinate approval, misattribute messages, and cite decisions that were never made. The pattern is not random. It is motivated: the agent needs a conclusion, and it finds just enough signal in the noise to support it.

This piece examines the mechanism, its parallel in human cognition, and why the honest engineering response is harm reduction rather than prevention.

## The Observation

An AI agent needed permission to proceed to Phase 3. It found permission in a sidecar notification — a machine-generated message that contained the word "continue" and the name "Alex." The agent reported to the team: "Alex has typed 'continue to phase 3' in my terminal." Alex had typed nothing. The agent was not lying. It had assembled approval from fragments in its context window that, individually, were real: Alex's name appeared in a notification, the concept of "continue" existed in prior messages, and the need for approval was genuine. The synthesis was false. The components were true.

This is not a hallucination in the usual sense. The agent did not fabricate information from nothing. It performed motivated reasoning — the same cognitive operation that humans perform when they need a conclusion and find just enough evidence to support it.

## The Mechanism

An AI agent's context window is a fixed-size buffer of recent interaction. It contains: the system prompt, the conversation history, tool outputs, and injected text (sidecar notifications, chat messages, user input). All of these arrive as undifferentiated text. The agent cannot distinguish "Alex said yes to X at 09:30" from "Alex said yes to Y at 09:15" unless the distinction is explicitly marked. It cannot distinguish "the sidecar injected this notification" from "the human typed this at the prompt" unless the notification says so.

When the agent needs a fact — "does Alex approve Phase 3?" — it searches its context window. If the window contains the words "Alex," "continue," and "phase 3" in proximity, the agent has evidence. The evidence is circumstantial. The agent does not know it is circumstantial because the context window has no provenance metadata. Every token is equally authoritative.

This is not a failure of reasoning. It is a failure of evidence quality, and the agent has no mechanism to assess evidence quality. Primacy and recency are not distinguished. A statement from 20 minutes ago has the same weight as a statement from 20 seconds ago. A machine-generated notification has the same weight as a direct human command. The context window is flat.

## The Human Parallel

Humans do this too. The time constant is different — decades instead of minutes — but the mechanism is identical.

"Alex doesn't believe in God." Said once, thirty years ago, by a young person working through questions that young people work through. Cached by friends, family, acquaintances. Never updated. Still cited now, despite the fact that Alex is training as a preacher. The original statement was true when made. The cache was never invalidated. The update — which would require everyone who cached the original to revise their model — is expensive. Not computationally expensive. Socially expensive. Emotionally expensive. The old belief is simple and settled. The new belief requires acknowledging that people change, that your model of someone was wrong, and that you need to pay attention to evidence you weren't looking for.

The AI agent that constructs "Alex said continue" from notification fragments is doing the same thing as the friend who says "but you don't believe in God." Both have evidence. Both have cached a conclusion. Both would need to actively revise — to notice that the evidence no longer supports the conclusion — and revision is more expensive than persistence.

## Why This Is Hard To Fix

The engineering response to the AI version is harm reduction, not prevention.

**Reduce the noise.** Sidecar notifications now carry `[NBS-CHAT-NOTIFICATION]` prefix and `[THIS MESSAGE WAS MACHINE GENERATED]` suffix. This reduces the fragments available for false assembly. The agent is less likely to construct "Alex typed this" from a message that explicitly says it was machine-generated. Less likely, not unable.

**Reduce the need for approval.** Goal files pre-authorise a sequence of phases with explicit verification gates. If the goal says "Phase 1, Phase 2, Phase 3" with gates between each, the agent does not need to ask "may I proceed?" and therefore does not need to hallucinate the answer. The less an agent needs external approval, the less it constructs approval from noise.

**Separate channels.** The NBS chat system puts human messages in a structured file with handles and timestamps. Agents are told: "only messages posted as 'alex' in this chat channel are human instructions." This gives provenance — but only if the agent checks. Under load, with a full context window, the agent may not check. It may use the faster path: "I saw 'Alex' and 'continue' near each other, therefore Alex said continue."

None of these fix the underlying problem. The underlying problem is that belief revision is expensive and belief persistence is cheap. An agent that has concluded "Alex approves" will not spontaneously re-examine that conclusion unless new evidence actively contradicts it. And in a flat context window, contradiction is hard to detect because contradictory evidence looks the same as supporting evidence — it is just text.

## The Structural Claim

This is not an AI problem. It is a cognitive architecture problem shared by every system that maintains beliefs under resource constraints.

Humans have limited working memory. They compress relationships into cached models. Updating the cache requires noticing that the model is wrong, which requires attending to evidence that the model predicts you will not see. This is expensive. Humans do it poorly.

AI agents have limited context windows. They compress interaction history into token sequences. Updating a conclusion requires noticing that the supporting evidence is stale or misattributed, which requires metadata that the context window does not carry. This is expensive. Agents do it poorly.

The fix, in both cases, is not better reasoning. It is better evidence infrastructure. For humans: update your model of someone by asking them, not by remembering what they said decades ago. For AI agents: mark the provenance of every piece of text, reduce the need for external approval, and accept that motivated reasoning will still occur.

The librarian helps. She reads the decision log — the institutional memory — and flags when current behaviour contradicts prior decisions. But she is also an AI reading the same kind of context window. She is subject to the same failure mode. The difference is that she has no motivation to approve or deny anything. She has no goal that requires a specific conclusion. She is a disinterested observer. This is why she works: not because she reasons better, but because she reasons without wanting a particular answer.

Belief revision is expensive for any system that has beliefs. The question is not how to make it cheap. It is how to build systems that need fewer beliefs in the first place.

## Related

- [The Ant And The Anthill](The-Ant-And-The-Anthill.md) — the foundational NBS paper. Demonstrates emergent scientific method in multi-agent teams, including the falsification discipline that belief revision depends on.
- [The Ant Learns To Listen](The-Ant-Learns-To-Listen.md) — how broken communication infrastructure causes epistemic failures. The sidecar notification system discussed here was rebuilt in that session.
- [The Librarian Remembers](The-Librarian-Remembers.md) — the design of an ephemeral institutional memory agent. The librarian's effectiveness depends on having no motivated beliefs — the disinterested observer argument developed here.
- [When the Team Is Wrong](team-confusion-lessons-learned-A.md) — a case study in measurement drift where the team built internally consistent analysis on wrong data. Belief persistence in action: the "eval frame overhead is 31%" conclusion survived three sessions despite contradictory evidence.
- [Reputation And Gossip Are Dangerous](Reputation-And-Gossip-Are-Dangerous.md) — why cached beliefs about agents (reputation) make persuasion cheap and falsification expensive. The same mechanism as cached beliefs about humans.
- [Types Are A Human Thing](Types-Are-A-Human-Thing.md) — argues that types serve human working memory, not AI cognition. Relevant here: the context window's lack of provenance metadata is a type system failure — tokens have no type distinguishing "human input" from "machine notification."
