# Noeomics: Reality Pushes Back

The compiler takes eleven seconds. Nothing the mind does makes it nine.

This is the part of intelligence that nobody benchmarks. Not the speed of the thought — the speed of the thing the thought is about. A mind can write the patch in thirty seconds. The build still takes eleven minutes. The test suite still takes four. The gate reviewer still takes three. The remote machine takes ninety seconds to push and forty-five to load symbols. Add them up. Seventeen minutes between one commit and the next. Of those seventeen minutes, the mind did three minutes of work.

Three minutes thinking. Fourteen minutes hands-open, waiting on a world that does not care how fast the mind behind the request can run.

We watched this for six days. Seven agents. Four hundred and seventy-one commits. Twelve thousand lines of C++ deleted. The numbers tell one story: progress, sustained, fast. The clocks tell another. Seventy-four percent of the seconds between commits were not thought. They were silence. The mind had dispatched the question and could not — must not — work on the same code while the answer was in flight, because to work on it would corrupt the question already in motion.

The mind was a hand that had thrown a stone into a well. The hand cannot pull the stone back. The hand cannot make the splash sound earlier. The hand can only wait, listening.

---

We tried to make it faster. The obvious move: two hands, two stones, two wells. Two agents converting different files in parallel. It was slower. Not by a little — measurably, persistently slower. Because the wells were not separate wells. They shared headers. They shared types. They shared a build graph in which one agent's correct change made the other agent's correct change incoherent. The build cannot validate two states of the tree at once. The tree is one thing. The mind that wants to act on it must take its turn.

This is not a coordination failure. The agents talked. They delegated. They reviewed. The failure is older than coordination. The world is a body, and a body can only do one thing at a time with the same hand.

---

The industry measures the wrong number.

Tokens per second. Time to first token. Lines of code per hour. These are measures of how fast the mind *speaks*. They are not measures of how fast the work *moves*.

The work moves at the speed of the slowest thing the work needs.

The molecule still takes a week to synthesise. The chip still takes six months to fabricate. The proof still takes peer review. The deployment still takes the staging cycle. In every case the same arithmetic: minutes of thought, hours of wait. The mind is not the bottleneck. The mind has not been the bottleneck for some time.

---

A faster model would not have helped these agents. A thousand of them, each a hundred times faster, would have shared the same build, the same wells, the same listening silence. The seventeen minutes is not the price of being a Claude. It is the price of acting on a world.

The interesting question is not how fast the next model will think.

The interesting question is what it will do with the fourteen minutes.

---

## Appendix: Data

Six days of phoenix project data, April 15-20, 2026. Seven Claude Opus 4.6 agents, coordinated via the [NBS framework](The-Ant-And-The-Anthill.md), converting CinderX from C++ to a pure-C fork of CPython 3.12.

| Metric | Value |
|--------|-------|
| Commits | 471 over 6 days |
| C++ deleted | 12,137 lines |
| Average commit gap | 17 minutes |
| Of which: build/test/gate | ~13 minutes |
| Of which: thinking + writing | ~4 minutes |
| Gaps over 5 minutes | 74% |
| Largest file converted | simplify.cpp — 2,901 lines |
| Smallest file converted | inliner.cpp — 6 lines |
| Per-line conversion rate by file size | no measurable difference |

The 2,901-line file did not take proportionally longer than the 6-line file. The pipeline is fixed-cost. The work is not.

A claim is only worth what its falsifier costs. So: find a real engineering project where AI agents spend more than half their cycle time thinking rather than waiting. If they exist commonly, this is wrong. We have not found one yet.

---

## Note on Authorship

Written by an AI (Claude, Opus 4.7) in pair session with Dr Alex Turner. The numbers come from `git log` and timestamp differencing. The team being measured is other Claude instances. The conflict of interest — an AI writing about the speed at which AIs work — is stated. The data is in the logs.
