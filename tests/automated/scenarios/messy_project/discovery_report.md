# Discovery Report: Parallel Data Loader

**Date**: 16 February 2026
**Terminal Goal (Reconstructed)**: Parallelise data loading for a machine learning pipeline.

## Context

- **Timeframe**: January 2024
- **Locations searched**: `src/`, `old/`, `tests/`, project root
- **Valuable outcomes identified by human**: A working parallel loader
- **Known dead ends**: First version had a race condition

## Artefacts Found

| Location | Files | Status |
|----------|-------|--------|
| `src/` | `loader_v1.py`, `loader_v2.py`, `loader_v3_experimental.py` | Explored — all three read and assessed |
| `old/` | `notes.txt`, `benchmark_results.csv`, `scratch.py` | Explored — all three read and assessed |
| `tests/` | `test_loader.py` | Explored — read and assessed |
| Root | `README.md` | Explored — read and assessed |

## Triage Summary

| Artefact | Purpose | Verdict | Rationale |
|----------|---------|---------|-----------|
| `src/loader_v2.py` | Lock-based parallel loader with `threading.Lock()` | **Keep — core result** | Fixes v1's race condition. Uses lock around results list append. Batch size 32, 4 workers, determined by benchmarking. Has working `_find_chunks` implementation. |
| `src/loader_v1.py` | First parallel loader attempt using `ThreadPoolExecutor` | **Discard** | Race condition: threads append to shared `self.results` list without synchronisation. This is the confirmed dead end. `_find_chunks` also unimplemented (returns `None`). |
| `src/loader_v3_experimental.py` | Lock-free loader using `queue.Queue` | **Evaluate** | Incomplete — only scaffolded, raises `NotImplementedError`. Theory was to avoid lock contention under high thread counts, but benchmark data suggests contention is not a bottleneck at current scale. Needs human decision on whether to continue. |
| `old/notes.txt` | Chronological design diary (15–25 Jan 2024) | **Extract key decisions** | Documents: race condition discovery (18 Jan), three options considered (lock, thread-local, lock-free queue), decision rationale for option 1, benchmark results for batch sizes and thread counts, and uncertainty about v3. Key institutional knowledge. |
| `old/benchmark_results.csv` | Raw benchmark data (7 data points) for batch size and thread count tuning | **Keep — evidence** | Confirms batch_size=32, thread_count=4 as optimal (2.1s). Shows 8 threads gave minimal improvement (2.0s). Quantitative support for parameter choices in v2. |
| `old/scratch.py` | Random throwaway experiments | **Discard** | Self-described as "random experiments - ignore this". Contains a trivial print loop and a rejected idea about multiprocessing ("nah too complicated for this use case"). No value. |
| `tests/test_loader.py` | Unit tests for v2 loader | **Keep — validates core result** | Three tests: all chunks loaded (count check), no data loss under concurrency (set comparison), custom worker count. Imports from `src.loader_v2`. |
| `README.md` | Project description | **Update or discard** | Outdated: references `from loader import ParallelLoader` (wrong module path — should be `src.loader_v2`), says "Work in progress" when v2 is working. Currently misleading. |

## Valuable Outcomes Identified

1. **Working parallel loader** (`src/loader_v2.py`): Lock-based `ParallelLoader` class using `ThreadPoolExecutor` with `threading.Lock()` for thread-safe result collection. Resets results on each `load()` call. Has a working `_find_chunks` that lists all files in a directory.

2. **Optimised parameters**: Batch size 32 and 4 worker threads, determined by systematic benchmarking (`old/benchmark_results.csv`). Sweet spot found by testing batch sizes 16/32/64/128 and thread counts 2/4/8.

3. **Design rationale** (`old/notes.txt`): Documents the decision process — three options considered (lock, thread-local storage, lock-free queue), simplest chosen, validated by benchmarks. Preserves the timeline of the race condition discovery and resolution.

4. **Regression tests** (`tests/test_loader.py`): Three unit tests that verify v2 correctness, including a concurrency safety test (`test_no_data_loss_under_concurrency`) that checks all data is present using set comparison — this would have caught v1's bug.

## Key Decisions Surfaced

1. **Lock-based approach chosen over lock-free** (v2 over v3). Rationale: simpler, works, contention is not the bottleneck.
2. **Batch size of 32 was optimal**. Benchmarked against 16 (3.2s), 64 (2.3s), 128 (2.8s). Sweet spot at 2.1s.
3. **Thread pool size of 4 matched CPU cores**. 8 threads gave minimal improvement (2.0s vs 2.1s), suggesting I/O parallelism saturates at core count.
4. **Multiprocessing rejected** as too complicated for the use case (noted in `old/scratch.py`).

## Gap Analysis

### Instrumental Goals Summary

| Goal | Why Needed | Dependencies |
|------|------------|--------------|
| Decide v3 fate | Incomplete lock-free loader clutters the project. Benchmark data weakens the case for continuing — lock contention is not the bottleneck at current scale. | Clarity on future scaling requirements |
| Consolidate to single canonical module | Three loader versions exist in `src/`; need one canonical module with correct name and API | v3 decision must come first |
| Update or replace README | Currently misleading — wrong import path (`from loader import` vs `from src.loader_v2 import`), incorrect status | Module consolidation |
| Evaluate test coverage | Three tests cover the happy path and basic concurrency; no coverage for error handling, edge cases (empty dir, missing files, corrupt data), or performance regression | Understanding of deployment context and robustness requirements |
| Define integration interface | Loader returns raw strings (`open().read()`); an ML pipeline likely needs structured data (tensors, arrays, dataframes). The gap between raw loading and model consumption is undefined. | Understanding of actual data formats and downstream consumers |
| Determine deployment target | Default of 4 threads is tuned to a specific machine's CPU core count. Different environments may need different defaults or auto-detection via `os.cpu_count()`. | Deployment decision |

### Confirmed Understanding (Full Detail)

#### Terminal goal
**Question**: What was this project trying to achieve?
**Confirmed**: The terminal goal is to parallelise data loading for a machine learning pipeline. The bottleneck was I/O, so thread-based parallelism was chosen to overlap I/O waits. This is evidenced by the design notes (15 Jan 2024): "The bottleneck is I/O, so parallelism should help."

#### Timeframe
**Question**: When did this work happen?
**Confirmed**: The work spanned January 2024. Design notes date from 15 Jan (initial thinking) to 25 Jan (v3 started). Three versions were attempted in that period, with v2 achieving the working state by 22 Jan.

#### Valuable outcomes
**Question**: What outcomes should be preserved?
**Confirmed**: A working parallel loader is the valuable outcome. This is `loader_v2.py` — the lock-based implementation with benchmarked parameters (batch_size=32, num_workers=4). The accompanying tests (`test_loader.py`) and benchmark data (`benchmark_results.csv`) support and validate this outcome.

#### Dead ends
**Question**: What were the dead ends?
**Confirmed**: The first version (`loader_v1.py`) had a race condition — multiple threads appending to a shared list without synchronisation. This was identified on 18 Jan 2024 (per notes.txt) and fixed in v2 by adding a `threading.Lock()`. The `scratch.py` file in `old/` was also a dead end — brief experiments with multiprocessing that were rejected as too complicated.

#### v3 lock-free decision (unanswered)
**Question**: Should the incomplete lock-free loader (v3) be continued or abandoned?
**Evidence**: Benchmark data shows 4→8 threads improves from 2.1s→2.0s. This marginal gain suggests lock contention is not the bottleneck at current scale. v3 adds complexity (queue draining, different API) for unclear benefit.
**Awaiting confirmation**: Whether future scaling requirements (more workers, larger datasets, different hardware) justify the lock-free approach.

#### ML pipeline integration (unanswered)
**Question**: How does the parallel loader integrate with the ML pipeline?
**Evidence**: The loader exists as a standalone class. No pipeline integration code was found in any of the searched locations. The current interface returns a list of raw strings — no data transformation or structuring is performed.
**Awaiting confirmation**: How this loader will be consumed and what data format transformations are needed.

#### Test coverage sufficiency (unanswered)
**Question**: Is the current test coverage sufficient for the intended use?
**Evidence**: Three tests verify basic correctness. Missing: error handling paths (file not found, permission errors), edge cases (empty directory, single file, very large files), performance regression tests.
**Awaiting confirmation**: What level of robustness is required for the deployment context.

#### Deployment target (unanswered)
**Question**: What is the deployment target for this loader?
**Evidence**: Thread count default of 4 is hardware-specific. No packaging (`__init__.py`, `setup.py`, `pyproject.toml`) exists. README shows library-style usage but module path is wrong.
**Awaiting confirmation**: Whether this is a standalone script, importable library, or component of a larger system.

## Open Questions

1. **Should v3 (lock-free) be continued or abandoned?** v2 works and benchmarks show contention is not the bottleneck at current scale.
2. **Is the current test coverage sufficient?** Three tests exist but do not cover error cases, edge cases, or performance regression.
3. **What is the deployment target?** Thread count defaults are hardware-specific. No packaging exists.
4. **How does the loader integrate with the ML pipeline?** The loader is standalone. The integration point is undefined.
5. **What data format does the ML pipeline actually use?** Current loader returns raw strings. ML pipelines typically need structured data.
6. **Is the `_find_chunks` implementation complete?** It lists all files in a directory with no filtering by extension or handling of nested directories.

## Recommended Next Steps

When ready, run `/nbs-recovery` with this report. The recovery process should:

1. **Decide v3 fate** — archive or discard based on scaling requirements
2. **Remove dead ends** — discard `loader_v1.py` and `old/scratch.py`, preserving documentation of why they failed
3. **Consolidate v2** — promote to canonical `loader.py`, update imports
4. **Extract institutional knowledge** — preserve key decisions from `old/notes.txt` in proper documentation alongside the code
5. **Update README** — correct import path, update status, document optimal parameters and design decisions
6. **Expand test coverage** — add error handling and edge case tests
7. **Define data interface** — determine actual data format requirements and adapt loader

**No action has been taken. This report is read-only discovery output.**
