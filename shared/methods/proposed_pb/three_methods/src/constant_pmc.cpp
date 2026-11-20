#include "search.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace metro {
namespace {

using Clock = std::chrono::steady_clock;
using Alternative = std::pair<int, int>;  // path, departure
using Groups = std::map<int, std::map<Alternative, std::vector<std::size_t>>>;
using StateKey = std::vector<Alternative>;

struct TimeExpired {};

void check(Deadline deadline) {
    if (Clock::now() >= deadline) throw TimeExpired{};
}

struct Direction {
    int od;
    Alternative donor, receiver;
    double benefit, cost;

    double gain() const { return benefit - cost; }
    auto key() const {
        return std::make_tuple(-gain(), od, donor.second, donor.first, receiver.second, receiver.first);
    }
};

Groups group_agents(const Assignment& agents) {
    Groups groups;
    for (std::size_t i = 0; i < agents.size(); ++i) {
        const auto& agent = agents[i];
        groups[agent.od][{agent.path, agent.dep}].push_back(i);
    }
    for (auto& [od, alternatives] : groups) {
        for (auto& [alt, indices] : alternatives) {
            std::sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
                return std::tie(agents[a].rank, agents[a].id) > std::tie(agents[b].rank, agents[b].id);
            });
        }
    }
    return groups;
}

struct Journey {
    int arrival;
    std::vector<std::size_t> services;
};

// Shortcut unit diagnostics only when every other passenger's ride stays unchanged.
class UnitScorer {
public:
    UnitScorer(const Data& data, const Snapshot& baseline)
        : data_(data), usable_(data.travel_weight == 1 && data.early_weight == 0 && data.late_weight == 0)
    {
        for (const auto& [id, train] : data.trains) {
            if (std::adjacent_find(train.times.begin(), train.times.end(), std::greater_equal<int>{})
                    != train.times.end()) usable_ = false;
        }
        if (!usable_) return;
        for (std::size_t i = 0; i < baseline.services.size(); ++i) {
            const auto& service = baseline.services[i];
            by_link_[service.link].push_back(i);
            by_train_segment_.emplace(std::make_pair(service.train, service.segment), i);
        }
    }

    void prepare(const Snapshot& state) {
        unsafe_removals_.clear();
        if (!usable_) return;
        for (const auto& service : state.services) {
            // Only a service that left an eligible passenger behind can use a freed seat.
            if (service.eligible.size() > service.boarded.size())
                unsafe_removals_.insert(service.onboard.begin(), service.onboard.end());
        }
    }

    std::optional<double> minus(const Snapshot& state, int agent) const {
        if (!usable_ || unsafe_removals_.count(agent)) return std::nullopt;
        return state.agents.at(agent).objective;
    }

    std::optional<double> plus(const Snapshot& state, Alternative alt) {
        if (!usable_) return std::nullopt;
        auto found = journeys_.find(alt);
        if (found == journeys_.end())
            found = journeys_.emplace(alt, earliest_journey(state, alt)).first;
        if (!found->second) return std::nullopt;
        const auto& journey = *found->second;
        // The earliest compatible train must have a spare seat along the entire ride.
        for (auto i : journey.services)
            if (state.services[i].residual_after <= 0) return std::nullopt;
        return static_cast<double>(journey.arrival - alt.second);
    }

private:
    std::optional<Journey> earliest_journey(const Snapshot& state, Alternative alt) const {
        const auto& path = data_.paths.at(alt.first);
        Journey journey{alt.second, {}};
        bool waiting_for_run = false;
        for (std::size_t pos = 0; pos < path.links.size();) {
            const auto& link = data_.links.at(path.links[pos]);
            if (link.type != run_link) {
                if (waiting_for_run) return std::nullopt;
                waiting_for_run = link.type == access_link || link.type == transfer_link;
                journey.arrival += link.time;
                ++pos;
                continue;
            }
            if (!waiting_for_run) return std::nullopt;
            waiting_for_run = false;
            auto end = pos + 1;
            while (end < path.links.size() && data_.links.at(path.links[end]).type == run_link) ++end;
            const auto candidates = by_link_.find(link.id);
            if (candidates == by_link_.end()) return std::nullopt;
            const auto& indices = candidates->second;
            const auto first = std::lower_bound(indices.begin(), indices.end(), journey.arrival,
                [&](std::size_t i, int ready) { return state.services[i].dep < ready; });
            const auto compatible = [&](std::size_t i) {
                const auto& service = state.services[i];
                const auto& train = data_.trains.at(service.train);
                const auto segment = static_cast<std::size_t>(service.segment);
                return end - pos <= train.links.size() - segment &&
                    std::equal(path.links.begin() + pos, path.links.begin() + end, train.links.begin() + segment);
            };
            const auto chosen = std::find_if(first, indices.end(), compatible);
            if (chosen == indices.end()) return std::nullopt;
            const auto& boarding = state.services[*chosen];
            for (std::size_t i = pos; i < end; ++i) {
                const int segment = boarding.segment + static_cast<int>(i - pos);
                const auto service = by_train_segment_.at({boarding.train, segment});
                journey.services.push_back(service);
                journey.arrival = state.services[service].arr;
            }
            pos = end;
        }
        if (waiting_for_run) return std::nullopt;
        return journey;
    }

    const Data& data_;
    bool usable_;
    std::map<int, std::vector<std::size_t>> by_link_;
    std::map<std::pair<int, int>, std::size_t> by_train_segment_;
    std::map<Alternative, std::optional<Journey>> journeys_;
    std::unordered_set<int> unsafe_removals_;
};

std::optional<Direction> find_direction(const Data& data, const Loader& loader,
                                        UnitScorer& scorer, const Assignment& agents,
                                        const Snapshot& baseline, const Groups& groups,
                                        const std::map<int, int>& counts, int max_id, int max_rank,
                                        Deadline deadline, ConstantStepDetails& details) {
    std::optional<Direction> best;
    scorer.prepare(baseline);
    for (const auto& [od, demand] : data.demands) {
        if (demand.count == 0) continue;
        const auto& donors = groups.at(od);
        std::map<Alternative, double> minus, plus;
        int probes = 0, infeasible = 0;
        auto expected = counts;
        for (int path : demand.paths) {
            for (int dep = demand.earliest_dep; dep <= demand.latest_dep; ++dep) {
                check(deadline);
                const Alternative alt{path, dep};
                const auto donor = donors.find(alt);
                const auto n = donor == donors.end() ? 0 : static_cast<int>(donor->second.size());
                if (n > 0) {
                    const auto tail = donor->second.front();
                    auto value = scorer.minus(baseline, agents[tail].id);
                    if (!value) {
                        Assignment trial = agents;
                        trial.erase(trial.begin() + tail);
                        expected[od] = demand.count - 1;
                        const auto loaded = loader.evaluate(trial, expected, false);
                        check(deadline);
                        if (loaded.feasible) value = baseline.objective - loaded.objective;
                    }
                    ++probes;
                    if (value) {
                        if (!std::isfinite(*value)) throw std::runtime_error("Nonfinite first-unit minus PMC");
                        minus.emplace(alt, *value);
                    } else {
                        ++infeasible;
                    }
                }
                check(deadline);
                if (n < demand.count) {
                    auto value = scorer.plus(baseline, alt);
                    if (!value) {
                        if (max_id == std::numeric_limits<int>::max() || max_rank == std::numeric_limits<int>::max())
                            throw std::overflow_error("Cannot allocate an artificial diagnostic passenger ID or rank");
                        Assignment trial = agents;
                        trial.push_back({max_id + 1, max_rank + 1, od, path, dep});
                        expected[od] = demand.count + 1;
                        const auto loaded = loader.evaluate(trial, expected, false);
                        check(deadline);
                        if (loaded.feasible) value = loaded.objective - baseline.objective;
                    }
                    ++probes;
                    if (value) {
                        if (!std::isfinite(*value)) throw std::runtime_error("Nonfinite first-unit plus PMC");
                        plus.emplace(alt, *value);
                    } else {
                        ++infeasible;
                    }
                }
            }
        }
        details.pmc_probe_count += probes;
        details.infeasible_unit_diagnostics += infeasible;
        for (const auto& [donor, benefit] : minus) {
            for (const auto& [receiver, cost] : plus) {
                check(deadline);
                const Direction direction{od, donor, receiver, benefit, cost};
                if (donor != receiver && direction.gain() > 0 &&
                        (!best || direction.key() < best->key()))
                    best = direction;
            }
        }
    }
    return best;
}

StateKey state_key(const Assignment& agents) {
    StateKey key;
    key.reserve(agents.size());
    for (const auto& agent : agents) key.emplace_back(agent.path, agent.dep);
    return key;
}

std::string fingerprint(const StateKey& key) {
    // The diagnostic fingerprint is only a label. Detection compares full assignments.
    std::uint64_t value = 14695981039346656037ULL;
    for (const auto& [path, departure] : key) {
        for (const auto item : {static_cast<std::uint32_t>(path), static_cast<std::uint32_t>(departure)}) {
            for (unsigned shift = 0; shift < 32; shift += 8) {
                value ^= (item >> shift) & 0xffU;
                value *= 1099511628211ULL;
            }
        }
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

}  // namespace

SearchResult search_constant(const Data& data, const Assignment& initial,
                             Deadline deadline, int max_iterations) {
    if (max_iterations < 0) throw std::invalid_argument("Constant-step iteration limit must be nonnegative");
    const auto started = Clock::now();
    const Loader loader(data);
    SearchResult result;
    result.agents = initial;
    std::sort(result.agents.begin(), result.agents.end(),
              [](const Agent& a, const Agent& b) { return a.id < b.id; });
    auto baseline = loader.evaluate(result.agents);
    if (!baseline.feasible || !std::isfinite(baseline.objective))
        throw std::runtime_error("Initial constant-step assignment is infeasible");
    result.initial_objective = result.final_objective = baseline.objective;
    auto& details = result.constant_step.emplace();
    details.best_agents = result.agents;
    details.best_objective = baseline.objective;

    try {
        check(deadline);
        UnitScorer scorer(data, baseline);
        int max_od = 0, max_id = 0, max_rank = 0;
        std::map<int, int> counts;
        for (const auto& [od, demand] : data.demands) {
            counts.emplace(od, demand.count);
            max_od = std::max(max_od, demand.count);
        }
        for (const auto& agent : result.agents) {
            max_id = std::max(max_id, agent.id);
            max_rank = std::max(max_rank, agent.rank);
        }
        std::map<StateKey, int> seen;
        std::vector<double> objectives{baseline.objective};
        std::optional<Direction> direction;
        Groups groups;
        while (true) {
            check(deadline);
            const auto k = details.iterations;
            // Only an unchanged step size makes an exact repeated assignment a cycle.
            if (max_od < 2 * (static_cast<std::int64_t>(k) + 1)) {
                auto key = state_key(result.agents);
                const auto [visit, inserted] = seen.emplace(std::move(key), k);
                if (!inserted) {
                    const auto start = visit->second;
                    const auto [lo, hi] = std::minmax_element(objectives.begin() + start, objectives.end());
                    details.cycle = ConstantCycle{start, k, fingerprint(visit->first), *lo, *hi};
                    result.status = "CYCLE_DETECTED_NOT_CERTIFIED";
                    break;
                }
            }
            if (k >= max_iterations) {
                result.status = "ITERATION_LIMIT_NOT_CERTIFIED";
                break;
            }
            // After an infeasible attempt the unchanged accepted state retains the same unit direction.
            if (!direction) {
                groups = group_agents(result.agents);
                direction = find_direction(data, loader, scorer, result.agents, baseline, groups, counts,
                                           max_id, max_rank, deadline, details);
                if (!direction) {
                    result.status = "NO_POSITIVE_APPROXIMATE_DIRECTION";
                    break;
                }
            }
            const auto& move = *direction;
            const auto& donor = groups.at(move.od).at(move.donor);
            const int n = static_cast<int>(donor.size());
            const int q = std::max(1, n / (k + 1));
            Assignment candidate = result.agents;
            ConstantAttempt attempt;
            attempt.iteration = k + 1;
            attempt.od = move.od;
            attempt.source_path = move.donor.first;
            attempt.source_dep = move.donor.second;
            attempt.target_path = move.receiver.first;
            attempt.target_dep = move.receiver.second;
            attempt.units = q;
            attempt.donor_count = n;
            attempt.unit_minus_benefit = move.benefit;
            attempt.unit_plus_cost = move.cost;
            attempt.unit_gain = move.gain();
            attempt.objective_before = baseline.objective;
            for (int i = 0; i < q; ++i) {
                auto& agent = candidate[donor[i]];
                agent.path = move.receiver.first;
                agent.dep = move.receiver.second;
                attempt.moved_ids.push_back(agent.id);
            }
            std::sort(attempt.moved_ids.begin(), attempt.moved_ids.end());

            check(deadline);
            auto after = loader.evaluate(candidate);
            check(deadline);
            attempt.accepted = after.feasible;
            if (attempt.accepted) {
                if (!std::isfinite(after.objective))
                    throw std::runtime_error("Feasible constant-step assignment has a nonfinite objective");
                attempt.candidate_objective = after.objective;
                const auto delta = after.objective - baseline.objective;
                if (delta > 0) ++details.uphill_updates;
                else if (delta < 0) ++details.downhill_updates;
                else ++details.equal_cost_updates;
                details.moved_agents_total += q;
                Update update;
                update.od = attempt.od;
                update.source_path = attempt.source_path;
                update.source_dep = attempt.source_dep;
                update.target_path = attempt.target_path;
                update.target_dep = attempt.target_dep;
                update.units = q;
                update.distinct_changed = q;
                update.objective_before = baseline.objective;
                update.objective_after = after.objective;
                update.total_travel_time = after.travel;
                update.total_waiting_time = after.waiting;
                update.moved_ids = attempt.moved_ids;
                update.accepted_by = "constant_first_unit_pmc";
                update.surrogate_objective_delta = -q * move.gain();
                update.elapsed_seconds = std::chrono::duration<double>(Clock::now() - started).count();
                result.updates.push_back(std::move(update));
                result.agents = std::move(candidate);
                baseline = std::move(after);
                direction.reset();
                if (baseline.objective < details.best_objective) {
                    details.best_agents = result.agents;
                    details.best_objective = baseline.objective;
                    details.best_iteration = attempt.iteration;
                }
            } else {
                ++details.infeasible_steps;
            }
            details.iterations = attempt.iteration;
            result.final_objective = baseline.objective;
            attempt.objective_after = baseline.objective;
            attempt.best_objective = details.best_objective;
            attempt.elapsed_seconds = std::chrono::duration<double>(Clock::now() - started).count();
            details.attempts.push_back(std::move(attempt));
            objectives.push_back(baseline.objective);
        }
    } catch (const TimeExpired&) {
        result.status = "TIME_LIMIT_NOT_CERTIFIED";
    }
    return result;
}

}  // namespace metro
