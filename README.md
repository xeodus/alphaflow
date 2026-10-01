# AlphaFlow

AlphaFlow is a real-time pricing and risk service for the USD SOFR interest-rate
curve. It ingests a live rates feed, bootstraps a discount curve from market
quotes, prices a portfolio of interest-rate swaps and bonds against that curve,
computes curve risk, and answers request-for-quote (RFQ) messages on a path with
a bounded latency budget.

The project is an exercise in building a front-office rates system to production
engineering standards: deterministic output, honest latency measurement, and
correctness verified against an independent reference implementation.

**Status: design complete, implementation not started.** The architecture is
frozen at v0.2 (see [`ARCHITECTURE.md`](ARCHITECTURE.md)). The repository
currently contains the build skeleton only; the first vertical slice is the next
milestone.

---

## The problem

A rates desk asks two very different kinds of question, at very different rates.

The first is pricing: *what is this swap worth right now, and at what price will
you show it?* This is asked constantly, and the answer is only useful if it
arrives quickly and predictably. A quote that arrives in 40 microseconds on
average but occasionally takes 4 milliseconds is worse than a slower, steadier
one, because the tail is what a desk actually plans around.

The second is risk and attribution: *how much would we lose if the curve moved a
basis point, and where did yesterday's profit and loss come from?* These are
asked rarely, but the answers are expensive to compute and must be exactly right.

The two are easy to run on one thread, and that is the trap. Bootstrapping the
curve and repricing the book are not cheap, and if they sit in front of the quote
handler, then quote latency is coupled to curve size and book size. The tail
latency becomes something you hope for rather than something you control.

AlphaFlow is built around separating them.

## The central idea

The RFQ path never does work proportional to curve size, the book, or the feed
rate. It reads a pointer to an already-built, immutable snapshot of the curve and
prices the one instrument it was asked about. Everything expensive — bootstrap,
full-book risk, profit-and-loss attribution — runs on its own thread and
*publishes* an immutable result that readers pick up with a single atomic load.

```
  FeedThread ──SPSC──▶ CurveThread ──atomic ptr──▶ CurveSnapshot (immutable)
                                                         │
                          ┌──────────────────────────────┼───────────────────┐
                          ▼                              ▼                   ▼
                     RFQThread(s)                   RiskThread         (replay/audit)
                     read-only, O(cashflows)        read-only, batch
```

One decision — publish immutable snapshots, never share mutable state on the hot
path — is what makes latency, reliability, and scalability fall out together. It
is recorded as ADR-001 in the architecture document, and the rest of the design
exists to support it.

## What the system does

The processing pipeline runs from a raw market-data tick to a measured quote.

**Market data ingestion.** A feed line arrives on a dedicated thread, is
timestamped at the moment of arrival (before any parsing), and is normalized into
a `Tick`. Two independent feed lines, A and B, provide redundancy. A separate
arbiter thread compares their sequence numbers, detects gaps, promotes the healthy
line when the other stalls, and de-duplicates ticks across the join. The arbiter
is the single writer of the live quote cache.

**Curve construction.** USD SOFR discount curve from the overnight SOFR fixing,
SR1 and SR3 SOFR futures at the short end, and SOFR OIS swaps from one week to
fifty years. Construction is a sequential bootstrap: walking from short to long,
each pillar's discount factor is solved so that its instrument prices to par given
the factors already found. The solver is Newton's method with an analytic
derivative and Brent's method as a bracketing fallback, with a convexity adjustment applied to
the futures. Because the discount-factor interpolation (log-linear) has only local
support, a change to a pillar invalidates that pillar and everything after it, so
the curve is rebuilt incrementally from the first changed pillar rather than from
scratch.

**Pricing.** A stateless, allocation-free pricing library operates directly on an
immutable curve snapshot. Swaps are priced from the fixed-leg annuity and the
telescoping identity for the compounded floating leg. Bonds are priced for clean
and dirty value, yield to maturity by Newton, and Z-spread by a root-find against
the curve.

**Spread.** Every instrument carries a spread to the curve: Z-spread for bonds,
and the difference between par swap rate and OIS for swaps.

**Risk.** DV01 and bucketed curve risk for the whole book, computed by bumping
each curve pillar and repricing, using central differences. This is deliberately
the simple, defensible method. An adjoint (reverse-mode) sensitivity pass is
planned for a later milestone, once the direct method has been validated.

**RFQ.** Busy-polled socket handling, one worker per core, reading the latest
snapshot and returning a price. Every response carries the generation of the
snapshot it was priced from, and every response is an explicit outcome — priced,
stale, warming up, unknown instrument, or overloaded — so a client never receives
a fabricated price.

**P&L attribution.** An end-of-day decomposition of the change in book value into
carry, curve movement, and spread movement, with any unexplained residual raised
as an alert rather than absorbed as rounding.

**Measurement.** Per-stage latency histograms, sampled without logging on the hot
path, reporting the full distribution rather than a single number.

## Engineering principles

The design follows a small number of rules, applied without exception. The full
set is in [`ARCHITECTURE.md`](ARCHITECTURE.md) §3; the ones that shape everything
else are these.

*Single writer, immutable snapshots.* Every piece of mutable state has exactly one
owning thread. Anyone else reads a published snapshot or a lock-free cell. If two
threads ever need the same value, that is a design error, not a synchronisation
problem to solve with a lock.

*Derived state is disposable.* The curve and the risk numbers are pure functions
of the feed, the positions, and the code. Only the inputs are persisted. Recovery
is replay, and high availability is replication of inputs, not of state. There is
no consensus protocol and no shared broker because there is nothing to agree on.

*Nothing on the hot path allocates, locks, throws, or touches the disk.* This
includes the replay log: accepted ticks are handed to a low-priority writer thread
through a bounded queue, so the ingest thread never risks a page fault or a
filesystem stall while it is also producing the timestamps the latency numbers
depend on.

*Determinism is a contract.* The same inputs must produce a byte-identical curve
and quote on any replica. This is enforced by a hermetic build, fixed
floating-point contraction, stable iteration order, and a replay test in
continuous integration — not merely assumed because the code is the same.

*Two clocks, never mixed.* A monotonic clock for latency and a wall clock for
business dates and P&L, both captured at tick arrival.

## Performance and how it is measured

Latency claims are only meaningful when the measurement span is stated. AlphaFlow
defines RFQ latency as the interval from reading the request bytes off the socket
to handing the response bytes to the network interface, and labels any smaller
in-process number as such. It reports p50, p90, p99, p99.9, and maximum per stage,
because the tail is what a trading desk plans around.

The performance platform is **Linux x86-64** with isolated cores, busy-polling
sockets, prefaulted snapshot memory, and link-time optimisation. macOS is
supported for development and correctness testing but not for performance claims.
The exception is deliberate: the mechanisms that make the tail tight — invariant
TSC, CPU pinning, socket busy-poll — either do not exist or behave differently on
macOS, and a number produced on the wrong platform is not worth reporting.

The architecture takes a position on the headline target: a previous draft
suggested roughly 6 microseconds at p99 is attainable, and the design treats that
as plausible but unproven. The histogram produces the number; the project does not
assert it in advance.

## Reliability

A capital-markets system is not measured by uptime percentage but by its recovery
time and recovery point when something fails. AlphaFlow's approach is redundancy
plus fast deterministic recovery.

Two or more identical engine processes run active-active on one host, each with
its own feed adapters. Because the engine is deterministic, either process can
answer any quote with the same result, so there is nothing to fail over to and
nothing to elect. Rolling restarts drain quotes to peers and rejoin without
migrating state, since state is rebuilt by replay. Under overload the system
prefers returning an explicitly stale, timestamped quote to blocking, on the
principle that a frozen desk is worse than an old price.

## Correctness and testing

Silent pricing errors are the expensive kind, so the test strategy is layered.
Day-count conventions, calendars, and schedule generation are tested exhaustively
against hand-computed values. Curve and pricing results are validated against
QuantLib as an independent oracle — linked into the test suite only, never into
the engine. Root finders and interpolation are checked analytically and with
property tests. Concurrency is stressed under ThreadSanitizer, memory under
AddressSanitizer and UndefinedBehaviorSanitizer, and determinism is verified by
replaying the same log twice and requiring byte-identical output.

## Roadmap

Work is organised into milestones, each a working system rather than a layer.

- **M1 — Vertical slice.** Synthetic feed through the arbiter and pricing queue
  into an incremental OIS bootstrap, a swap RFQ served over loopback, and a real
  latency histogram. Exits when the curve matches the oracle to within 1e-9, the
  swap identities hold, the sanitizers are clean, and one Linux latency report
  exists with a documented methodology.
- **M2 — Breadth.** Bonds, Z-spread, and full DV01 and bucketed risk on their own
  thread.
- **M3 — Reliability.** Replay logging and checkpoint recovery, feed arbitration
  under injected failure, cross-replica determinism, and rolling restart.
- **M4 — Depth.** P&L attribution, monotone-convex forward interpolation, and
  adjoint risk.
- **M5 — Performance hardening.** Core pinning, busy-polling, memory layout, and
  the final Linux latency report.

## Repository layout

```
alphaflow/
  CMakeLists.txt         build entry point
  ARCHITECTURE.md        design authority and decision log
  include/alphaflow/
    core/                types, dual clock, errors, configuration
    concurrency/         SPSC ring, seqlock, pooled snapshot pointers
    market/              ticks, feed sources, arbiter, replay log
    curve/               day counts, calendars, schedules, interpolation,
                         instruments, convexity, bootstrap, curve snapshot
    pricing/             swap and bond pricers, spreads
    risk/                DV01, bucketed risk, book snapshot
    pnl/                 attribution
    rfq/                 responder, server, wire protocol
    metrics/             histograms, latency recorder
    platform/            clock and TSC calibration, affinity, sockets
  src/                   implementations, main
  tests/                 unit, integration, benchmark, oracle
  tools/                 synthetic data generator, replay driver
```

## Building

Requirements: CMake 3.25 or newer, Ninja, a C++23 compiler (GCC 13+, Clang
16+, or Apple Clang), and a checkout of vcpkg at `$HOME/vcpkg`. Dependencies
are declared in `vcpkg.json` and pinned to a specific vcpkg baseline, so the
build resolves the same versions everywhere.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/src/alpha
```

The presets are `dev` (Debug, warnings-as-errors), `release`, `asan`, `ubsan`,
`tsan`, and `bench`. Each writes to its own build directory under `build/`.
Continuous integration builds `dev`, `release`, `asan`, `ubsan`, and `tsan` on
Linux, and `dev` on macOS.

To run that whole matrix locally before pushing:

```sh
scripts/ci.sh
```

CI is pinned to `ubuntu-24.04` (GCC 13 / libstdc++ 13) and `macos-15`, so the
toolchain baseline is explicit. Avoid standard-library features newer than that
baseline — `<print>` is GCC 14+, for example.

At this stage the build produces a small binary that reports its monotonic
clock source; the engine itself is being built out milestone by milestone.

## Documentation

[`ARCHITECTURE.md`](ARCHITECTURE.md) is the authoritative design document. It
contains the problem statement, the component and thread model, the curve and
pricing design, the reliability and observability strategy, the testing plan, and
a decision log of every architectural choice marked with an ADR identifier.
Where this README and the architecture document disagree, the architecture
document wins.

[`M1_PLAN.md`](M1_PLAN.md) schedules the M1 milestone: its phases, the task in
each, and where every M1 exit criterion is earned.
