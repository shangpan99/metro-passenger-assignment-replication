#include "search.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace metro {
namespace {

struct Alternative {
    int path;
    int departure;

    bool operator<(const Alternative& other) const {
        return std::tie(departure, path) < std::tie(other.departure, other.path);
    }

    bool operator==(const Alternative& other) const {
        return path == other.path && departure == other.departure;
    }
};

struct Point {
    int od;
    Alternative donor;
    Alternative receiver;
    std::vector<std::size_t> indices;
    double delta;
    double marginal;
    int block;

    auto key() const {
        return std::make_tuple(delta, donor.departure, donor.path,
                               receiver.departure, receiver.path, indices.size());
    }
};

struct Profile {
    Alternative donor;
    Alternative receiver;
    std::vector<std::size_t> indices;
    std::vector<Point> points;
};

using Alternatives = std::map<int, std::vector<Alternative>>;
using BudgetCheck = std::function<void()>;

void relocate(Agent& agent, Alternative receiver) {
    agent.path = receiver.path;
    agent.dep = receiver.departure;
}

Assignment relocated(const Assignment& base, const Point& point) {
    auto trial = base;
    for (auto i : point.indices)
        relocate(trial[i], point.receiver);
    return trial;
}

class Neighborhood {
public:
    Neighborhood(const Assignment& base, double baseline, const Alternatives& alts,
                 const Loader& loader, const BudgetCheck& check_budget)
        : base_(base), baseline_(baseline), alternatives_(alts), loader_(loader),
          check_budget_(check_budget) {
        for (std::size_t i = 0; i < base.size(); ++i) {
            const auto& agent = base[i];
            groups_[agent.od][{agent.path, agent.dep}].push_back(i);
        }
        for (auto& od : groups_)
            for (auto& entry : od.second)
                std::sort(entry.second.begin(), entry.second.end(),
                          [&](auto a, auto b) { return base[a].id > base[b].id; });
    }

    std::optional<Point> best(int od, int depth) {
        std::optional<Point> result;
        for_each_eligible(od, depth, [&](const Point& point) {
            if (!result || point.key() < result->key())
                result = point;
        });
        return result;
    }

    std::vector<Point> neutral(int od, int depth) {
        std::vector<Point> result;
        for_each_eligible(od, depth, [&](const Point& point) {
            if (point.delta == 0.0)
                result.push_back(point);
        });
        std::sort(result.begin(), result.end(),
                  [](const Point& a, const Point& b) { return a.key() < b.key(); });
        return result;
    }

private:
    const Assignment& base_;
    double baseline_;
    const Alternatives& alternatives_;
    const Loader& loader_;
    const BudgetCheck& check_budget_;
    std::map<int, std::map<Alternative, std::vector<std::size_t>>> groups_;
    std::map<int, std::vector<Profile>> by_od_;

    std::vector<Profile>& profiles(int od) {
        auto entry = by_od_.try_emplace(od);
        auto& result = entry.first->second;
        if (entry.second) {
            const auto& alts = alternatives_.at(od);
            for (const auto& group : groups_[od]) {
                for (const auto& receiver : alts) {
                    if (!(group.first == receiver))
                        result.push_back({group.first, receiver, group.second, {}});
                }
            }
        }
        return result;
    }

    void extend(Profile& profile, int od, int depth) {
        auto& points = profile.points;
        const auto& indices = profile.indices;
        if (points.size() == indices.size() || (!points.empty() && points.back().block > depth))
            return;

        auto trial = base_;
        for (std::size_t i = 0; i < points.size(); ++i)
            relocate(trial[indices[i]], profile.receiver);

        // Keep the first prefix in the next block for later depth scans.
        while (points.size() < indices.size()) {
            check_budget_();
            const auto q = points.size() + 1;
            relocate(trial[indices[q - 1]], profile.receiver);
            const auto score = loader_.evaluate(trial, false);
            check_budget_();
            if (!score.feasible || !std::isfinite(score.objective))
                throw std::runtime_error("Infeasible Proposed prefix; no neighborhood certificate");
            const double delta = score.objective - baseline_;
            const double marginal = delta - (points.empty() ? 0.0 : points.back().delta);
            const int block = points.empty() ? 1 :
                points.back().block + (marginal != points.back().marginal);
            points.push_back({od, profile.donor, profile.receiver,
                              {indices.begin(), indices.begin() + q}, delta, marginal, block});
            if (block > depth)
                break;
        }
    }

    template <typename Visitor>
    void for_each_eligible(int od, int depth, Visitor visit) {
        for (auto& profile : profiles(od)) {
            extend(profile, od, depth);
            for (const auto& point : profile.points) {
                if (point.block > depth)
                    break;
                visit(point);
            }
        }
    }
};


Alternatives build_alternatives(const Data& data) {
    Alternatives alternatives;
    for (const auto& [od, demand] : data.demands) {
        auto& alts = alternatives[od];
        for (int departure = demand.earliest_dep; departure <= demand.latest_dep; ++departure)
            for (const int path : demand.paths)
                alts.push_back({path, departure});
        std::sort(alts.begin(), alts.end());
    }
    return alternatives;
}

struct TwoStep {
    Point first;
    Point second;
    int second_depth;
    Assignment agents;
    double objective;
    double travel;
    double waiting;
};

std::optional<TwoStep> find_two_step(const Assignment& base, double baseline,
                                    Neighborhood& neighborhood, const Alternatives& alts,
                                    int bmax, const Loader& loader, const BudgetCheck& check_budget) {
    // Enumerate all neutral prefixes at the terminal depth, even if another
    // direction was first in an OD's ordinary tie ordering.
    for (const auto& od : alts) {
        check_budget();
        for (const auto& first : neighborhood.neutral(od.first, bmax)) {
            check_budget();
            const auto neutral = relocated(base, first);
            const auto score1 = loader.evaluate(neutral, false);
            check_budget();
            if (!score1.feasible || score1.objective != baseline)
                throw std::runtime_error("Proposed first step failed exact neutrality verification");

            Neighborhood second_neighborhood(neutral, baseline, alts, loader, check_budget);
            for (int depth = 1; depth <= bmax; ++depth) {
                for (const auto& second_od : alts) {
                    check_budget();
                    auto second = second_neighborhood.best(second_od.first, depth);
                    check_budget();
                    if (!second || second->delta >= 0.0)
                        continue;
                    auto final = relocated(neutral, *second);
                    const auto score2 = loader.evaluate(final, false);
                    check_budget();
                    if (!score2.feasible || score2.objective - baseline != second->delta)
                        throw std::runtime_error("Proposed second step failed exact improvement verification");
                    return TwoStep{first, *second, depth, std::move(final), score2.objective,
                                   score2.travel, score2.waiting};
                }
            }
        }
    }
    check_budget();
    return std::nullopt;
}

std::vector<int> moved_ids(const Assignment& base, const Point& point) {
    std::vector<int> ids;
    ids.reserve(point.indices.size());
    for (auto i : point.indices)
        ids.push_back(base[i].id);
    return ids;
}

Update make_update(const Assignment& base, const Point& point, int depth,
                   double before, double after) {
    Update update{};
    update.od = point.od;
    update.source_path = point.donor.path;
    update.source_dep = point.donor.departure;
    update.target_path = point.receiver.path;
    update.target_dep = point.receiver.departure;
    update.units = static_cast<int>(point.indices.size());
    update.distinct_changed = update.units;
    update.depth = depth;
    update.objective_before = before;
    update.objective_after = after;
    update.moved_ids = moved_ids(base, point);
    return update;
}

struct TimeLimit {};

} // namespace

SearchResult search_proposed(const Data& data, const Assignment& initial, Deadline deadline,
                             int max_updates) {
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [started] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    constexpr int bmax = 11;
    const Loader loader(data);
    const auto initial_score = loader.evaluate(initial, false);
    if (!initial_score.feasible || !std::isfinite(initial_score.objective))
        throw std::runtime_error("Initial Proposed assignment is infeasible");
    const auto alts = build_alternatives(data);
    std::vector<int> od_order;
    for (const auto& od : alts)
        od_order.push_back(od.first);
    if (od_order.empty())
        throw std::runtime_error("Proposed requires at least one OD");

    SearchResult result{};
    result.agents = initial;
    result.initial_objective = initial_score.objective;
    result.final_objective = initial_score.objective;
    result.status = "ITERATION_LIMIT";
    const BudgetCheck check_budget = [deadline] {
        if (std::chrono::steady_clock::now() >= deadline)
            throw TimeLimit{};
    };

    // The cursor is stable while scanning depths of an unchanged state. A
    // strict single move retains its OD. A committed pair restarts from OD1.
    std::size_t cursor = 0;
    try {
        while (max_updates < 0 || result.updates.size() < static_cast<std::size_t>(max_updates)) {
            Neighborhood neighborhood(result.agents, result.final_objective, alts, loader, check_budget);
            bool accepted = false;
            for (int depth = 1; depth <= bmax && !accepted; ++depth) {
                result.max_order_checked = depth;
                for (std::size_t offset = 0; offset < od_order.size(); ++offset) {
                    check_budget();
                    const auto index = (cursor + offset) % od_order.size();
                    auto point = neighborhood.best(od_order[index], depth);
                    check_budget();
                    if (!point || point->delta >= 0.0)
                        continue;
                    auto trial = relocated(result.agents, *point);
                    const auto score = loader.evaluate(trial, false);
                    check_budget();
                    if (!score.feasible || score.objective - result.final_objective != point->delta)
                        throw std::runtime_error("Proposed single move failed exact improvement verification");
                    auto update = make_update(result.agents, *point, depth,
                                              result.final_objective, score.objective);
                    update.total_travel_time = score.travel;
                    update.total_waiting_time = score.waiting;
                    update.elapsed_seconds = elapsed();
                    result.updates.push_back(std::move(update));
                    result.agents = std::move(trial);
                    result.final_objective = score.objective;
                    cursor = index;
                    accepted = true;
                    break;
                }
            }
            if (accepted)
                continue;
            auto pair = find_two_step(result.agents, result.final_objective,
                                      neighborhood, alts, bmax, loader, check_budget);
            check_budget();
            if (!pair) {
                result.status = "BLOCK_AND_NEUTRAL_TWO_STEP_LOCAL_OPTIMUM";
                return result;
            }
            auto update = make_update(result.agents, pair->first, bmax,
                                      result.final_objective, pair->objective);
            update.two_step = true;
            update.distinct_changed = 0;
            for (std::size_t i = 0; i < result.agents.size(); ++i) {
                const auto& before = result.agents[i];
                const auto& after = pair->agents[i];
                update.distinct_changed += before.path != after.path || before.dep != after.dep;
            }
            update.total_travel_time = pair->travel;
            update.total_waiting_time = pair->waiting;
            update.elapsed_seconds = elapsed();
            update.second_od = pair->second.od;
            update.second_source_path = pair->second.donor.path;
            update.second_source_dep = pair->second.donor.departure;
            update.second_target_path = pair->second.receiver.path;
            update.second_target_dep = pair->second.receiver.departure;
            update.second_units = static_cast<int>(pair->second.indices.size());
            update.second_depth = pair->second_depth;
            update.second_moved_ids = moved_ids(result.agents, pair->second);
            result.updates.push_back(std::move(update));
            result.agents = std::move(pair->agents);
            result.final_objective = pair->objective;
            cursor = 0;
        }
        result.max_order_checked = 1;  // The next ordinary round would restart at depth 1.
    } catch (const TimeLimit&) {
        result.status = "TIME_LIMIT_NOT_CERTIFIED";
    }
    return result;
}

} // namespace metro
