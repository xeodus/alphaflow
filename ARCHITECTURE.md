# AlphaFlow — Real-Time USD SOFR Curve Desk

**Design authority for the project.** This document is the contract. If code and
this document disagree, one of them is a bug. Changes to a decision marked
`[ADR]` require a new entry in the Decision Log (§14), not a silent edit.

Status: **locked** (design phase), implementation not started.
Target audience: front-office rates/credit engineering (e.g. Citadel Securities).

---

## 1. Problem statement

Build a C++23 service that:

1. ingests a live rates feed on a dedicated thread and timestamps every tick on
   arrival;
2. moves ticks across a lock-free queue so the market-data thread never blocks
   the pricer;
3. bootstraps a USD SOFR discount curve, rebuilding incrementally as quotes move;
4. prices a book of government bonds, corporate bonds, and interest-rate swaps
   off that curve;
5. computes spread-to-curve per instrument;
6. computes DV01 and bucketed curve risk for the whole book on every rebuild;
7. answers RFQ-style quote requests off the live curve, recording end-to-end
   latency per quote;
8. runs overnight P&L attribution (curve / spread / carry);
9. reports a latency histogram and proves the p99 quote path stays in budget.

### 1.1 Non-functional requirements (ranked)

| Priority | Requirement | Consequence |
|---|---|---|
| P0 | **Low, bounded tail latency** on the RFQ path | Hot path: no locks, no allocation, no blocking syscalls |
| P0 | **Correctness** — silent pricing bugs are the expensive kind | Oracle-based tests, conventions tested exhaustively |
| P1 | **Reliability / no single point of failure** | Derived state is rebuildable; active-active deterministic replicas |
| P1 | **Near-zero downtime** | Rolling restart + drain; no state to migrate |
| P2 | **Scalability** | Scale RFQ throughput horizontally; expensive work stays off hot path |
| P2 | **Sustainability** | Deterministic replay, golden masters, sanitizers in CI |

### 1.2 Explicit non-goals (v1)

- Real exchange connectivity / FIX / vendor adapters.
- Kernel bypass (DPDK / Solarflare `ef_vi`) — the socket interface is *shaped*
  for it, but not implemented.
- Options / swaptions / vol surfaces.
- Regulatory reporting, trade booking workflows.

---

## 2. The central architectural idea `[ADR-001]`

> **The RFQ path must never do work proportional to curve size, book size, or
> feed rate.**

The RFQ path reads a pointer to a fully-built, immutable snapshot and does O(1)
work for the requested instrument. Everything expensive — bootstrap, risk, P&L —
runs on separate threads and *publishes* immutable snapshots. Readers never lock.

**This is the single decision that makes latency, reliability, and scalability
fall out together.** The naive alternative (curve builder and RFQ responder on
one thread, as in the original sketch) couples RFQ latency to bootstrap cost and
makes any µs-level claim impossible.

```
        [ADR-001] producer/consumer with immutable snapshots

  FeedThread ──SPSC──▶ CurveThread ──atomic ptr──▶ CurveSnapshot (immutable)
                                                         │
                          ┌──────────────────────────────┼───────────────────┐
                          ▼                              ▼                   ▼
                     RFQThread(s)                   RiskThread         (replay/audit)
                     read-only, O(1)                read-only, batch
```

---

## 3. Design principles

1. **Single-writer, immutable snapshots.** One thread writes the curve; readers
   acquire-load a pointer. No shared mutable state on the hot path.
2. **Derived state is disposable.** Curve and risk are pure functions of
   (feed + positions + code). Persist only the inputs (event log + trade log).
   Recovery = replay. HA = replicate the inputs, not the state.
3. **Market data is state, not a stream, for pricing.** Keep the latest quote per
   instrument in a lock-free cache and price off that. Keep the full tick stream
   in an append-only log for replay/audit/P&L. Delivering ticks you do not need
   is a latency bug.
4. **Nothing on the hot path allocates, locks, throws, or logs.**
5. **Determinism is a requirement.** Same inputs → identical curve → identical
   quote. This is what allows active-active replicas with no consensus and no
   shared broker.
6. **Two clocks, never mixed.** Monotonic for latency; wall-clock for business
   dates and P&L. Capture both at tick arrival.

---

## 4. Component and thread model

One process, one thread per responsibility (not per request). No thread pools.

| Thread | Count | Responsibility | Writes |
|---|---|---|---|
| Feed adapter | 1 per feed line (A/B) | recv, timestamp, sequence check, normalize | `QuoteCache`, replay log, SPSC ring |
| Curve | 1 | drain changed-quotes, incremental bootstrap, publish | `atomic<const CurveSnapshot*>` |
| RFQ | 1 per core (`SO_REUSEPORT`) | busy-poll socket, read request, load snapshot, price, serialize, send | per-thread `HdrHistogram` |
| Risk | 1 | on new snapshot: DV01, bucketed risk, spread P&L | `atomic<const RiskSnapshot*>` |
| P&L | 1 (EOD) | attribution from yesterday/today snapshots | attribution report |
| Metrics | 1 (low prio) | drain histograms, export | — |

### 4.1 Data flow

```
 External feed A ─┐
 External feed B ─┴─▶ SequenceArbiter ─▶ Tick
                                           │
                    ┌──────────────────────┼──────────────────────┐
                    ▼                      ▼                      ▼
             QuoteCache (seqlock)     ReplayLog (mmap)      SpscRing
             latest per instrument    append-only           → CurveThread
                    │
                    ▼
             CurveThread: incremental bootstrap ──▶ CurveSnapshot (atomic)
                                                         │
                                    ┌────────────────────┼─────────────┐
                                    ▼                    ▼             ▼
                               RFQThread(s)          RiskThread     Replay/audit
```

---

## 5. Concurrency primitives

### 5.1 SPSC ring `[ADR-002]`

`SpscRing<T, N>`: power-of-two capacity, `head`/`tail` padded to separate cache
lines to avoid false sharing, acquire on consume / release on produce.

- **Why SPSC, not MPSC/MPMC:** a single producer needs no CAS, so there is no
  contention and no retry loop. MPSC buys nothing with one feed thread and adds
  cache-line ping-pong.
- **Why lock-free, not `mutex`+condvar:** a contended mutex can resume the
  waiter after an unbounded scheduler delay — exactly what p99.9 measures.
- **Memory order:** `acquire`/`release` only. `seq_cst` is banned on the hot
  path; on ARM it compiles to full barriers.
- **Backpressure:** bounded by design. Full-queue policy for market data is
  *coalesce*, implemented upstream in the `QuoteCache` (latest value wins), so
  the ring carries only change-notifications, not every tick.

### 5.2 Snapshot publication `[ADR-003]`

`SnapshotPtr<T>`: `std::atomic<const T*>` + epoch/quiescent-state reclamation.

- Reader cost ≈ one acquire load.
- **Why not `std::atomic<std::shared_ptr<T>>`:** the standard-library
  implementation is typically lock/spinlock-based, reintroducing the tail
  latency you removed.
- **Why not a seqlock for the snapshot:** the snapshot is a complex object;
  copy-on-read is expensive. A seqlock is reserved for small POD cells
  (`QuoteCache`).
- **Why not a raw double-buffer:** the writer would be unable to write while a
  reader reads; needs triple buffering or seqlock anyway.

### 5.3 Quote cache `[ADR-004]`

Per-instrument seqlock holding the latest quote. Writer bumps a sequence counter
odd → write → even; reader retries if odd or changed. Small, POD, cache-line
aligned per instrument.

### 5.4 Scratch workspace

`Scratch` is a per-thread, preallocated workspace (root-finder state, cashflow
scratch). No heap traffic on the hot path.

---

## 6. Market data plane

### 6.1 Tick and clocking

```cpp
struct Tick {
  InstrumentId  id;
  double        value;      // rate or price, normalized units
  TscTicks      recv_tsc;   // monotonic capture at arrival
  std::uint64_t seq;        // per-feed sequence for gap detection
  FeedId        feed;       // A or B
};
```

- Timestamp at **arrival**, before any parsing/allocation, to keep the latency
  span honest.
- Hardware/NIC timestamp used when available; software monotonic otherwise.
- Wall-clock derived separately from the same instant for business dates.

### 6.2 Sequence arbitration

- A/B lines carry independent sequence numbers.
- `SequenceArbiter` tracks last-good per feed; on a gap it marks that line stale
  and promotes the peer. A line is only trusted again after it catches up.
- Duplicate sequence numbers across the join are de-duplicated by
  `(feed, seq)`.

### 6.3 Replay log

- Append-only, memory-mapped, length-prefixed records.
- Contains every accepted tick (feed, seq, wall/monotonic timestamps, value) and
  every trade/position event.
- **Purpose:** deterministic replay, crash recovery, audit, A/B reconciliation,
  and the regression harness.

---

## 7. Curve construction

### 7.1 Instrument set `[ADR-005]`

USD SOFR discount curve:

| Bucket | Instruments |
|---|---|
| Overnight | SOFR fixing (published by NY Fed) |
| Short end | 1M/3M 3M-SOFR futures (SR3), quarterly |
| Term | SOFR OIS swaps, 1W–50Y |

### 7.2 Bootstrap algorithm `[ADR-006]`

- Sequential, short → long. For each pillar, solve the discount factor that
  makes the instrument PV = 0 given already-solved DFs.
- **Newton** with an analytic derivative (quadratic convergence, ~3–5
  iterations); **Brent** as a robust fallback. Root-finder tolerance and
  iteration cap live in config and are part of the deterministic contract.
- **Futures convexity adjustment** is applied to SR3 before use; without it the
  curve kinks at the futures/swap junction.

### 7.3 Interpolation `[ADR-007]`

- **v1:** log-linear on discount factors. Positive DFs, piecewise-constant
  forwards, no arbitrage, and **local support** — which is what makes
  incremental rebuild correct.
- **v2:** monotone-convex on forward rates (Hagan–West, 2006).
- **Why not naive cubic spline on zero rates:** oscillates between pillars and
  can produce negative forwards → arbitrage. Disqualified.

### 7.4 Incremental rebuild `[ADR-008]`

Because v1 interpolation is local (adjacent pillars only), a bump to pillar *k*
only invalidates pillars *k…end* under a sequential bootstrap. Rebuild re-solves
from the earliest changed pillar forward. **This is the correct incremental
boundary;** a full rebuild is never required just to publish a new quote.

### 7.5 Key curve identities (make these tests)

- OIS floating leg with daily compounding and no spread telescopes to
  `PV_float = N · (D(t0) − D(tn))`.
- Par OIS rate:
  `K = (D(t0) − D(tn)) / Σ_j δ_j · D(t_j)`.
- Swap DV01 (fixed receiver) = `−N · Σ_j δ_j · D(t_j) · δt` (parallel bump).

### 7.6 Conventions (the #1 source of silent bugs)

Day counts (ACT/360, ACT/ACT ICMA, 30/360, ACT/365), US SIFMA / NY-Fed holiday
calendar, business-day adjustment (Modified Following), end-of-month rule, T+2
spot lag for swaps. `Schedule` generation is fully deterministic and generated
once per instrument; RFQ only *evaluates* the precomputed schedule.

---

## 8. Pricing engine `[ADR-009]`

- **Stateless library**, used by both the RFQ path and the risk path. Not a
  service. A network hop here would add microseconds-to-milliseconds and destroy
  the budget.
- **Swap pricer:** fixed leg = `Σ K·δ_j·D(t_j)`; floating via §7.5.
- **Bond pricer:** accrued, dirty = clean + accrued; yield-to-maturity by
  Newton; Z-spread by root-find on `PV(curve + s) = dirty`; G-spread vs
  benchmark.
- **Spread to curve:** bond Z-spread; swap spread = par rate − OIS.
- Contract: `noexcept`, allocation-free, computes into caller-provided scratch.

```cpp
Quote price_swap(const CurveSnapshot&, const SwapSpec&, Scratch&) noexcept;
Quote price_bond(const CurveSnapshot&, const BondSpec&, Scratch&) noexcept;
```

---

## 9. Risk and P&L `[ADR-010]`

- **DV01 / bucketed risk:** bump each curve pillar (e.g. 0.5–1 bp), re-price the
  book, central difference. Runs on the Risk thread off the same immutable
  snapshot; never blocks RFQ.
- **v2 upgrade:** adjoint / reverse-mode AAD — all sensitivities in one pass.
  Kept for v2 because v1 must be validated against a simple oracle first.
- **P&L attribution (EOD):** carry (accrual + roll + funding) + curve
  (DV01 · Δcurve) + spread (spread-DV01 · Δspread) + residual, using yesterday's
  and today's snapshots. Any unattributed residual beyond threshold is an alert,
  not a rounding detail.

---

## 10. Reliability, HA, and "zero downtime"

"Zero downtime" in capital markets means **no impact from any single component
failure**, achieved by redundancy + fast deterministic recovery + graceful
degradation. It is measured with RTO/RPO, not uptime percentage.

- **Topology (single host, multi-process) `[ADR-011]`:** two or more identical
  engine processes, active-active, each with its own feed adapter (A/B). Both are
  deterministic, so either can answer any RFQ with the same result.
- **No consensus, no shared broker, no coordinator.** There is nothing to elect
  and nothing to fail over *to*; that is the no-SPOF property. Introducing
  Raft/Kafka here would solve a problem this system does not have.
- **Feed redundancy:** A/B lines with gap detection and primary/backup
  arbitration (§6.2).
- **Rolling restart / deploy:** drain RFQs to peers, restart, rejoin, resume.
  No state migration — state is derived and replayed.
- **Overload policy:** bounded queues; shed load or return a
  **stale-but-timestamped** quote rather than block. A frozen desk is worse than
  a stale quote.
- **Watchdog:** heartbeat + supervisor restart; fail fast at startup, degrade at
  runtime.
- **Recovery:** replay the append log to the last checkpoint; RTO ≈ replay time.

---

## 11. Observability and latency measurement `[ADR-012]`

- Per-thread lock-free `HdrHistogram` for each stage: ingest, rebuild, RFQ.
- **Nothing logs on the hot path.** Samples go to a lock-free buffer drained by
  the Metrics thread.
- **The latency span must be written down next to every number.** RFQ latency is
  defined as: *timestamp when request bytes are read off the socket → timestamp
  after response bytes are handed to the NIC.* In-process handler time is a
  different, smaller number and must be labelled as such.
- Report **p50 / p90 / p99 / p99.9 / max**, per stage. p99 alone hides the tail a
  desk actually cares about.
- Export to Prometheus/OpenTelemetry for ops; the histogram is the source of
  truth for the p99 claim.

### 11.1 The 6 µs p99 question

A prior claim of **~6 µs p99** is *plausible but conditional and cannot be
asserted before building.* Rough in-process budget on a warm snapshot: snapshot
load ~10 ns, schedule lookup ~200 ns, swap PV + spread ~1–2 µs, serialize
~200 ns, socket ~1–3 µs. p99 is then dominated by OS preemption.

Rules for the claim:

1. Define the span (§11). No span → the number is indefensible.
2. Report the full distribution, not just p99.
3. Platform is stated: **Linux x86-64 is the performance platform** `[ADR-013]`;
   macOS is for iteration only. A desk runs Linux; kernel bypass, `SO_BUSY_POLL`,
   `isolcpus`, huge pages, and `RDTSC` do not exist in the same form on macOS.

Levers to beat 6 µs and stay defensible: precompute schedules, prefault all
snapshot memory, avoid virtual dispatch on the hot path, `-O3 -march=native` +
LTO + PGO, busy-poll instead of `epoll`, pin threads to isolated cores. **Let
the histogram produce the number; never assert it first.**

---

## 12. Platform abstraction

`platform/` is the only place that touches the OS:

| Concern | Linux | macOS (iteration) |
|---|---|---|
| Monotonic clock | `clock_gettime(CLOCK_MONOTONIC_RAW)` / `RDTSC` | `mach_absolute_time` |
| Affinity | `sched_setaffinity`, `isolcpus` | best-effort `THREAD_AFFINITY_POLICY` |
| Socket | `SO_BUSY_POLL`, `SO_REUSEPORT` | kqueue / loopback |

Engine code includes only `platform/`, never `<sys/socket.h>` etc. The socket
interface is deliberately shaped so a kernel-bypass backend can be added without
touching pricing or curve code.

---

## 13. Testing and verification

| Layer | Method |
|---|---|
| Conventions (day count, calendar, schedule) | Exhaustive unit tests vs hand-computed values |
| Curve / pricing | Oracle tests vs QuantLib (**test-only dependency**) |
| Root finders / interpolation | Analytic cases + property tests |
| Concurrency | TSan stress tests; SPSC/`QuoteCache` model tests |
| Memory | ASan + UBSan |
| Determinism | Replay the same log twice → byte-identical `CurveSnapshot` |
| Feed arbitration | Injected gaps/failures on A/B; verify promotion |
| Latency | Google Benchmark micro + in-process histogram; CI guardrail on p99.9 |
| Recovery | Kill -9 mid-replay → restart → state matches checkpoint |

**QuantLib is never linked into the engine.** It is a reference oracle only.

---

## 14. Decision log (ADRs)

| ID | Decision | Alternatives rejected |
|---|---|---|
| ADR-001 | Snapshot-decoupled RFQ path | Shared curve+RFQ thread |
| ADR-002 | SPSC lock-free ring | MPSC, mutex+condvar |
| ADR-003 | Atomic pointer + epoch reclamation | `atomic<shared_ptr>`, seqlock snapshot, double buffer |
| ADR-004 | Per-instrument seqlock quote cache | FIFO of every tick |
| ADR-005 | SOFR fixings + SR3 futures + OIS swaps | Deposits/OIS only |
| ADR-006 | Newton (Brent fallback) bootstrap + futures convexity adj | Pure Brent, no convexity adj |
| ADR-007 | v1 log-linear DF, v2 monotone-convex forwards | Cubic spline on zeros |
| ADR-008 | Incremental rebuild from earliest changed pillar | Full rebuild per tick |
| ADR-009 | Pricing as stateless library | Pricing as a service |
| ADR-010 | v1 bump-and-revalue, v2 AAD | AAD from day one |
| ADR-011 | Single-host active-active deterministic replicas | Consensus/coordinator; single process |
| ADR-012 | HdrHistogram per stage, defined span | Single aggregate number |
| ADR-013 | Linux x86-64 as performance platform | macOS-only |

---

## 15. Repo layout

```
alphaflow/
  CMakeLists.txt  CMakePresets.json  cmake/
  include/alphaflow/
    core/         types, time (dual clock), error (std::expected), config
    concurrency/  spsc_ring, seqlock, snapshot_ptr, latest_value_cache
    market/       tick, feed_source, synthetic_feed, sequence_arbiter, replay_log
    curve/        day_count, calendar, schedule, interpolation, ois_instrument,
                  bootstrap, curve_snapshot
    pricing/      swap_pricer, bond_pricer, spread
    risk/         dv01, bucket_risk
    pnl/          attribution
    rfq/          responder, server
    metrics/      hdr_histogram, latency_recorder
    platform/     clock, affinity, socket
  src/            implementations + main.cpp
  tests/{unit,integration,benchmark}/
  tools/          gen_synthetic, replay_driver
  data/replay/    generated logs
```

---

## 16. Milestones

**M1 — Vertical slice (first buildable target).** Synthetic SOFR feed → SPSC →
incremental OIS bootstrap → swap RFQ over loopback → HdrHistogram. Done when:

1. Deterministic synthetic generator + replay log.
2. Feed → SPSC → curve thread TSan-clean under stress.
3. Bootstrap matches QuantLib oracle to `< 1e-9`; bump-one-pillar rebuild
   touches only downstream pillars.
4. `PV_float == N·(D(t0)−D(tn))`; par swap PV = 0.
5. RFQ histogram with p50/p90/p99/p99.9/max and a documented span.
6. ASan/UBSan/TSan green; determinism test passes.
7. One Linux latency report with methodology.

**M2 — Breadth.** Bonds, Z-spread, DV01/bucketed risk on a separate thread.

**M3 — Reliability.** Replay log + checkpoint/recovery, A/B arbitration,
active-active determinism test, rolling restart/drain.

**M4 — Depth.** P&L attribution, monotone-convex forwards, AAD risk.

**M5 — Performance hardening.** Core pinning, busy-poll, memory layout, LTO/PGO,
final Linux latency report.

---

## 17. Success criteria

The project succeeds when every number it reports is **reproducible, span-
documented, and platform-labelled**, and the system keeps answering RFQs
correctly while a feed line, a process, or a thread dies. Correctness and
honest measurement are the product; latency is the proof.
