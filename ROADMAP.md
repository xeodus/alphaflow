# Roadmap

The **single source of truth** for delivery: what is built, what is next, and
what remains. [`ARCHITECTURE.md`](ARCHITECTURE.md) is the design contract; this
file tracks the work against it. If they disagree, the architecture wins and
this file is corrected.

**Status legend:** `done` · `partial` · `next` · `planned` · `out of scope`.

## Where we are

The **systems scaffolding is built and verified** (100 tests, CI green): feed
ingestion, lock-free queues, the A/B arbiter, immutable snapshots, the RFQ
server, and honest latency measurement. The **quant core — the curve and the
pricing — is not built yet**; the running system currently quotes a placeholder
number.

**Target scope (locked): M1 + M3 + M5.** M2 (breadth) and M4 (depth) are out of
scope. See "Scope" below.

## The five milestones

| Milestone | What it adds | What you can demo when done | Status |
|---|---|---|---|
| **M1** Vertical slice | Real SOFR curve + swap pricing + oracle | Watch a SOFR curve build live; send a swap RFQ; get a real price; see honest p50/p99; matches QuantLib | **partial** |
| **M2** Breadth | Bonds, Z-spread, DV01 + bucketed risk | Price a book of swaps+bonds; show risk by tenor and spreads | out of scope |
| **M3** Reliability | Recovery, A/B failover, active-active, drain | Kill it mid-run → it recovers; two replicas agree bit-for-bit | planned |
| **M4** Depth | P&L attribution, monotone-convex forwards, AAD | End-of-day attribution; all sensitivities in one adjoint pass | out of scope |
| **M5** Performance | Pinning, busy-poll, LTO/PGO, final report | Bare-metal Linux latency report with methodology | planned |

---

## Scope (locked)

The target is a low-latency **software-engineering** flagship for top quant
firms: a real-time USD SOFR swap RFQ service that is correct, fast, and reliable.

- **In scope:** M1 (curve + pricing + oracle), M3 (recovery, failover,
  determinism), M5 (performance + Linux latency report).
- **Out of scope:** M2 (bonds, book risk) and M4 (P&L, AAD) — domain/research
  breadth with little signal for a low-latency SWE role. Recorded here so the
  scope does not silently drift back in.
- **Optional future add-on:** a small **order-book / matching-engine module** —
  the domain top market makers test directly. It reuses the existing concurrency
  substrate and can be added after M5. Not required to ship the locked scope.

---

## Milestone 1 — Vertical slice (the gate)

**Goal.** Synthetic SOFR feed → arbiter → SPSC → incremental OIS bootstrap →
swap RFQ over loopback → HdrHistogram. Real curve, real prices, oracle-checked.

### Market data plane
| Task | Status |
|---|---|
| `platform/clock` (monotonic + TSC) | done |
| `market/tick`, `market/sequence_arbiter`, `market/arbiter` (A/B, sole cache writer) | done |
| `market/synthetic_feed` (deterministic PRNG, rate-controlled, replayable) | done |
| `market/replay_log` + Logger thread (ADR-015) | done |

### Conventions (the #1 source of silent bugs)
| Task | Status |
|---|---|
| `core/date` (civil date arithmetic) | done |
| `curve/day_count` (ACT/360, ACT/365F, ACT/ACT ISDA, 30/360, 30E/360) | done |
| `curve/calendar` (SIFMA/NY-Fed holidays, Modified Following, EOM) | done |
| `curve/schedule` (deterministic rolls, T+2 spot, EOM, stubs) | done |

### Curve
| Task | Status |
|---|---|
| `curve/interpolation` (log-linear discount factors) | done |
| `curve/ois_instrument` (par rate, annuity, telescoping identity) | done |
| **QuantLib oracle harness** (test-only) | done |
| `curve/bootstrap` (Newton per pillar, config-driven tolerance) | done |
| `curve/futures_convexity` (SR1/SR3 + futures/swap splice) | done |
| real `curve_snapshot` (pillars, DFs, metadata) | done |
| incremental rebuild from earliest changed pillar | done |
| direct bootstrap-vs-QuantLib DF comparison (criterion 3) | done |

### Pricing
| Task | Status |
|---|---|
| `pricing/swap_pricer` (allocation-free; Scratch not needed for swaps) | done |
| single-swap DV01 (bump-and-revalue) | done |

### Transport, observability, skeleton
| Task | Status |
|---|---|
| `platform/socket`, `rfq/protocol`, `rfq/responder`, `rfq/server` | done |
| `metrics` + live Metrics thread (`hdr_interval_recorder`) | done |
| end-to-end walking skeleton (feed A/B → arbiter → curve thread → RFQ) | done |

### Evidence
| Task | Status |
|---|---|
| ASan/UBSan/TSan green; Linux `platform/` smoke (CI) | done |
| determinism test (replay → byte-identical snapshot) | done |
| no-allocation guard on the RFQ path | done |
| benchmarks + in-process latency distribution | done |
| Linux latency report (needs hardware) | planned |

---

## Milestone 2 — Breadth *(out of scope)*

Bonds, Z-spread, book-level DV01/bucketed risk. Excluded: domain breadth that
adds little signal for a low-latency SWE role.
| Task | Status |
|---|---|
| bond pricer: accrued, clean/dirty, yield-to-maturity, Z-spread, G-spread | out of scope |
| immutable `BookSnapshot` from the trade log | out of scope |
| Risk thread: DV01 + bucketed curve risk (bump-and-revalue) | out of scope |
| spread-to-curve per instrument | out of scope |

## Milestone 3 — Reliability
| Task | Status |
|---|---|
| replay log + checkpoint/recovery (kill -9 → restart → state matches) | done |
| A/B failure injection and promotion under stress | done |
| active-active determinism test (ADR-017) | done |
| rolling restart / drain; watchdog; overload policy | done |

## Milestone 4 — Depth *(out of scope)*

P&L attribution, monotone-convex forwards, AAD risk. Excluded: research-flavored
depth; revisit only if pivoting toward quant research/strats.
| Task | Status |
|---|---|
| P&L attribution (carry + curve + spread + residual) | out of scope |
| monotone-convex forward interpolation (Hagan–West) | out of scope |
| AAD / reverse-mode risk | out of scope |

## Milestone 5 — Performance
| Task | Status |
|---|---|
| core pinning, busy-poll sockets, memory layout, LTO/PGO | partial — LTO done; pinning/busy-poll need Linux |
| bare-metal Linux host | planned |
| final latency report with methodology | planned |

---

## Dependency order

Target scope: **M1 → M3 → M5**.

```
M1 ──▶ M3 ──▶ M5
```

M1 first, always: nothing downstream works without a real curve. M3
(reliability) needs M1. M5 (performance) hardens a system that is already
correct and reliable. M2 and M4 are out of scope.

## Timeline (focused effort)

| | Estimate |
|---|---|
| M1 (the gate) | 3–6 weeks |
| M3 (reliability) | 2–3 weeks |
| M5 (performance + report) | 2–3 weeks + hardware |
| **Target total** | **~7–12 weeks** |

The wildcard is matching QuantLib's conventions (M1). M5 is the only hard
external dependency: it needs a bare-metal Linux host.

## What "finished" means

The project is finished when **M1 + M3 + M5** are complete: a real USD SOFR
curve, real swap prices validated against an independent oracle, honest
span-documented latency, recovery/failover/determinism, and a bare-metal Linux
latency report. **M1 is the gate**; M3 and M5 build on it. M2 and M4 are out of
scope.

## Keeping this current

Update the relevant status cell **in the same commit** as the work it describes.
No scope is "done" until it is merged and CI-green.
