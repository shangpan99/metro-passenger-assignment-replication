#include "search.hpp"
#include "input.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace metro {
namespace {

constexpr int maximum_order = 8;
constexpr double plateau_tolerance = 1e-6;

struct TimeExpired {};

void check(Deadline deadline)
{
    if (std::chrono::steady_clock::now() >= deadline)
        throw TimeExpired{};
}

// Ordering the departure before the path reproduces the original tie break.
using Alternative = std::pair<int, int>;
using PairKey = std::tuple<int, Alternative, Alternative>;

Alternative alternative(const Agent& agent)
{
    return {agent.dep, agent.path};
}

std::vector<int> donor_indices(const Assignment& agents, int od, Alternative alt)
{
    std::vector<int> donors;
    for (int i = 0; i < static_cast<int>(agents.size()); ++i) {
        if (agents[i].od == od && alternative(agents[i]) == alt)
            donors.push_back(i);
    }
    std::sort(donors.begin(), donors.end(), [&](int left, int right) {
        const Agent& a = agents[left];
        const Agent& b = agents[right];
        return std::tie(a.rank, a.id) > std::tie(b.rank, b.id);
    });
    return donors;
}

std::vector<Alternative> alternatives(const Demand& demand)
{
    std::vector<Alternative> result;
    for (int dep = demand.earliest_dep; dep <= demand.latest_dep; ++dep) {
        for (int path : demand.paths)
            result.emplace_back(dep, path);
    }
    std::sort(result.begin(), result.end());
    return result;
}

struct Marginal {
    double value = 0, direct = 0, nonwaiting = 0, schedule = 0;
    std::map<QueueKey, double> waiting;
    std::vector<std::array<int, 4>> event_response;

    bool same_components(const Marginal& other) const
    {
        // All event and walking times in the supplied experiment are integral.
        // The maps omit zero entries, so exact equality is sufficient for waiting.
        return std::abs(direct - other.direct) <= 1e-8 &&
               std::abs(nonwaiting - other.nonwaiting) <= 1e-8 &&
               std::abs(schedule - other.schedule) <= 1e-8 &&
               waiting == other.waiting;
    }
};

std::array<int, 4> service_counts(const Service& service)
{
    return {static_cast<int>(service.onboard.size()),
            static_cast<int>(service.through.size()),
            static_cast<int>(service.eligible.size()),
            static_cast<int>(service.eligible.size()) - service.residual_before};
}

Marginal marginal(const Snapshot& before, const Snapshot& after, int changed, bool plus)
{
    Marginal result;
    const int sign = plus ? 1 : -1;
    result.value = sign * (after.objective - before.objective);
    result.schedule = sign * (after.schedule_cost - before.schedule_cost);
    result.direct = (plus ? after : before).agents.at(changed).travel;

    for (const auto& [id, old_agent] : before.agents) {
        if (id == changed)
            continue;
        const auto& new_agent = after.agents.at(id);
        result.nonwaiting += sign * (new_agent.nonwaiting - old_agent.nonwaiting);
        if (old_agent.episodes.size() != new_agent.episodes.size())
            throw std::logic_error("Isolated SAR-PMC probe changed a passenger's queue topology");
        for (std::size_t i = 0; i < old_agent.episodes.size(); ++i) {
            const Episode& a = old_agent.episodes[i];
            const Episode& b = new_agent.episodes[i];
            if (!(a.queue == b.queue))
                throw std::logic_error("Isolated SAR-PMC probe changed a passenger's queue");
            const int wait_change = (b.board - b.ready) - (a.board - a.ready);
            if (wait_change)
                result.waiting[a.queue] += sign * wait_change;
        }
    }
    for (auto it = result.waiting.begin(); it != result.waiting.end();) {
        if (it->second == 0)
            it = result.waiting.erase(it);
        else
            ++it;
    }

    if (before.services.size() != after.services.size())
        throw std::logic_error("Timetable service count changed between SAR-PMC probes");
    result.event_response.reserve(before.services.size());
    for (std::size_t i = 0; i < before.services.size(); ++i) {
        const auto old_counts = service_counts(before.services[i]);
        const auto new_counts = service_counts(after.services[i]);
        std::array<int, 4> change{};
        for (std::size_t count = 0; count < change.size(); ++count)
            change[count] = new_counts[count] - old_counts[count];
        result.event_response.push_back(change);
    }
    return result;
}

struct Direction {
    int h1_bound = 0;
    bool exhausted = false;
    std::vector<double> costs;

    int order_bound(int order) const
    {
        if (order == 1)
            return h1_bound;
        int blocks = 0;
        double block_cost = 0;
        for (int i = 0; i < static_cast<int>(costs.size()); ++i) {
            if (i == 0 || std::abs(costs[i] - block_cost) > plateau_tolerance) {
                if (++blocks > order)
                    return i;
                block_cost = costs[i];
            }
        }
        return static_cast<int>(costs.size());
    }
};

using Curves = std::map<Alternative, std::pair<Direction, Direction>>;

class Profiles {
public:
    Profiles(const Data& data, const Loader& loader, Deadline deadline)
        : data_(data), loader_(loader), deadline_(deadline)
    {
        for (const auto& [od, demand] : data.demands)
            counts_.emplace(od, demand.count);
    }

    void populate(Curves& curves, const Assignment& agents, const Snapshot& accepted,
                  int od, int order) const
    {
        int max_id = 0;
        int max_rank = 0;
        for (const Agent& agent : agents) {
            max_id = std::max(max_id, agent.id);
            max_rank = std::max(max_rank, agent.rank);
        }
        for (Alternative alt : alternatives(data_.demands.at(od))) {
            check(deadline_);
            const auto donors = donor_indices(agents, od, alt);
            auto& [plus, minus] = curves[alt];
            build(plus, agents, accepted, od, alt, donors, max_id, max_rank, true, order);
            build(minus, agents, accepted, od, alt, donors, max_id, max_rank, false, order);
        }
    }

private:
    void build(Direction& curve, const Assignment& agents, const Snapshot& accepted,
               int od, Alternative alt, const std::vector<int>& donors, int max_id,
               int max_rank, bool plus, int order) const
    {
        const int donor_count = static_cast<int>(donors.size());
        const int maximum = plus ? counts_.at(od) - donor_count : donor_count;
        const int known = static_cast<int>(curve.costs.size());
        if (curve.exhausted || (order > 1 && curve.order_bound(order) < known))
            return;

        Assignment work = agents;
        const auto apply_unit = [&](int unit) {
            if (plus) {
                const int id = max_id + unit;
                work.push_back({id, max_rank + unit, od, alt.second, alt.first});
                return id;
            }
            const int id = agents[donors[unit - 1]].id;
            work.erase(std::remove_if(work.begin(), work.end(),
                       [&](const Agent& agent) { return agent.id == id; }), work.end());
            return id;
        };

        auto expected = counts_;
        const int sign = plus ? 1 : -1;
        Snapshot current;
        const Snapshot* previous = &accepted;
        if (known > 0) {
            for (int unit = 1; unit <= known; ++unit)
                apply_unit(unit);
            expected[od] += sign * known;
            check(deadline_);
            current = loader_.evaluate(work, expected, order == 1);
            check(deadline_);
            if (!current.feasible)
                throw std::logic_error("Previously feasible SAR-PMC diagnostic prefix changed");
            previous = &current;
        }

        Marginal first;
        bool event_prefix_open = true;
        for (int unit = known + 1; unit <= maximum; ++unit) {
            check(deadline_);
            const int changed = apply_unit(unit);
            expected[od] += sign;
            Snapshot next = loader_.evaluate(work, expected, order == 1);
            check(deadline_);
            if (!next.feasible) {
                curve.exhausted = true;
                return;
            }
            if (order == 1) {
                Marginal cost = marginal(*previous, next, changed, plus);
                curve.costs.push_back(cost.value);
                if (unit == 1)
                    first = std::move(cost);
                else if (!first.same_components(cost))
                    return;
                else if (first.event_response != cost.event_response)
                    event_prefix_open = false;
                if (event_prefix_open)
                    curve.h1_bound = unit;
            } else {
                curve.costs.push_back(sign * (next.objective - previous->objective));
                if (curve.order_bound(order) < unit)
                    return;
            }
            current = std::move(next);
            previous = &current;
        }
        curve.exhausted = true;
    }

    const Data& data_;
    const Loader& loader_;
    Deadline deadline_;
    std::map<int, int> counts_;
};

using PrefixMap = std::map<QueueKey, std::map<int, int>>;
using NodeTrains = std::map<int, std::map<int, std::set<int>>>;

bool same_episode_prefix(const Episode& a, const Episode& b, bool already_boarded)
{
    return a.queue == b.queue && a.source_link == b.source_link &&
           a.source_type == b.source_type && a.block == b.block &&
           a.ready == b.ready &&
           (!already_boarded || (a.board == b.board && a.train == b.train));
}

std::size_t active_episode(const AgentResult& agent, const Service& service, bool through)
{
    for (std::size_t i = 0; i < agent.episodes.size(); ++i) {
        const Episode& episode = agent.episodes[i];
        if (through) {
            const auto link = std::find(episode.block.begin(), episode.block.end(), service.link);
            if (episode.train == service.train && episode.board <= service.dep &&
                link != episode.block.end() && link != episode.block.begin())
                return i;
        } else if (episode.queue == service.queue && episode.ready <= service.dep &&
                   episode.board >= service.dep) {
            return i;
        }
    }
    throw std::logic_error("SAR-PMC cannot identify the queue episode of a service input");
}

bool same_input_tokens(const Snapshot& before, const Snapshot& after,
                       const Service& a, const Service& b, bool through)
{
    const auto& left = through ? a.through : a.waiting;
    const auto& right = through ? b.through : b.waiting;
    if (left != right)
        return false;
    for (int id : left) {
        const AgentResult& old_agent = before.agents.at(id);
        const AgentResult& new_agent = after.agents.at(id);
        if (alternative(old_agent.agent) != alternative(new_agent.agent))
            return false;
        const auto i = active_episode(old_agent, a, through);
        if (i != active_episode(new_agent, b, through))
            return false;
        // A FIFO input token contains the completed journey prefix. Waiting
        // inputs have yet to board at their current episode.
        for (std::size_t j = 0; j <= i; ++j) {
            if (!same_episode_prefix(old_agent.episodes[j], new_agent.episodes[j], through || j < i))
                return false;
        }
    }
    return true;
}

class AffectedODs {
public:
    AffectedODs(const Data& data, const Snapshot& baseline, Deadline deadline)
        : data_(data), node_trains_(realized_od_node_trains(data, deadline))
    {
        check(deadline);
        for (const Service& service : baseline.services) {
            auto& positions = positions_[service.queue];
            if (!positions.count(service.train)) {
                auto& order = trains_[service.queue];
                positions[service.train] = static_cast<int>(order.size());
                order.push_back(service.train);
            }
        }
    }

    std::set<int> find(const Snapshot& before, const Snapshot& after, int active_od,
                       const std::set<int>& changed, int donor_path, int receiver_path) const
    {
        std::set<int> anchors;
        std::set<int> nodes;
        for (int path : {donor_path, receiver_path}) {
            const auto& route_nodes = data_.paths.at(path).nodes;
            nodes.insert(route_nodes.begin(), route_nodes.end());
        }

        int passive_depth = 0;
        for (const auto& [id, old_agent] : before.agents) {
            const AgentResult& new_agent = after.agents.at(id);
            if (changed.count(id)) {
                for (const auto* agent : {&old_agent, &new_agent}) {
                    for (const Episode& episode : agent->episodes) {
                        anchors.insert(episode.train);
                        nodes.insert(episode.queue.platform);
                    }
                }
                continue;
            }
            if (old_agent.episodes.size() != new_agent.episodes.size())
                throw std::logic_error("Unmoved passenger changed queue topology");
            for (std::size_t i = 0; i < old_agent.episodes.size(); ++i) {
                const Episode& a = old_agent.episodes[i];
                const Episode& b = new_agent.episodes[i];
                if (!(a.queue == b.queue) || a.train != b.train) {
                    anchors.insert(a.train);
                    anchors.insert(b.train);
                    nodes.insert(a.queue.platform);
                    nodes.insert(b.queue.platform);
                }
                if (a.queue == b.queue && a.train != b.train) {
                    const auto& order = positions_.at(a.queue);
                    passive_depth = std::max(passive_depth, std::abs(order.at(a.train) - order.at(b.train)));
                }
            }
        }

        for (std::size_t i = 0; i < before.services.size(); ++i) {
            const Service& a = before.services[i];
            const Service& b = after.services[i];
            if (!same_input_tokens(before, after, a, b, false) ||
                !same_input_tokens(before, after, a, b, true)) {
                anchors.insert(a.train);
                nodes.insert(a.queue.platform);
            }
        }

        std::map<int, std::set<int>> expanded_by_node;
        for (int node : nodes) {
            auto& expanded = expanded_by_node[node];
            for (int id : anchors) {
                const auto& train = data_.trains.at(id);
                const QueueKey key{node, train.line, train.direction};
                const auto queue = positions_.find(key);
                if (queue == positions_.end())
                    continue;
                const auto pos = queue->second.find(id);
                if (pos == queue->second.end())
                    continue;
                const auto& order = trains_.at(key);
                const int lo = std::max(0, pos->second - passive_depth);
                const int hi = std::min(static_cast<int>(order.size()), pos->second + passive_depth + 1);
                expanded.insert(order.begin() + lo, order.begin() + hi);
            }
        }

        std::set<int> result{active_od};
        for (const auto& [od, reachable] : node_trains_) {
            for (const auto& [node, trains] : expanded_by_node) {
                const auto match = reachable.find(node);
                if (match != reachable.end() && std::any_of(trains.begin(), trains.end(),
                    [&](int train) { return match->second.count(train) > 0; })) {
                    result.insert(od);
                    break;
                }
            }
        }
        return result;
    }

private:
    const Data& data_;
    NodeTrains node_trains_;
    PrefixMap positions_;
    std::map<QueueKey, std::vector<int>> trains_;
};

struct Pair {
    int od;
    Alternative donor, receiver;
    double gain;
};

std::vector<Pair> pairs(const Curves& profile, int od, int order, Deadline deadline)
{
    std::vector<Pair> result;
    for (const auto& [donor, donor_curves] : profile) {
        check(deadline);
        const Direction& minus = donor_curves.second;
        if (!minus.order_bound(order))
            continue;
        for (const auto& [receiver, receiver_curves] : profile) {
            const Direction& plus = receiver_curves.first;
            if (donor == receiver || !plus.order_bound(order))
                continue;
            const double gain = minus.costs.front() - plus.costs.front();
            if (order != 1 || gain > 0)
                result.push_back({od, donor, receiver, gain});
        }
    }
    if (order == 1) {
        std::sort(result.begin(), result.end(), [](const Pair& a, const Pair& b) {
            return std::tuple(-a.gain, a.donor, a.receiver) <
                   std::tuple(-b.gain, b.donor, b.receiver);
        });
    }
    return result;
}

struct Batch {
    Pair pair;
    int units = 0;
    double delta = 0;

    auto key() const { return std::tie(delta, pair.od, pair.donor, pair.receiver, units); }
    bool operator>(const Batch& other) const { return key() > other.key(); }
};

Batch solve(const Curves& curves, const Pair& pair, int order, const std::set<int>& excluded)
{
    const Direction& plus = curves.at(pair.receiver).first;
    const Direction& minus = curves.at(pair.donor).second;
    const int upper = std::min(plus.order_bound(order), minus.order_bound(order));
    Batch best{pair};
    double cumulative_plus = 0, cumulative_minus = 0;
    // The one-variable integer PWL objective is defined by these knots.
    // Scanning integer q reproduces the original OPTIMAL knot audit and tie break.
    for (int units = 1; units <= upper; ++units) {
        cumulative_plus += plus.costs[units - 1];
        cumulative_minus += minus.costs[units - 1];
        const double delta = cumulative_plus - cumulative_minus;
        if (delta < best.delta && !excluded.count(units)) {
            best.units = units;
            best.delta = delta;
        }
    }
    return best;
}

struct CachedProfile {
    int iteration = -1;
    Curves curves;
};

class Search {
public:
    Search(const Data& data, const Assignment& initial, Deadline deadline, int max_updates)
        : data_(data), loader_(data), deadline_(deadline), max_updates_(max_updates),
          builder_(data, loader_, deadline)
    {
        result_.agents = initial;
    }

    SearchResult run()
    {
        snapshot_ = loader_.evaluate(result_.agents);
        if (!snapshot_.feasible)
            throw std::runtime_error("Initial SAR-PMC assignment is infeasible");
        result_.initial_objective = result_.final_objective = snapshot_.objective;

        try {
            check(deadline_);
            AffectedODs detector(data_, snapshot_, deadline_);
            detector_ = &detector;
            for (const auto& [od, demand] : data_.demands) {
                if (demand.count > 0) {
                    ods_.push_back(od);
                    dirty_.insert(od);
                }
            }
            result_.status = optimize();
        } catch (const TimeExpired&) {
            result_.status = "TIME_LIMIT_NOT_CERTIFIED";
        }
        return std::move(result_);
    }

private:
    const Curves& profile(int od, int order, bool force = false)
    {
        auto& cache = profiles_[od];
        const int iteration = static_cast<int>(result_.updates.size());
        if (force || dirty_.count(od) || cache.iteration < 0 ||
            (order > 1 && cache.iteration != iteration)) {
            cache.curves.clear();
            builder_.populate(cache.curves, result_.agents, snapshot_, od, 1);
            cache.iteration = iteration;
            dirty_.erase(od);
        }
        if (order > 1)
            builder_.populate(cache.curves, result_.agents, snapshot_, od, order);
        return cache.curves;
    }

    Batch best_batch(const Curves& curves, const Pair& pair, int order)
    {
        check(deadline_);
        return solve(curves, pair, order, excluded_[{pair.od, pair.donor, pair.receiver}]);
    }

    bool attempt(const Batch& batch, int order)
    {
        const auto& [od, donor, receiver, gain] = batch.pair;
        Assignment candidate = result_.agents;
        auto indices = donor_indices(candidate, od, donor);
        if (batch.units > static_cast<int>(indices.size()))
            throw std::logic_error("SAR-PMC proposal exceeds the available donor flow");
        std::set<int> changed;
        for (int i = 0; i < batch.units; ++i) {
            Agent& agent = candidate[indices[i]];
            changed.insert(agent.id);
            agent.dep = receiver.first;
            agent.path = receiver.second;
        }

        check(deadline_);
        Snapshot next = loader_.evaluate(candidate);
        check(deadline_);
        if (!next.feasible || !(next.objective < snapshot_.objective)) {
            excluded_[{od, donor, receiver}].insert(batch.units);
            return false;
        }

        const auto affected = detector_->find(snapshot_, next, od, changed, donor.second, receiver.second);
        check(deadline_);
        Update update;
        update.od = od;
        update.source_path = donor.second;
        update.source_dep = donor.first;
        update.target_path = receiver.second;
        update.target_dep = receiver.first;
        update.units = batch.units;
        update.depth = order;
        update.objective_before = snapshot_.objective;
        update.objective_after = next.objective;
        update.moved_ids.assign(changed.begin(), changed.end());
        update.total_travel_time = next.travel;
        update.total_waiting_time = next.waiting;
        update.elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        update.accepted_by = order == 1 ? "H1_OD_INNER_PAIR"
                                        : "H" + std::to_string(order) + "_PAIR_ESCAPE";
        update.surrogate_objective_delta = batch.delta;
        result_.updates.push_back(std::move(update));
        snapshot_ = std::move(next);
        result_.agents = std::move(candidate);
        result_.final_objective = snapshot_.objective;
        dirty_.insert(affected.begin(), affected.end());
        stalled_.clear();
        excluded_.clear();
        rescanned_ = false;
        result_.max_order_checked = 0;
        return true;
    }

    int choose_od()
    {
        const auto dirty = dirty_;
        for (int od : dirty)
            profile(od, 1);
        int best_od = 0;
        double best_gain = 0;
        for (int od : ods_) {
            check(deadline_);
            if (stalled_.count(od))
                continue;
            const auto ranking = pairs(profile(od, 1), od, 1, deadline_);
            if (ranking.empty()) {
                stalled_.insert(od);
            } else if (ranking.front().gain > best_gain) {
                best_gain = ranking.front().gain;
                best_od = od;
            }
        }
        return best_od;
    }

    bool improve_h1(int od)
    {
        const Curves& curves = profile(od, 1);
        for (const Pair& pair : pairs(curves, od, 1, deadline_)) {
            while (true) {
                const Batch batch = best_batch(curves, pair, 1);
                if (!batch.units)
                    break;
                if (attempt(batch, 1))
                    return true;
            }
        }
        return false;
    }

    bool escape(int order)
    {
        std::priority_queue<Batch, std::vector<Batch>, std::greater<Batch>> heap;
        for (int od : ods_) {
            const Curves& curves = profile(od, order);
            for (const Pair& pair : pairs(curves, od, order, deadline_)) {
                const Batch batch = best_batch(curves, pair, order);
                if (batch.units)
                    heap.push(batch);
            }
        }
        while (!heap.empty()) {
            check(deadline_);
            const Batch batch = heap.top();
            heap.pop();
            if (attempt(batch, order))
                return true;
            const Batch next = best_batch(profiles_.at(batch.pair.od).curves, batch.pair, order);
            if (next.units)
                heap.push(next);
        }
        result_.max_order_checked = order;
        return false;
    }

    std::string optimize()
    {
        int active_od = 0;
        while (true) {
            check(deadline_);
            if (max_updates_ >= 0 && static_cast<int>(result_.updates.size()) >= max_updates_)
                return "ITERATION_LIMIT_NOT_CONVERGED";
            if (!active_od)
                active_od = choose_od();
            if (active_od) {
                if (!improve_h1(active_od)) {
                    stalled_.insert(active_od);
                    active_od = 0;
                }
                continue;
            }
            if (!rescanned_) {
                for (int od : ods_)
                    profile(od, 1, true);
                stalled_.clear();
                rescanned_ = true;
                continue;
            }
            result_.max_order_checked = 1;
            bool escaped = false;
            for (int order = 2; order <= maximum_order; ++order) {
                if (escape(order)) {
                    escaped = true;
                    break;
                }
            }
            if (!escaped)
                return "H8_APPROXIMATE_PMC_SEARCH_EXHAUSTED";
        }
    }

    const Data& data_;
    const Deadline started_ = std::chrono::steady_clock::now();
    Loader loader_;
    Deadline deadline_;
    int max_updates_;
    SearchResult result_;
    Snapshot snapshot_;
    Profiles builder_;
    const AffectedODs* detector_ = nullptr;
    std::vector<int> ods_;
    std::map<int, CachedProfile> profiles_;
    std::set<int> dirty_, stalled_;
    std::map<PairKey, std::set<int>> excluded_;
    bool rescanned_ = false;
};

} // namespace

SearchResult search_sar(const Data& data, const Assignment& initial, Deadline deadline, int max_updates)
{
    return Search(data, initial, deadline, max_updates).run();
}

} // namespace metro
