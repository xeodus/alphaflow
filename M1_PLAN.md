# M1 Build Plan

Scheduling breakdown for milestone **M1** of [`ARCHITECTURE.md`](ARCHITECTURE.md).
The architecture document is the contract; this file only says *when* each piece
is built and *how it maps* to the M1 exit criteria. If the two disagree, the
architecture wins and this file is corrected.

This file exists because the plan previously lived only in conversation and
drifted: two items fell between phases. They are now placed explicitly, marked
**[moved]** below.

## Milestones (from ARCHITECTURE.md §16)

M1 vertical slice · M2 breadth · M3 reliability · M4 depth · M5 performance.
This plan covers **M1** only.

## M1 in phases

| Phase | Goal | Status |
|---|---|---|
| **P0 Foundation** | reproducible build, CMake presets, CI matrix, `platform/clock` | done |
| **P1 Concurrency substrate** | `spsc_ring`, `seqlock`, `latest_value_cache`, `snapshot_ptr` | done |
| **P2 Walking skeleton** | `platform/socket`, `rfq/protocol`, `metrics`, `rfq` server, end-to-end stub | done |
| **P3 Engine** | market-data plane, curve, pricing, oracle | **next** |
| **P4 Evidence** | determinism, no-allocation guard, live metrics, benchmarks | |
| **M1b Hardware** | bare-metal Linux, pinning, published latency report | blocked on hardware |

Phase status describes code merged and CI-green, not intent.

## Phase 3 — Engine

**Market-data plane**
- `core/date` — civil date arithmetic (foundation of every convention).
- `curve/calendar`, `curve/day_count`, `curve/schedule`.
- `market/tick`, `market/synthetic_feed` (deterministic PRNG, rate-controlled).
- `market/replay_log` + Logger thread (ADR-015).
- **A/B arbiter** — **[moved]** from Phase 2. `market/sequence_arbiter` +
  `market/arbiter`: the single writer of the quote cache, with gap detection,
  dedupe, and line promotion (ADR-014). Deferred from P2.5 because it needs the
  market types above; it is the missing spine of the data plane.

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
- **Live metrics thread** — **[moved]** from "Phase 2 gap". Uses HdrHistogram's
  `hdr_interval_recorder` to drain per-thread histograms while workers record.
  This was never a Phase 2 deliverable; it is an observability item and belongs
  here.
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
| 5 | RFQ histogram p50…max + span + SLO comparison | P2 (machinery, done) + P4 (SLO compare) |
| 6 | ASan/UBSan/TSan green; determinism; Linux `platform/` smoke | P0–P2 (sanitizers, Linux CI) + P4 (determinism) |
| 7 | Linux latency report | M1b |
