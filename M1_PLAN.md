# M1 Build Plan

Scheduling breakdown for milestone **M1** of [`ARCHITECTURE.md`](ARCHITECTURE.md).
The architecture document is the contract; this file only says *when* each piece
is built and *how it maps* to the M1 exit criteria. If the two disagree, the
architecture wins and this file is corrected.

This file exists because the plan previously lived only in conversation and
drifted. Two items that were in Phase 2's original scope — the A/B arbiter and
the live metrics thread (both named in the P2.5 wiring task) — were deferred
rather than built. **Phase 2 is therefore recorded as partial, not complete.**
The deferred items are marked **[deferred]** and placed explicitly below.

## Milestones (from ARCHITECTURE.md §16)

M1 vertical slice · M2 breadth · M3 reliability · M4 depth · M5 performance.
This plan covers **M1** only.

## M1 in phases

| Phase | Goal | Status |
|---|---|---|
| **P0 Foundation** | reproducible build, CMake presets, CI matrix, `platform/clock` | done |
| **P1 Concurrency substrate** | `spsc_ring`, `seqlock`, `latest_value_cache`, `snapshot_ptr` | done |
| **P2 Walking skeleton** | `platform/socket`, `rfq/protocol`, `metrics`, `rfq` server, single-producer end-to-end stub | **partial** — A/B arbiter and live metrics thread deferred |
| **P3 Engine** | market-data plane, curve, pricing, oracle | **next** |
| **P4 Evidence** | determinism, no-allocation guard, live metrics, benchmarks | |
| **M1b Hardware** | bare-metal Linux, pinning, published latency report | blocked on hardware |

Status describes code merged and CI-green. **partial** means the listed scope is
built, but items that were originally scoped in that phase were deferred.

## Phase 3 — Engine

**Market-data plane**
- `core/date` — civil date arithmetic (foundation of every convention).
- `curve/calendar`, `curve/day_count`, `curve/schedule`.
- `market/tick`, `market/synthetic_feed` (deterministic PRNG, rate-controlled).
- `market/replay_log` + Logger thread (ADR-015).
- **A/B arbiter** — **[deferred]** from Phase 2 (originally scoped in P2.5).
  `market/sequence_arbiter` + `market/arbiter`: the single writer of the quote
  cache, with gap detection, dedupe, and line promotion (ADR-014). It needs the
  market types above, which is why it sits here rather than in P2.

**Curve**
- `curve/interpolation` (log-linear discount factors).
- `curve/ois_instrument` (PV, par rate, telescoping identity).
- `curve/futures_convexity` (SR1/SR3 + the futures/swap splice).
- `curve/bootstrap` (Newton + Brent) and the real `curve_snapshot`.
- Incremental rebuild from the earliest changed pillar.
- **QuantLib oracle harness** (test-only) — the independent check.

**Pricing**
- `pricing/swap_pricer` + `Scratch` (allocation-free).
- Single-swap DV01 (bump-and-revalue).

## Phase 4 — Evidence

- No-allocation guard on the RFQ path.
- Determinism test: replay the same log twice → byte-identical snapshot.
- Rebuild-coalescing and staleness tests; RFQ response-contract tests.
- **Live metrics thread** — **[deferred]** from Phase 2 (originally scoped in
  P2.5). Uses HdrHistogram's `hdr_interval_recorder` to drain per-thread
  histograms while workers record. It was named in the Phase 2 wiring plan and
  was not built.
- Benchmarks and the full in-process latency distribution.

## M1b — Hardware

- Bare-metal Linux host, isolated cores, pinned threads.
- The published latency report with methodology (criterion 7).

## M1 exit criteria → where they are earned

| # | Criterion | Earned in |
|---|---|---|
| 1 | Deterministic generator + logger-written replay log | P3 market data |
| 2 | A/B → arbiter → curve TSan-clean; no-lost-change | P3 market data |
| 3 | Oracle < 1e-9; incremental bump; coalescing | P3 curve |
| 4 | Telescoping; par PV = 0; DV01 vs bump-revalue | P3 pricing |
| 5 | RFQ histogram p50…max + span + SLO comparison | P2 (machinery) + M1b (SLO compare on Linux) |
| 6 | ASan/UBSan/TSan green; determinism; Linux `platform/` smoke | P0–P2 (sanitizers, Linux CI) + P4 (determinism) |
| 7 | Linux latency report | M1b |
