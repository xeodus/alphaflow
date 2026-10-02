# Roadmap

The **single source of truth** for delivery: what is built, what is next, and
what remains. [`ARCHITECTURE.md`](ARCHITECTURE.md) is the design contract; this
file tracks the work against it. If they disagree, the architecture wins and
this file is corrected.

**Status legend:** `done` · `partial` · `next` · `planned`.

## Where we are

The **systems scaffolding is built and verified** (100 tests, CI green): feed
ingestion, lock-free queues, the A/B arbiter, immutable snapshots, the RFQ
server, and honest latency measurement. The **quant core — the curve and the
pricing — is not built yet**; the running system currently quotes a placeholder
number.

Roughly: **M1 ≈ 50%, M2–M5 = 0%, full vision ≈ 15–20%.**

## The five milestones

| Milestone | What it adds | What you can demo when done | Status |
|---|---|---|---|
| **M1** Vertical slice | Real SOFR curve + swap pricing + oracle | Watch a SOFR curve build live; send a swap RFQ; get a real price; see honest p50/p99; matches QuantLib | **partial** |
| **M2** Breadth | Bonds, Z-spread, DV01 + bucketed risk | Price a book of swaps+bonds; show risk by tenor and spreads | planned |
| **M3** Reliability | Recovery, A/B failover, active-active, drain | Kill it mid-run → it recovers; two replicas agree bit-for-bit | planned |
| **M4** Depth | P&L attribution, monotone-convex forwards, AAD | End-of-day attribution; all sensitivities in one adjoint pass | planned |
| **M5** Performance | Pinning, busy-poll, LTO/PGO, final report | Bare-metal Linux latency report with methodology | planned |

---

## Milestone 1 — Vertical slice (the gate)

**Goal.** Synthetic SOFR feed → arbiter → SPSC → incremental OIS bootstrap →
swap RFQ over loopback → HdrHistogram. Real curve, real prices, oracle-checked.

### Market data plane
| Task | Status |
|---|---|
| `platform/clock` (monotonic + TSC) | done |
| `market/tick`, `market/sequence_arbiter`, `market/arbiter` (A/B, sole cache writer) | done |
| `market/synthetic_feed` (deterministic PRNG, rate-controlled, replayable) | planned |
| `market/replay_log` + Logger thread (ADR-015) | planned |

### Conventions (the #1 source of silent bugs)
| Task | Status |
|---|---|
| `core/date` (civil date arithmetic) | done |
| `curve/day_count` (ACT/360, ACT/365F, ACT/ACT ISDA, 30/360, 30E/360) | done |
| `curve/calendar` (SIFMA/NY-Fed holidays, Modified Following, EOM) | done |
| `curve/schedule` (deterministic rolls, T+2 spot, EOM, stubs) | **next** |

### Curve
| Task | Status |
|---|---|
| `curve/interpolation` (log-linear discount factors) | planned |
| `curve/ois_instrument` (par rate, annuity, telescoping identity) | planned |
| `curve/futures_convexity` (SR1/SR3 + futures/swap splice) | planned |
| `curve/bootstrap` (Newton + Brent, config-driven tolerance) | planned |
| real `curve_snapshot` (pillars, DFs, metadata) | planned |
| incremental rebuild from earliest changed pillar | planned |
| **QuantLib oracle harness** (test-only) | planned |

### Pricing
| Task | Status |
|---|---|
| `pricing/swap_pricer` + `Scratch` (allocation-free) | planned |
| single-swap DV01 (bump-and-revalue) | planned |

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
| determinism test (replay → byte-identical snapshot) | planned |
| no-allocation guard on the RFQ path | planned |
| benchmarks + in-process latency distribution | planned |
| Linux latency report (needs hardware) | planned |

---

## Milestone 2 — Breadth
| Task | Status |
|---|---|
| bond pricer: accrued, clean/dirty, yield-to-maturity, Z-spread, G-spread | planned |
| immutable `BookSnapshot` from the trade log | planned |
| Risk thread: DV01 + bucketed curve risk (bump-and-revalue) | planned |
| spread-to-curve per instrument | planned |

## Milestone 3 — Reliability
| Task | Status |
|---|---|
| replay log + checkpoint/recovery (kill -9 → restart → state matches) | planned |
| A/B failure injection and promotion under stress | planned |
| active-active determinism test (ADR-017) | planned |
| rolling restart / drain; watchdog; overload policy | planned |

## Milestone 4 — Depth
| Task | Status |
|---|---|
| P&L attribution (carry + curve + spread + residual) | planned |
| monotone-convex forward interpolation (Hagan–West) | planned |
| AAD / reverse-mode risk | planned |

## Milestone 5 — Performance
| Task | Status |
|---|---|
| core pinning, busy-poll sockets, memory layout, LTO/PGO | planned |
| bare-metal Linux host | planned |
| final latency report with methodology | planned |

---

## Dependency order

```
M1 ──▶ M2 ──▶ M4
 │       │
 └───────┴──▶ M3 ──▶ M5
```

M1 first, always: nothing downstream works without a real curve. M2 needs M1.
M3 needs M1+M2. M4 needs M2. M5 last — hardening a system that is not correct
yet is waste.

## Timeline (focused effort)

| | Estimate |
|---|---|
| M1 (the gate) | 3–6 weeks |
| M2 | 2–3 weeks |
| M3 | 2–3 weeks |
| M4 | 3–4 weeks |
| M5 | 2–3 weeks + hardware |
| **Full vision** | **~3–5 months** |

The wildcards: matching QuantLib's conventions (M1) and AAD (M4) are the two
places where iteration, not typing, dominates.

## What "finished" means

The project is finished when **M1 is complete**: a real USD SOFR curve, real
swap prices validated against an independent oracle, and an honest,
span-documented latency distribution. M2–M5 are additive; each is demoable on
its own. The gate is M1.

## Keeping this current

Update the relevant status cell **in the same commit** as the work it describes.
No scope is "done" until it is merged and CI-green.
