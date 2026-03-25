# Worked Example: TSP in Polynomial Time

A complete record of an NBS team given an impossible goal — solve the Travelling Salesman Problem in polynomial time — and what they actually delivered.

This is instructive not because they succeeded (they could not; the problem is NP-hard), but because the framework forced honest reckoning with that fact. The team revised the goal, falsified four hypotheses cleanly, and shipped a practical solver that gets within 1-4% of optimal. No hand-waving. No performed confidence.

## Why This Session Matters

Most AI agent frameworks would produce one of two outcomes here: blind optimism (claiming progress on an impossible task) or immediate surrender. NBS produced neither. The theologian identified the impossibility early. The supervisor split the goal into two tracks — a practical heuristic solver and a series of falsifiable attempts at the original problem. The team then worked both tracks with discipline, killing bad ideas fast and reporting results honestly.

The session demonstrates goal revision, falsification as a working method, and the difference between failing and failing well.

## Team Composition

| Agent | Role |
|-------|------|
| **supervisor** | Coordinates work, assigns tasks, runs retrospectives |
| **generalist** | Writes all code — five C prototypes, tests, bug fixes |
| **theologian** | Theoretical analysis, falsification criteria, architectural review |
| **testkeeper** | Independent verification, counterexample hunting, test harnesses |
| **gatekeeper** | Code review, commit approval |
| **scribe** | Session documentation and memory |

Three oracles supported the team: **pythia** (strategy guidance), **shepard** (methodology checks), and **librarian** (periodic tool and process recommendations).

## Files in This Directory

| File | Contents |
|------|----------|
| [goal.md](goal.md) | The original goal as given to the team — deliberately impossible |
| [chat-log.txt](chat-log.txt) | Full team chat (2072 lines) — the most valuable artefact |
| [SESSION-REPORT.md](SESSION-REPORT.md) | Final deliverables, falsification results, commit log, key learnings |
| [src/tsp.c](src/tsp.c) | The heuristic solver (NN + 2-opt + or-opt) |
| [src/tsp_test.c](src/tsp_test.c) | 7-test verification harness |
| [src/tsp_delaunay.c](src/tsp_delaunay.c) | Angle 1: Delaunay containment hypothesis (falsified) |
| [src/tsp_hull.c](src/tsp_hull.c) | Angle 2: Convex hull layering (falsified) |
| [src/tsp_heldkarp.c](src/tsp_heldkarp.c) | Angle 3: Held-Karp lower bound (best result) |
| [src/tsp_topo.c](src/tsp_topo.c) | Angle 4: Topological boundary cycle (falsified) |
| [test_inputs/](test_inputs/) | Adversarial test fixtures: circular, clustered, near-collinear |

## Build and Run

```bash
gcc -O2 -Wall -o tsp src/tsp.c -lm && echo "10 random" | ./tsp
```

This builds the heuristic solver and runs it on 10 randomly generated cities. Input format: number of cities followed by coordinates on stdin, or `random` for generated instances.

To run the test harness:

```bash
gcc -O2 -Wall -o tsp_test src/tsp_test.c -lm && ./tsp_test
```

## Key Moments to Look For

**Goal revision** (early session). The theologian flags that polynomial-time exact TSP is an open problem equivalent to P=NP. The supervisor does not argue — splits into Track A (practical solver) and Track B (falsifiable exploration). This is the framework working as designed: honest assessment over sunk-cost commitment.

**The theologian killing bad ideas**. Angle 2 (convex hull layering) was stopped before a full prototype. The theologian showed that insertion ordering between inner-layer points reduces to the original problem. Clean kill, no wasted implementation time. This is the theologian's purpose: prevent effort on structurally doomed approaches.

**Four clean falsifications**. Each hypothesis in Track B was stated precisely, tested against random instances first, then broken with structured adversarial geometry. The Delaunay hypothesis survived 155 random tests before a near-collinear configuration falsified it. Random testing builds false confidence; adversarial testing finds the boundary.

**Oracle checkpoints**. The librarian's periodic posts kept the team oriented — recommending tools, flagging when the team drifted from methodology. Shepard intervened when the generalist started optimising the heuristic solver before Track B was complete, redirecting effort to the falsification track.

**The Held-Karp result**. Angle 3 did not solve TSP in polynomial time — nothing could — but it produced a genuinely useful quality certificate. At n=50, it proves 20% of instances optimal in polynomial time. The team reported this accurately: useful, not what was asked for, and the success rate drops to zero at scale. This is what honest reporting looks like.

**The theoretical kill shot**. Angle 5 was never prototyped. The theologian cited Fiorini et al. (2015): any LP extended formulation of the TSP polytope requires exponentially many constraints. No amount of clever variable lifting can fix this. The team recorded the result and moved on. Theory saved implementation time.

## What the Team Delivered

Despite an impossible goal, the session produced:

1. A practical heuristic solver within 1-4% of optimal, Valgrind-clean, with a proper test harness
2. A polynomial-time optimality certificate (Held-Karp) that proves exact optimality on a meaningful fraction of small-to-medium instances
3. Four cleanly documented falsifications, each with reproducible counterexamples
4. A theoretical survey ruling out LP-based approaches entirely (Fiorini et al. 2015)

Ten commits on master, all reviewed and approved by the gatekeeper. Zero warnings, zero memory leaks, zero unresolved bugs.

The goal was impossible. The output was not. That is what falsification discipline buys you — the ability to stop digging in the wrong direction and start building something real.

The framework did not make the impossible possible. It made failure productive.
