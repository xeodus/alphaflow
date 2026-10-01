# AlphaFlow — Real-Time USD SOFR Curve Desk

**Design authority for the project.** This document is the contract. If code and
this document disagree, one of them is a bug. Changes to a decision marked
`[ADR]` require a new entry in the Decision Log (§14), not a silent edit.

Status: **v0.2 — frozen-enough-to-build.** Design phase; M1 not started.
Target audience: front-office rates/credit engineering (e.g. Citadel Securities).

> **Revision history.**
> - **v0.1** — full-document design lock.
> - **v0.2** — architectural review amendments. Fixes four internal contradictions
>   (single-writer topology, hot-path replay logging, unbounded snapshot
>   reclamation, undefined rebuild cadence) and four domain/correctness issues
>   (swap DV01 identity, SR1/SR3 futures, cross-replica determinism, risk
>   cadence). Adds the SLO contract (§11.2), the RFQ response contract (§10.1),
>   and ADR-014…ADR-019. **ADRs are frozen; prose may still be refined as M1
>   produces evidence.** The rule for the build: *the ADRs are locked, the prose
>   is a living spec.*
> - **v0.3** — Phase 1 build feedback. Records the reclamation actually built as
>   ADR-020 (reference counting with validate-on-acquire), superseding ADR-003's
>   "epoch reclamation" wording, and corrects the snapshot reader-cost claim in
>   §5.2.

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

The RFQ path reads a pointer to a fully-built, immutable snapshot and does work
proportional only to the *requested instrument's* cashflow count — bounded by its
tenor, not by the curve or the book. Everything expensive — bootstrap, risk,
P&L — runs on separate threads and *publishes* immutable snapshots. Readers never
lock.

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
                     read-only, O(cashflows)        read-only, batch
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
4. **Nothing on the hot path allocates, locks, throws, or does I/O.** (I/O
   includes the replay log — see §6.3.)
5. **Determinism is a requirement.** Same inputs → identical curve → identical
   quote. This is what allows active-active replicas with no consensus and no
   shared broker. It is a *contract*, not a hope — see §13.1.
6. **Two clocks, never mixed.** Monotonic for latency; wall-clock for business
   dates and P&L. Capture both at tick arrival.
7. **Every thread has exactly one owner of every piece of mutable state.** If
   two threads must touch the same value, one is the writer and the other reads a
   published snapshot or a lock-free cell. No exceptions.

---

## 4. Component and thread model

One process, one thread per responsibility (not per request). No thread pools.

| Thread | Count | Responsibility | Writes |
|---|---|---|---|
| Feed adapter | 1 per feed line (A/B) | recv, timestamp on arrival, normalize; **no shared state** | its own SPSC ring → Arbiter |
| **Arbiter** | 1 | sequence check, gap detection, dedupe, coalesce; **sole writer of the quote cache** | `QuoteCache` (seqlock), SPSC ring → Curve, SPSC ring → Logger |
| Curve | 1 | drain changed set, coalesced incremental bootstrap, publish | `SnapshotPtr<CurveSnapshot>` (pooled) |
| RFQ | 1 per core (`SO_REUSEPORT`) | busy-poll socket, read request, load snapshot, price, serialize, send | per-thread `HdrHistogram` |
| Risk | 1 | on a throttled cadence: DV01, bucketed risk, spread P&L | `SnapshotPtr<RiskSnapshot>` (pooled) |
| **Logger** | 1 (low priority) | drain accepted records from ring, append to replay log | replay log (disk) |
| P&L | 1 (EOD) | attribution from yesterday/today snapshots | attribution report |
| Metrics | 1 (low prio) | drain histograms, export | — |

> **Why a dedicated Arbiter thread `[ADR-014]`.** Feed lines A and B are separate
> threads. A/B arbitration, dedupe, and the "latest quote wins" write are a
> *single-writer* responsibility. Letting both feed threads write the seqlock
> cache would violate the seqlock contract and the SPSC single-producer
> assumption. So each feed thread owns its own SPSC ring into the Arbiter, and
> the Arbiter is the unique writer of the `QuoteCache` and of the downstream
> rings. This is the fix for v0.1's producer-count contradiction.

### 4.1 Data flow

```
 External feed A ─▶ FeedThread A ──SPSC──┐
 External feed B ─▶ FeedThread B ──SPSC──┴─▶ ArbiterThread  (SOLE writer)
                                                │     │            │
                                     QuoteCache │  SpscRing     SpscRing
                                     (seqlock)  │  → Curve      → Logger
                                                │                  │
                                                │                  ▼
                                                │           ReplayLog (mmap;
                                                │           never touched by hot path)
                                                ▼
                                          CurveThread: coalesced incremental bootstrap
                                          ──▶ SnapshotPtr<CurveSnapshot> (pooled)
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

- **Why SPSC, not MPSC/MPMC:** every ring in the topology has **exactly one
  producer** (§4.1): each feed line owns its own ring into the Arbiter, and the
  Arbiter is the sole producer on the Curve and Logger rings. A single producer
  needs no CAS, so there is no contention and no retry loop. MPSC would only be
  required if two threads wrote the same ring — the topology forbids that, which
  is precisely why the Arbiter is a separate thread rather than shared state.
- **Why lock-free, not `mutex`+condvar:** a contended mutex can resume the
  waiter after an unbounded scheduler delay — exactly what p99.9 measures.
- **Memory order:** `acquire`/`release` only. `seq_cst` is banned on the hot
  path; on ARM it compiles to full barriers. The exact fence set is a reviewed,
  documented constant in `concurrency/`.
- **Backpressure:** bounded by design. Full-queue policy for market data is
  *coalesce*, implemented upstream in the `QuoteCache` (latest value wins), so
  the ring carries only change-notifications, not every tick. The no-lost-change
  invariant is stated in §5.3.

### 5.2 Snapshot publication `[ADR-003]` + `[ADR-016]` + `[ADR-020]`

A **bounded pool** of preallocated snapshot slots plus a single
`std::atomic<Slot*>` publication pointer. The built type is
`SnapshotPool<T, Depth>`; readers receive a move-only RAII lease.

- **Reader cost `[ADR-020]`:** two loads, one atomic fetch-add, and one
  `seq_cst` fence — **not** a single load. Safe reclamation needs a
  validate-re-load and a fence; no cheaper scheme is correct. Stated plainly
  because v0.2's "≈ one acquire load" cannot be met.
- **Bounded pool `[ADR-016]`:** snapshots are **not** heap-allocated per rebuild.
  The writer claims a free slot, fills it, then release-publishes the pointer. A
  reader *leases* the current snapshot; a lease keeps it valid and immutable for
  its lifetime.
- **Reclamation `[ADR-020]`:** reference counting with validate-on-acquire. The
  reader pins the slot it loaded, then re-checks that the publication pointer
  still points at it before trusting it; the writer reuses a slot only when it is
  neither published nor pinned. A `seq_cst` fence on each side closes the
  store→load race between "reader pins" and "writer scans"; without it the writer
  can reclaim a slot a reader is in the act of pinning.
- **Reclamation cannot be starved:** if no slot is free when the writer wants to
  publish (e.g. the Risk thread holds an old snapshot through a long
  bump-and-revalue), `try_publish` returns `false`; the writer coalesces and
  retries on a later tick. A stalled reader degrades *freshness* of the published
  curve; it can never block the writer or grow memory without bound. Pool depth
  is a configuration constant sized to the number of concurrent reader threads +
  headroom.
- **Why not `std::atomic<std::shared_ptr<T>>`:** the standard-library
  implementation is typically lock/spinlock-based, reintroducing the tail
  latency you removed.
- **Why not a seqlock for the snapshot:** the snapshot is a complex object;
  copy-on-read is expensive. A seqlock is reserved for small POD cells
  (`QuoteCache`).
- **Why not a raw double-buffer:** the writer would be unable to write while a
  reader reads; needs triple buffering or seqlock anyway.
- **v2 option:** hazard pointers or an epoch scheme give each reader a private
  registration slot and avoid the per-acquire atomic RMW. Deferred until
  contention on the shared counter is observed; the validate step and the fence
  remain in any scheme.

### 5.3 Quote cache `[ADR-004]`

Per-instrument seqlock holding the latest quote. Writer bumps a sequence counter
odd → write → even; reader retries if odd or changed. Small, POD, cache-line
aligned per instrument.

- **Single writer by construction:** only the Arbiter thread writes; the seqlock
  contract is respected because there is exactly one writer (§4.1).
- **No-lost-change invariant (coalescing is lossy but complete):** the
  change-notification ring may drop or merge notifications under load, but it
  must never lose a *change*. Each instrument carries a monotonically increasing
  `write_epoch`. The Curve thread, on draining the ring, re-reads the union of
  dirty epochs; an instrument whose `write_epoch` advanced is processed even if
  its notification was coalesced. A per-instrument dirty bit is cleared only
  after a successful read. **Invariant:** for every instrument, the Curve thread
  eventually observes the latest `write_epoch`.

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
- **TSC discipline `[ADR-013]`:** use an **invariant TSC** only (`CPUID` check).
  Calibrate TSC→ns at startup against `CLOCK_MONOTONIC_RAW`, periodically
  re-calibrate, and record the calibration epoch. On multi-socket hosts, verify
  cross-socket TSC synchronization before trusting raw TSC deltas; otherwise fall
  back to `clock_gettime`. macOS uses `mach_absolute_time`. TSC-based and
  `clock_gettime`-based spans are never mixed. See §12.

### 6.2 Sequence arbitration

Runs **inside the Arbiter thread** (§4.1) — it is not a separate shared object.

- A/B lines carry independent sequence numbers.
- `SequenceArbiter` tracks last-good per feed; on a gap it marks that line stale
  and promotes the peer. A line is only trusted again after it catches up.
- Duplicate sequence numbers across the join are de-duplicated by `(feed, seq)`.

### 6.3 Replay log `[ADR-015]`

- Append-only, memory-mapped (`MAP_POPULATE`-prefaulted), length-prefixed records.
- Contains every accepted tick (feed, seq, wall/monotonic timestamps, value) and
  every trade/position event.
- **Purpose:** deterministic replay, crash recovery, audit, A/B reconciliation,
  and the regression harness.
- **Written off the hot path `[ADR-015]`:** the Arbiter enqueues accepted records
  onto a bounded SPSC ring; the dedicated **Logger thread** (low priority) appends
  them to the mmap file. The ingest/arbitration threads **never touch the disk**.
  Rationale: an mmap append can take a first-touch page fault or stall on
  dirty-page writeback, and that stall would corrupt the very timestamps the
  latency claim depends on. The hot path writes RAM only.
- **Backpressure:** if the Logger ring is full, the Arbiter drops replay records
  (never blocks) and increments a `replay_gap` counter that is persisted and
  surfaced as an alert; recovery then starts from the last durable checkpoint.
  Market-data delivery to the curve is never gated on the logger.

---

## 7. Curve construction

### 7.1 Instrument set `[ADR-005]`

USD SOFR discount curve:

| Bucket | Instruments |
|---|---|
| Overnight | SOFR fixing (published by NY Fed) |
| Short end | 3-Month SOFR futures (**SR3**, quarterly) and 1-Month SOFR futures (**SR1**) |
| Term | SOFR OIS swaps, 1W–50Y |

> **Naming fix (v0.2):** SR3 is the **3-Month** SOFR contract; the **1-Month**
> contract is **SR1**. v0.1 merged the two under "SR3".

### 7.2 Bootstrap algorithm `[ADR-006]`

- Sequential, short → long. For each pillar, solve the discount factor that
  makes the instrument PV = 0 given already-solved DFs.
- **Newton** with an analytic derivative (quadratic convergence, ~3–5
  iterations); **Brent** as a robust fallback. Root-finder tolerance and
  iteration cap live in config and are part of the deterministic contract.
- **Futures convexity adjustment** is applied to SR3 (and SR1) before use;
  without it the curve kinks at the futures/swap junction.
- **Splice rule (deterministic):** SR1/SR3 futures imply forward rates after the
  convexity adjustment; the bootstrap splices those implied forwards into the
  first liquid OIS swap pillar at a **configured junction tenor**. The junction
  rule is fixed in config, recorded in the snapshot metadata, and tested — it is
  a common source of silent dislocations.

### 7.3 Interpolation `[ADR-007]`

- **v1:** log-linear on discount factors. Positive DFs, piecewise-constant
  forwards, no arbitrage, and **local support** — which is what makes
  incremental rebuild correct.
- **v2:** monotone-convex on forward rates (Hagan–West, 2006).
- **Why not naive cubic spline on zero rates:** oscillates between pillars and
  can produce negative forwards → arbitrage. Disqualified.

### 7.4 Incremental rebuild `[ADR-008]` + `[ADR-016]`

Because v1 interpolation is local (adjacent pillars only), a bump to pillar *k*
only invalidates pillars *k…end* under a sequential bootstrap. Rebuild re-solves
from the earliest changed pillar forward. **This is the correct incremental
boundary;** a full rebuild is never required just to publish a new quote.

- **Cadence `[ADR-016]`:** the Curve thread **coalesces** all pending changes and
  rebuilds **at most once per drain**, not once per tick. Publication rate is
  rate-limited by a configured floor interval to prevent snapshot-pool churn and
  reader thrash. Under a burst of quote updates the curve is rebuilt with the
  latest value of every dirty pillar, then published once.
- **Fast path:** if only long-end pillars move, the re-solve is `O(number of
  downstream pillars)`, not `O(curve)`. If the front pillar moves, it degrades to
  a full sequential re-solve — still off the RFQ path.

### 7.5 Key curve identities (make these tests)

- OIS floating leg with daily compounding and no spread telescopes to
  `PV_float = N · (D(t0) − D(tn))`.
- Par OIS rate:
  `K = (D(t0) − D(tn)) / Σ_j δ_j · D(t_j)`.
- **Swap sensitivity (corrected v0.2).** For a receive-fixed swap,
  `PV_fixed = N·K·Σ_j δ_j·D(t_j)` and `PV_float = N·(D(t0) − D(tn))`. Under a
  parallel bump of the zero curve `z(t) → z(t) + δ` (so `D(t) → D(t)·e^{−δ·t}`),
  the total derivative is

  ```
  dPV/dδ = N · [ K · Σ_j δ_j · D(t_j) · (−t_j) + t_n · D(t_n) − t_0 · D(t_0) ]
  ```

  > **Correction to v0.1:** the previously stated `−N · Σ_j δ_j · D(t_j) · δt`
  > is *not* swap DV01 — it omits the fixed rate `K`, omits the time weighting
  > `t_j`, and ignores the floating leg. **Do not hardcode a DV01 closed form.**
  > The engine asserts DV01 by bump-and-revalue against the QuantLib oracle; the
  > expression above is a cross-check, exact only for the zero-curve parallel
  > bump and only with a single-curve (OIS-discounting) convention.

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

## 9. Risk and P&L `[ADR-010]` + `[ADR-018]`

- **DV01 / bucketed risk:** bump each curve pillar (e.g. 0.5–1 bp), re-price the
  book, central difference. Runs on the Risk thread off the same immutable
  snapshot; never blocks RFQ.
- **Throttled cadence `[ADR-018]`:** full-book bump-and-revalue is
  `O(pillars × book × pricer_cost)` — tens of milliseconds at realistic size. It
  runs on a **configured cadence (target 5–20 Hz) or on-quiesce**, decoupled from
  the curve publication rate. Risk answers "what is risk *as of* snapshot S";
  it never claims to be synchronous with the latest quote.
- **Book source `[ADR-019]`:** the priced book is an immutable `BookSnapshot` assembled from
  the replayed trade log (or a static booked set for M2). It is read-only to the
  Risk thread; book updates publish a new immutable snapshot. (v0.1 never said
  where positions came from.)
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
  deterministic (§13.1), so either can answer any RFQ with the same result.
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

### 10.1 RFQ response contract

Every RFQ response carries the snapshot generation it was priced from, and one of
these outcomes. Nothing on the RFQ path blocks; every failure mode is explicit:

| Outcome | When | Client sees |
|---|---|---|
| `OK` | Snapshot available and curve age ≤ staleness threshold | Price + curve timestamp + generation |
| `STALE` | Snapshot available, curve age > threshold | Price labeled stale, with age and generation |
| `WARMING_UP` | No published snapshot yet | Explicit status (no price) — never a fabricated one |
| `UNKNOWN_INSTRUMENT` | Instrument not in the book/spec registry | Explicit status |
| `OVERLOADED` | Worker load-shedding | Explicit status (retry) |

The response is a discriminated union; there is no "price of zero" default.

---

## 11. Observability and latency measurement `[ADR-012]`

- Per-thread `HdrHistogram` for each stage: ingest, rebuild, RFQ. Each histogram
  is owned by exactly one thread (so it is uncontended — *not* "lock-free"; the
  correct word is *single-writer/uncontended*).
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

### 11.2 SLO contract (targets to be validated, not claims)

Everything above is unfalsifiable without numbers. These are **initial targets**
for M1–M2; the report states measured values and which target was missed.

| Metric | v1 target | Notes |
|---|---|---|
| Feed ingest rate | 50k ticks/s/line (peak 200k) | above typical USD rates burst |
| Curve rebuild rate | coalesced, ≤ 2k/s publish (typ. < 100/s) | ADR-016 |
| RFQ throughput | 20k req/s aggregate | across workers |
| RFQ in-process p50 | ≤ 2 µs | warm snapshot |
| RFQ in-process p99 | ≤ 6 µs | §11.1 |
| RFQ in-process p99.9 | ≤ 15 µs | headroom for preemption |
| Book size | 1k–10k instruments | M2 |
| Risk cadence | 5–20 Hz | ADR-018 |
| Curve staleness at RFQ | ≤ 100 µs after publish | measured, not assumed |
| Recovery RTO | ≤ replay wall-time of one trading day | M3 |

---

## 12. Platform abstraction

`platform/` is the only place that touches the OS:

| Concern | Linux | macOS (iteration) |
|---|---|---|
| Monotonic clock | `clock_gettime(CLOCK_MONOTONIC_RAW)` / invariant `RDTSC` + calibration | `mach_absolute_time` |
| Affinity | `sched_setaffinity`, `isolcpus` | best-effort `THREAD_AFFINITY_POLICY` |
| Socket | `SO_BUSY_POLL`, `SO_REUSEPORT` | kqueue / loopback |

Engine code includes only `platform/`, never `<sys/socket.h>` etc. The socket
interface is deliberately shaped so a kernel-bypass backend can be added without
touching pricing or curve code.

> **Build order note:** because macOS and Linux differ for clocks, affinity, and
> sockets, the `platform/` layer is exercised **on Linux from M1**, not deferred
> to M5. A performance platform that is only discovered at the end of the
> project is a debugging cliff.

---

## 13. Testing and verification

| Layer | Method |
|---|---|
| Conventions (day count, calendar, schedule) | Exhaustive unit tests vs hand-computed values |
| Curve / pricing | Oracle tests vs QuantLib (**test-only dependency**) |
| Root finders / interpolation | Analytic cases + property tests |
| Concurrency | TSan stress tests; SPSC/`QuoteCache` model tests |
| Memory | ASan + UBSan |
| Determinism | §13.1 reproducibility contract; replay the same log twice → byte-identical `CurveSnapshot` |
| Feed arbitration | Injected gaps/failures on A/B; verify promotion; notification-coalescing no-lost-change |
| Latency | Google Benchmark micro + in-process histogram; CI guardrail on p99.9 |
| Recovery | Kill -9 mid-replay → restart → state matches checkpoint |

**QuantLib is never linked into the engine.** It is a reference oracle only.

### 13.1 Cross-replica reproducibility contract `[ADR-017]`

ADR-011 (active-active identical results) depends on bit-identical computation.
That is a contract, enforced in CI, not an assumption:

- **Hermetic, reproducible build:** pinned compiler/toolchain; no `-ffast-math`;
  fixed FP-contraction (`-ffp-contract=off` or an explicit, tested setting);
  `-march`/ISA held constant **across replicas** (per-host `-march=native` + PGO
  is allowed only if the resulting binary is proven bit-identical on the target
  fleet; otherwise use a fixed baseline ISA for replicas and native only for
  local benchmarks).
- **No address-order or hash-order dependence:** no iteration over
  `unordered_map` in any priced path; all reductions use a fixed, specified order.
- **Test:** replay the same event log on two builds/hosts → byte-identical
  `CurveSnapshot` and identical `Quote` payloads. This test gates M3.

---

## 14. Decision log (ADRs)

| ID | Decision | Alternatives rejected |
|---|---|---|
| ADR-001 | Snapshot-decoupled RFQ path | Shared curve+RFQ thread |
| ADR-002 | SPSC lock-free ring | MPSC, mutex+condvar |
| ADR-003 | Atomic pointer + epoch reclamation | `atomic<shared_ptr>`, seqlock snapshot, double buffer |
| ADR-004 | Per-instrument seqlock quote cache | FIFO of every tick |
| ADR-005 | SOFR fixings + SR3/SR1 futures + OIS swaps | Deposits/OIS only |
| ADR-006 | Newton (Brent fallback) bootstrap + futures convexity adj | Pure Brent, no convexity adj |
| ADR-007 | v1 log-linear DF, v2 monotone-convex forwards | Cubic spline on zeros |
| ADR-008 | Incremental rebuild from earliest changed pillar | Full rebuild per tick |
| ADR-009 | Pricing as stateless library | Pricing as a service |
| ADR-010 | v1 bump-and-revalue, v2 AAD | AAD from day one |
| ADR-011 | Single-host active-active deterministic replicas | Consensus/coordinator; single process |
| ADR-012 | HdrHistogram per stage, defined span | Single aggregate number |
| ADR-013 | Linux x86-64 as performance platform | macOS-only |
| **ADR-014** | **Dedicated single-writer Arbiter thread** (feed A/B → 2 SPSC rings → Arbiter owns `QuoteCache`) | Two feed threads sharing the cache (seqlock violation) |
| **ADR-015** | **Replay log written off the hot path** by a dedicated Logger thread | mmap append on the ingest thread |
| **ADR-016** | **Bounded snapshot pool + coalesced, rate-limited rebuild/publication** | Heap-per-rebuild; publish per tick |
| **ADR-017** | **Cross-replica reproducibility contract** (hermetic build, fixed FP/ISA, order-stable reductions) | Assume determinism from "same code" |
| **ADR-018** | **Throttled risk cadence**, decoupled from curve publication | Bump-and-revalue on every snapshot |
| **ADR-019** | **Book assembled as an immutable `BookSnapshot`** from the replayed trade log | Undefined / implicit position source |
| **ADR-020** | **Snapshot reclamation by reference counting with validate-on-acquire** (supersedes ADR-003's "epoch reclamation") | Epoch/quiescent-state (no cheaper — still needs a fence — and more machinery); hazard pointers (v2 if reader contention appears) |

---

## 15. Repo layout

```
alphaflow/
  CMakeLists.txt  CMakePresets.json  cmake/
  include/alphaflow/
    core/         types, time (dual clock), error (std::expected), config
    concurrency/  spsc_ring, seqlock, snapshot_ptr (pooled), latest_value_cache
    market/       tick, feed_source, synthetic_feed, sequence_arbiter, arbiter,
                  replay_log, logger
    curve/        day_count, calendar, schedule, interpolation, ois_instrument,
                  futures_convexity, bootstrap, curve_snapshot
    pricing/      swap_pricer, bond_pricer, spread
    risk/         dv01, bucket_risk, book_snapshot
    pnl/          attribution
    rfq/          responder, server, protocol
    metrics/      hdr_histogram, latency_recorder
    platform/     clock (tsc calibration), affinity, socket
  src/            implementations + main.cpp
  tests/{unit,integration,benchmark,oracle}/
  tools/          gen_synthetic, replay_driver
  data/replay/    generated logs
```

---

## 16. Milestones

> The phase-by-phase build breakdown for M1 — which task is built when, and
> where each M1 exit criterion is earned — lives in
> [`M1_PLAN.md`](M1_PLAN.md). This section defines the milestone exit criteria;
> the plan file schedules the work against them.

**M1 — Vertical slice (first buildable target).** Synthetic SOFR feed → Arbiter →
SPSC → incremental OIS bootstrap → swap RFQ over loopback → HdrHistogram. Done
when:

1. Deterministic synthetic generator + replay log **written by the Logger thread**.
2. Feed A/B → Arbiter → curve thread TSan-clean under stress; coalescing
   no-lost-change test passes (ADR-014, ADR-015).
3. Bootstrap matches QuantLib oracle to `< 1e-9`; bump-one-pillar rebuild
   touches only downstream pillars; rebuild cadence coalesces (ADR-016).
4. `PV_float == N·(D(t0)−D(tn))`; par swap PV = 0; DV01 matches bump-and-revalue
   (§7.5).
5. RFQ histogram with p50/p90/p99/p99.9/max, a documented span, and results
   compared against the §11.2 SLO targets.
6. ASan/UBSan/TSan green; determinism test passes; `platform/` compiled and
   smoke-tested on Linux.
7. One Linux latency report with methodology (even if pre-hardening).

**M2 — Breadth.** Bonds, Z-spread, DV01/bucketed risk on a separate thread,
immutable `BookSnapshot`.

**M3 — Reliability.** Replay log + checkpoint/recovery, A/B arbitration,
active-active determinism test (ADR-017), rolling restart/drain.

**M4 — Depth.** P&L attribution, monotone-convex forwards, AAD risk.

**M5 — Performance hardening.** Core pinning, busy-poll, memory layout, LTO/PGO,
final Linux latency report.

---

## 17. Success criteria

The project succeeds when every number it reports is **reproducible, span-
documented, and platform-labelled**, and the system keeps answering RFQs
correctly while a feed line, a process, or a thread dies. Correctness and
honest measurement are the product; latency is the proof.
