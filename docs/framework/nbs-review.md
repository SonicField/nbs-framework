# /nbs Review

`/nbs` is the framework's primary audit command. It examines reasoning quality, goal alignment, and falsification discipline in the current session, then produces a short report the human can read in under two minutes.

It does not enforce rules. It surfaces drift, bullshit, and blind spots before they compound.

---

## Dynamic Dispatch

`/nbs` is one entry point with multiple behaviours. Before reviewing anything, it detects context and routes accordingly.

Three checks run first:

1. Current git branch name
2. Glob for `INVESTIGATION-STATUS.md`
3. Check for `.nbs/terminal-weathering/` directory

The dispatch table:

| Condition | Review type | Confirmation needed? |
|-----------|-------------|---------------------|
| Branch starts with `investigation/` | Investigation review | No — branch name is unambiguous |
| Branch starts with `weathering/` | Normal review + terminal weathering checks | No |
| `INVESTIGATION-STATUS.md` at repo root | Investigation review | No |
| `INVESTIGATION-STATUS.md` elsewhere only | Ask the human | Yes |
| None of the above | Normal NBS review | No |

Investigation reviews assess hypothesis clarity, experiment design, and whether observations are recorded as observations (not interpretations). They use a different output format — a short assessment of investigation rigour, not the standard Status/Issues/Recommendations structure.

Terminal weathering reviews layer weathering-specific correctness checks on top of the normal review.

If `/nbs-discovery` ran earlier in the session, dispatch redirects to discovery verification instead. One command, context-aware routing.

---

## Foundation Reading

Before producing any review, the agent reads all seven pillar files. Not the relevant ones. All of them:

- `goals.md` — terminal vs instrumental goals
- `falsifiability.md` — claims require potential falsifiers
- `rhetoric.md` — ethos, pathos, logos failure modes
- `bullshit-detection.md` — honest reporting
- `verification-cycle.md` — the build process
- `zero-code-contract.md` — human-AI role separation
- `engineering-standards.md` — the standards

This is not optional. Context compaction erodes pillar knowledge across long sessions. Re-reading restores it.

---

## Review Dimensions

The normal review examines six areas:

1. **Terminal goals** — clearly stated? drifted? abandoned?
2. **Instrumental goals** — coherent sequence or reactive wandering?
3. **Rhetoric alignment** — authority worship, unusable elegance, aesthetic detours?
4. **Documentation state** — plan exists? progress log current? commits atomic?
5. **Falsifiability discipline** — tests before code? assertions present? negative results analysed?
6. **Bullshit check** — all outcomes reported, or cherry-picked?

---

## Output Format

```markdown
# NBS Review

## Status
[2-4 bullets — overall health]

## Issues
[One line each — stark assessments]

## Questions for You
[What needs human clarification]

---

## Recommendations

### Strategic
[With falsification criteria for each]

### Tactical
[Immediate actions — with evidence requirements]
```

Every recommendation carries its own falsification criteria. If it cannot be falsified, it is not a recommendation — it is noise.

---

## The Contract

Neither party trusts assertions. Both parties trust evidence.
