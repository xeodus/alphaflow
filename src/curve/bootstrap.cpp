#include <alphaflow/curve/bootstrap.hpp>

#include <cmath>

namespace alphaflow::curve {
namespace {

struct PillarSolution {
    double last_df{1.0};
    double last_time{0.0};
    std::size_t iterations{0};
    bool ok{false};
};

/// One Newton iteration set for the last discount factor of a par OIS pillar.
///
/// f(D)  = K·(a + δ·D) − (D0 − D)   is the swap PV residual, linear in D, where
///         a  = Σ_{j<n} δ_j·D(t_j) over the known earlier coupons,
///         D0 = D(t0), δ = the final accrual.
/// f'(D) = K·δ + 1.
double solve_last_df(double d0, double a, double delta, double rate,
                     const BootstrapSpec& spec, std::size_t& iterations) noexcept {
    const double derivative = rate * delta + 1.0;
    double d = 1.0;
    for (int iteration = 0; iteration < spec.max_iterations; ++iteration) {
        ++iterations;
        const double residual = rate * (a + delta * d) - (d0 - d);
        if (std::abs(residual) < spec.tolerance) {
            break;
        }
        d -= residual / derivative;
    }
    return d;
}

PillarSolution solve_pillar(const DiscountCurve& curve, const OisPillar& pillar,
                            const Calendar& calendar, const BootstrapSpec& spec) noexcept {
    ScheduleSpec schedule_spec;
    schedule_spec.effective = spec.reference;
    schedule_spec.termination = pillar.maturity;
    schedule_spec.frequency = spec.frequency;
    schedule_spec.convention = spec.convention;
    schedule_spec.end_of_month = spec.end_of_month;
    schedule_spec.stub = StubPolicy::ShortBack;

    const Schedule schedule = make_schedule(schedule_spec, calendar, spec.fixed_day_count);
    if (schedule.count == 0) {
        return {};
    }

    const std::size_t last = schedule.count - 1;
    const double d0 = curve.discount(schedule.periods[0].start);

    double earlier_coupons = 0.0;
    for (std::size_t j = 0; j < last; ++j) {
        earlier_coupons +=
            schedule.periods[j].accrual * curve.discount(schedule.periods[j].payment);
    }

    PillarSolution solution;
    solution.last_df = solve_last_df(d0, earlier_coupons, schedule.periods[last].accrual,
                                     pillar.par_rate, spec, solution.iterations);
    solution.last_time = curve.time_of(schedule.periods[last].end);
    solution.ok = true;
    return solution;
}

}  // namespace

BootstrapResult bootstrap(std::span<const OisPillar> pillars, const Calendar& calendar,
                          const BootstrapSpec& spec) noexcept {
    BootstrapResult result;
    result.curve.reference = spec.reference;
    result.curve.count = 1;
    result.curve.times[0] = 0.0;
    result.curve.log_dfs[0] = 0.0;  // D(reference) = 1

    for (const OisPillar& pillar : pillars) {
        if (result.curve.count >= DiscountCurve::kMaxPillars) {
            result.converged = false;
            break;
        }

        const PillarSolution solution = solve_pillar(result.curve, pillar, calendar, spec);
        if (!solution.ok) {
            continue;
        }

        result.curve.times[result.curve.count] = solution.last_time;
        result.curve.log_dfs[result.curve.count] = std::log(solution.last_df);
        ++result.curve.count;
        ++result.pillars;
        result.iterations += solution.iterations;
    }

    return result;
}

bool rebuild_from(DiscountCurve& curve, std::size_t first_changed,
                  std::span<const OisPillar> pillars, const Calendar& calendar,
                  const BootstrapSpec& spec) noexcept {
    if (first_changed >= pillars.size()) {
        return true;
    }
    for (std::size_t i = first_changed; i < pillars.size(); ++i) {
        if (i + 1 >= DiscountCurve::kMaxPillars) {
            return false;
        }
        const PillarSolution solution = solve_pillar(curve, pillars[i], calendar, spec);
        if (!solution.ok) {
            return false;
        }
        curve.times[i + 1] = solution.last_time;
        curve.log_dfs[i + 1] = std::log(solution.last_df);
    }
    return true;
}

}  // namespace alphaflow::curve
