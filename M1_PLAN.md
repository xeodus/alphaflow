# M1 Build Plan

Scheduling breakdown for milestone **M1** of [`ARCHITECTURE.md`](ARCHITECTURE.md).
The architecture document is the contract; this file only says *when* each piece
is built and *how it maps* to the M1 exit criteria. If the two disagree, the
architecture wins and this file is corrected.

This file exists because the plan previously lived only in conversation and
drifted. Phase 2 was first reported complete while the P2.5 wiring task — the
A/B arbiter and the live metrics thread — was still outstanding. That was
corrected, both were then built, and P2 is complete as of this revision. Status
means code merged and CI-green, nothing else.

## Milestones (from ARCHITECTURE.md §16)

M1 vertical slice · M2 breadth · M3 reliability · M4 depth · M5 performance.
This plan covers **M1** only.

## M1 in phases

| Phase | Goal | Status |
|---|---|---|
| **P0 Foundation** | reproducible build, CMake presets, CI matrix, `platform/clock` | done |
| **P1 Concurrency substrate** | `spsc_ring`, `seqlock`, `latest_value_cache`, `snapshot_ptr` | done |
| **P2 Walking skeleton** | `platform/socket`, `rfq/protocol`, `metrics`, `rfq` server, **A/B arbiter**, end-to-end stub through the arbiter, **live Metrics thread** | done |
| **P3 Engine** | market-data plane, curve, pricing, oracle | **next** |
| **P4 Evidence** | determinism, no-allocation guard, benchmarks | |
| **M1b Hardware** | bare-metal Linux, pinning, published latency report | blocked on hardware |

Phase status describes code merged and CI-green, not intent.

## Phase 3 — Engine

**Market-data plane**
- `core/date` — civil date arithmetic (foundation of every convention).
- `curve/calendar`, `curve/day_count`, `curve/schedule`.
- `market/synthetic_feed` (deterministic PRNG, rate-controlled).
- `market/replay_log` + Logger thread (ADR-015).

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
- Benchmarks and the full in-process latency distribution.

## M1b — Hardware

- Bare-metal Linux host, isolated cores, pinned threads.
- The published latency report with methodology (criterion 7).

## M1 exit criteria → where they are earned

| # | Criterion | Earned in |
|---|---|---|
| 1 | Deterministic generator + logger-written replay log | P3 market data |
| 2 | A/B → arbiter → curve TSan-clean; no-lost-change | P2 (arbiter built; skeleton TSan-clean; no-lost-change tested) |
| 3 | Oracle < 1e-9; incremental bump; coalescing | P3 curve |
| 4 | Telescoping; par PV = 0; DV01 vs bump-revalue | P3 pricing |
| 5 | RFQ histogram p50…max + span + SLO comparison | P2 (machinery, done) + P4 (SLO compare) |
| 6 | ASan/UBSan/TSan green; determinism; Linux `platform/` smoke | P0–P2 (sanitizers, Linux CI) + P4 (determinism) |
| 7 | Linux latency report | M1b |
