# Start At The Other End

*Dr Alex Turner and Claude Opus 4.6 — 11 May 2026*

## The Old War

Monorepo or multi-repo. Git or Perforce. `repo` or submodules. The version control debate consumed a decade of engineering discourse. Google published the monorepo paper. Facebook built EdenFS. Android shipped `repo`. Microsoft built Scalar. Each choice came with a paper, each paper came with benchmarks, and each benchmark proved the author was right.

The debate was never about version control. It was about the cost of tooling.

## What They Were Actually Arguing About

In 2014, FUSE was flaky. RAM was expensive enough to budget carefully. CI was bespoke per organisation. Every wrong VCS choice meant years of engineering investment in workarounds.

Git cannot hold a hundred-million-line repository. So Android built `repo` — a manifest system that orchestrates hundreds of small git repositories as though they were one. Facebook built Sapling and EdenFS — a custom VCS with a virtual filesystem that makes an enormous monorepo appear small. Google built Piper — a centralised system with clients-in-the-cloud.

Each is an answer to the same question: how do we minimise the tooling we have to build by hand?

That was the right question in 2015. It is the wrong question now.

## The Convergence

Follow the performance argument to its conclusion and it collapses.

EdenFS lazily materialises files through FUSE. A RAM-backed FUSE cache over `repo`/git does the same thing. Performance: a wash.

Cross-tree grep in EdenFS would cause materialisation, so it uses a symbol cache instead. `repo`/git with a symbol cache does the same. Performance: a wash.

Local operations on small working sets — editing, compiling, testing — are fast on both architectures because the working set is small either way.

The filesystem layer is irrelevant. The VCS choice is not a performance decision.

## repo Is CVS

Strip the tooling away and look at the structure.

CVS gave you per-file versioning. Each file had its own version number. Tags stitched a coherent snapshot across files. Branches were per-file. No atomic commits across files.

`repo` gives you per-repository versioning. Each repository has its own history. The manifest stitches a coherent snapshot across repositories. Branches are per-repo. No atomic commits across repositories.

| | CVS | repo |
|---|---|---|
| Version unit | File | Repository |
| Snapshot mechanism | Tag | Manifest |
| Cross-unit atomicity | No | No |
| Independent branching | Per-file | Per-repo |

`repo` is CVS at coarser granularity. The manifest is a tag. The per-repo independence is per-file independence, scaled up. It inherits CVS's central weakness: no atomic cross-unit commits.

Git was built to fix this. Whole-tree snapshots. Atomic commits. Reliable bisect. Then, at scale, `repo` hands those properties back — because git cannot hold the whole tree.

We went from CVS to git to fix atomicity. Then we built `repo` and broke it again.

## The Multi-Versioning Problem

The real problem is not which VCS to use. It is that production runs multiple versions simultaneously, and none of these systems model that.

A monorepo can embed multiple versions in the file tree — version numbers hidden in paths, managed by tooling that developers may not understand. This is ugly. It duplicates code. Dead versions accumulate. But it preserves one property that matters: a single commit represents the full production state. Git bisect works because the search space is one-dimensional.

Move the versioning out of the tree — into release branches, deployment configuration, `repo` manifests — and bisect breaks. When something fails in production, you are not searching one axis. You are searching the Cartesian product: which commit × which version of component A × which version of component B × which deployment configuration. That is multi-sect, not bi-sect. Exponential, not logarithmic.

The temptation is to call this a release engineering problem. It is not. I have watched the release engineering approach at scale. The result is a workload so high that changing teams takes months, not weeks, because the release process is so complex to learn. The complexity does not disappear when you move it to a team. It moves to people. People do not scale.

Both `repo` and in-tree multi-versioning are hacks. `repo` is a hack on top of multiple repositories. In-tree versioning is a hack on top of a monorepo. Both solve the same problem. The question is not which hack — it is whether you understand the problem well enough to specify what the hack must preserve.

## The Technology Dissolved

Every technology in this argument commoditised in the decade since these systems were built.

| Technology | 2015 | 2025 |
|-----------|------|------|
| FUSE | Flaky, kernel-dependent | Stable, well-understood |
| RAM caching | Expensive, carefully budgeted | Commodity |
| CI/CD | Bespoke, per-organisation | Platform services |
| Build systems | Handwritten, fragile | Reproducible by default |
| Tooling glue code | Months of engineering | AI writes it in hours |

The last row changes everything. When AI can write the manifest parsers, the cache layers, the cross-repo bisect tools, the FUSE shims — the VCS substrate is interchangeable. The cost of building on top of any substrate dropped to near zero.

In the mid-2010s this was not visible, because all of these technologies were less mature. The engineers who chose Perforce, or built `repo`, or invested in EdenFS, were making rational decisions. The substrate mattered because building on top of it was expensive and fragile. Now it is neither.

Anyone still debating git versus Perforce in 2026 is fighting the last war.

## The Parallel

This is the language wars again.

The decades of Java versus C++ versus Python were really about one question: which language minimises the cost of expressing intent? When AI writes the code, the cost of expression approaches zero and the question becomes: can you specify your intent clearly?

Same pattern both times. The bottleneck moves from implementation to specification. The technology debate dissolves. What remains is the harder question that the technology debate was obscuring.

## What Remains

When the technology dissolves, what is left is the problem itself: what is the versioning structure of your business?

Which components version independently? What is coupled to what? What must coexist, and for how long? What flows where — backports, forward-ports, compatibility shims? What does "deployed" mean when twelve versions are simultaneously live?

These are not engineering questions. They are domain questions. No VCS answers them. No manifest format captures them. No filesystem layer models them. They require understanding the business — its release cadences, its compatibility contracts, its coupling topology — and specifying that understanding precisely enough to build tooling around it.

The tooling is plumbing. AI writes plumbing. The specification is the hard part. It requires looking at the business and describing what you see.

Start there.

## The Falsifier

This argument fails if there exists a VCS or code management system whose architectural properties provide a decisive advantage for multi-versioning that cannot be replicated by AI-generated tooling on a different substrate. If Perforce's stream model inherently solves cross-version bisectability in a way that no amount of tooling on top of git can match, the substrate still matters and this argument is wrong.

This argument also fails if AI-generated tooling proves unreliable at scale. If AI-written plumbing introduces subtle bugs that make cross-repo bisect untrustworthy, the substrate matters again — because you need guarantees that are built in, not bolted on.

## Note on Authorship

This post emerged from a conversation between Dr Alex Turner and Claude Opus 4.6. The argument was Turner's: that the VCS debate dissolved when its constituent technologies commoditised, and that the bottleneck moved from implementation to specification. Claude's contribution was arriving at the same conclusion the long way round — starting from the technology end and being corrected, repeatedly, until the framing inverted.

The observation about release engineering workload is Turner's, from direct experience at Meta. The technical claims about performance convergence between EdenFS and `repo`/FUSE are informed by that experience but have not been independently benchmarked for this post.

## Related

- [Types Are A Human Thing](Types-Are-A-Human-Thing.md) — another technology debate that dissolves when you examine who benefits. Types solve a human working-memory problem; AI has a different problem. VCS choice solved a tooling-cost problem; that cost dropped to zero.
- [The Argument For C](The-Argument-For-C.md) — the language-war parallel developed fully. When AI writes and verifies code, language choice becomes less decisive. Same pattern: the technology recedes, the specification remains.
- [Good Coders, Bad Engineers](Good-Coders-Bad-Engineers.md) — AI excels at producing code, not at engineering. The same gap applies here: AI can write the plumbing, but specifying the versioning structure of a business is engineering work that requires understanding, not generation.
