#include "assignment.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace metro {
namespace {

struct Passenger {
    AgentResult* result;
    const Path* path;
    const Demand* demand;
    bool details;
    std::size_t next = 0;
    int node = -1;
    int last_time = -1;
    bool queued = false;
};

using PassengerId = int;
using LinkCompletion = std::pair<PassengerId, int>;
using TrainArrival = std::pair<std::pair<int, int>, std::vector<PassengerId>>;

constexpr double infinity = std::numeric_limits<double>::infinity();

bool queue_link(int type) {
    return type == access_link || type == transfer_link;
}

void record_node(Passenger& passenger, int node, int time) {
    if (passenger.last_time > time) {
        throw std::logic_error("Nonmonotone passenger trajectory time");
    }
    if (passenger.last_time == time && passenger.node == node) {
        return;
    }
    if (passenger.details) {
        auto& result = *passenger.result;
        result.nodes.push_back(node);
        result.times.push_back(time);
    }
    passenger.node = node;
    passenger.last_time = time;
}

bool is_active(const Passenger& passenger) {
    const auto& result = *passenger.result;
    return !result.complete && result.failure_reason.empty();
}

void compute_metrics(AgentResult& result, const Demand& demand, const Data& data,
                     int last_time) {
    for (const auto& episode : result.episodes) {
        result.waiting += episode.board == -1 ? infinity : episode.board - episode.ready;
    }
    if (!result.complete) {
        result.arrival = result.travel = result.nonwaiting = infinity;
        result.early = result.late = result.schedule_cost = result.objective = infinity;
        return;
    }
    result.arrival = last_time;
    result.travel = result.arrival - result.agent.dep;
    result.nonwaiting = result.travel - result.waiting;
    result.early = std::max(demand.earliest_arr - result.arrival, 0.0);
    result.late = std::max(result.arrival - demand.latest_arr, 0.0);
    result.schedule_cost = data.early_weight * result.early + data.late_weight * result.late;
    result.objective = data.travel_weight * result.travel + result.schedule_cost;
}

}  // namespace

std::string QueueKey::label() const {
    return "N" + std::to_string(platform) + "_L" + std::to_string(line)
        + "_D" + std::to_string(direction);
}

Loader::Loader(const Data& data) : data_(data), horizon_(0) {
    int max_nonrun = 0;
    int last_train = 0;
    int last_departure = 0;
    for (const auto& [id, demand] : data.demands) {
        original_counts_.emplace(id, demand.count);
        last_departure = std::max(last_departure, demand.latest_dep);
    }
    for (const auto& [id, path] : data.paths) {
        int nonrun = 0;
        for (int link_id : path.links) {
            const auto& link = data.links.at(link_id);
            if (link.type != run_link) {
                nonrun += link.time;
            }
        }
        max_nonrun = std::max(max_nonrun, nonrun);
    }
    for (const auto& [id, train] : data.trains) {
        if (train.links.size() + 1 != train.times.size()
            || train.nodes.size() != train.times.size() || train.capacity < 0) {
            throw std::invalid_argument("Invalid train data for train " + std::to_string(id));
        }
        if (train.times.front() < 0 || !std::is_sorted(train.times.begin(), train.times.end())) {
            throw std::invalid_argument("Invalid timetable for train " + std::to_string(id));
        }
        last_train = std::max(last_train, train.times.back());
    }
    horizon_ = std::max(last_train + max_nonrun, last_departure) + 2;
    departures_.resize(horizon_ + 1);
    for (const auto& [id, train] : data.trains) {
        for (std::size_t segment = 0; segment < train.links.size(); ++segment) {
            departures_.at(train.times[segment]).emplace_back(id, static_cast<int>(segment));
            const auto& first = data.links.at(train.links[segment]);
            const QueueKey key{first.from, train.line, train.direction};
            std::vector<int> block;
            for (auto end = segment; end < train.links.size(); ++end) {
                block.push_back(train.links[end]);
                const auto [where, inserted] = queue_by_block_.emplace(block, key);
                if (!inserted && !(where->second == key)) {
                    throw std::invalid_argument("A physical run-link block maps to multiple queues");
                }
            }
        }
    }
}

Snapshot Loader::evaluate(const Assignment& assignment, bool details) const {
    return evaluate(assignment, original_counts_, details);
}

Snapshot Loader::evaluate(const Assignment& assignment,
                          const std::map<int, int>& expected_counts, bool details) const {
    if (expected_counts.size() != original_counts_.size()) {
        throw std::invalid_argument("Expected passenger counts must cover all ODs");
    }
    for (const auto& [od, original_count] : original_counts_) {
        if (expected_counts.at(od) < 0) {
            throw std::invalid_argument("Expected passenger counts must be nonnegative");
        }
    }
    Snapshot snapshot;
    std::map<int, Passenger> passengers;
    std::set<int> ranks;
    std::map<int, int> observed_counts;
    std::vector<std::vector<PassengerId>> activations(horizon_ + 1);
    std::vector<std::vector<LinkCompletion>> completions(horizon_ + 1);
    std::vector<std::vector<TrainArrival>> arrivals(horizon_ + 1);
    std::map<std::pair<int, int>, std::vector<PassengerId>> through_ready;
    std::map<QueueKey, std::vector<PassengerId>> queues;

    for (const auto& agent : assignment) {
        const auto& demand = data_.demands.at(agent.od);
        const auto& path = data_.paths.at(agent.path);
        const auto [where, inserted] = snapshot.agents.try_emplace(agent.id);
        if (!inserted) {
            throw std::invalid_argument("Duplicate agent ID " + std::to_string(agent.id));
        }
        auto& result = where->second;
        result.agent = agent;
        passengers.emplace(agent.id, Passenger{&result, &path, &demand, details});
        ++observed_counts[agent.od];
        if (agent.rank <= 0 || !ranks.insert(agent.rank).second
            || agent.dep < demand.earliest_dep || agent.dep > demand.latest_dep
            || std::find(demand.paths.begin(), demand.paths.end(), agent.path) == demand.paths.end()
            || path.from != demand.origin || path.to != demand.destination) {
            result.failure_reason = "invalid_assignment_choice_or_rank";
        } else {
            activations.at(agent.dep).push_back(agent.id);
        }
    }

    const auto advance_nonrun = [&](Passenger& passenger, int time, bool activation) {
        auto& result = *passenger.result;
        const auto& links = passenger.path->links;
        if (passenger.next == links.size()) {
            result.complete = passenger.node == passenger.demand->destination;
            if (!result.complete) {
                result.failure_reason = "link_sequence_exhausted_at_node_"
                    + std::to_string(passenger.node) + "_instead_of_"
                    + std::to_string(passenger.demand->destination);
            }
            return;
        }
        const auto& link = data_.links.at(links[passenger.next]);
        if (link.type == run_link) {
            result.failure_reason = activation ? "path_starts_with_run_link_without_access_queue"
                : "run_link_" + std::to_string(link.id) + "_without_queue_or_through_train";
            return;
        }
        if (passenger.node != link.from) {
            result.failure_reason = "nonrun_link_" + std::to_string(link.id) + "_starts_at_"
                + std::to_string(link.from) + "_but_agent_at_" + std::to_string(passenger.node);
            return;
        }
        ++passenger.next;
        if (details) {
            result.links.push_back(link.id);
        }
        const int completion_time = time + link.time;
        if (completion_time > horizon_) {
            result.failure_reason = "nonrun_link_" + std::to_string(link.id)
                + "_completion_beyond_horizon";
            return;
        }
        completions.at(completion_time).emplace_back(result.agent.id, link.id);
    };

    const auto complete_nonrun_at = [&](int time) {
        auto& pending = completions[time];
        while (!pending.empty()) {
            auto batch = std::move(pending);
            pending.clear();
            std::sort(batch.begin(), batch.end());
            for (auto [id, link_id] : batch) {
                auto& passenger = passengers.at(id);
                if (!is_active(passenger)) {
                    continue;
                }
                const auto& link = data_.links.at(link_id);
                record_node(passenger, link.to, time);
                if (!queue_link(link.type)) {
                    advance_nonrun(passenger, time, false);
                    continue;
                }
                auto& result = *passenger.result;
                const auto& links = passenger.path->links;
                if (passenger.next == links.size()) {
                    result.failure_reason = "queue_link_" + std::to_string(link_id)
                        + "_followed_by_no_run_link";
                    continue;
                }
                std::vector<int> block;
                for (auto i = passenger.next; i < links.size(); ++i) {
                    if (data_.links.at(links[i]).type != run_link) {
                        break;
                    }
                    block.push_back(links[i]);
                }
                if (block.empty()) {
                    result.failure_reason = "queue_link_" + std::to_string(link_id)
                        + "_not_followed_by_run_link";
                    continue;
                }
                const auto found = queue_by_block_.find(block);
                if (found == queue_by_block_.end() || found->second.platform != link.to) {
                    result.failure_reason = "queue_key_error:no_train_can_cover_run_block";
                    continue;
                }
                const auto& key = found->second;
                passenger.queued = true;
                result.episodes.push_back({key, link_id, link.type, std::move(block), time});
                auto& queue = queues[key];
                const auto fifo_key = [&](int queued_id) {
                    const auto& r = *passengers.at(queued_id).result;
                    return std::make_tuple(r.episodes.back().ready, r.agent.rank, r.agent.id);
                };
                const auto place = std::lower_bound(queue.begin(), queue.end(), id,
                    [&](int a, int b) { return fifo_key(a) < fifo_key(b); });
                queue.insert(place, id);
            }
        }
    };

    for (int time = 0; time <= horizon_; ++time) {
        auto& activating = activations[time];
        std::sort(activating.begin(), activating.end());
        for (int id : activating) {
            auto& passenger = passengers.at(id);
            record_node(passenger, passenger.demand->origin, time);
            advance_nonrun(passenger, time, true);
        }
        complete_nonrun_at(time);
        for (const auto& [next_event, ids] : arrivals[time]) {
            const auto [train_id, next_segment] = next_event;
            const auto& train = data_.trains.at(train_id);
            for (int id : ids) {
                auto& passenger = passengers.at(id);
                if (!is_active(passenger)) {
                    continue;
                }
                const auto& links = passenger.path->links;
                if (next_segment < static_cast<int>(train.links.size())
                    && passenger.next < links.size() && links[passenger.next] == train.links[next_segment]) {
                    through_ready[next_event].push_back(id);
                } else {
                    advance_nonrun(passenger, time, false);
                }
            }
        }
        complete_nonrun_at(time);
        for (const auto& event : departures_[time]) {
            const auto [train_id, segment] = event;
            const auto& train = data_.trains.at(train_id);
            const auto& link = data_.links.at(train.links[segment]);
            const QueueKey key{link.from, train.line, train.direction};
            const int arrival = train.times[segment + 1];
            Service service{};
            service.train = train_id;
            service.segment = segment;
            service.queue = key;
            service.dep = time;
            service.arr = arrival;
            service.link = link.id;
            service.capacity = train.capacity;
            if (auto through = through_ready.find(event); through != through_ready.end()) {
                service.through = std::move(through->second);
                through_ready.erase(through);
            }
            for (int id : service.through) {
                auto& passenger = passengers.at(id);
                ++passenger.next;
                if (details) {
                    passenger.result->links.push_back(link.id);
                }
            }
            service.residual_before = train.capacity - static_cast<int>(service.through.size());
            if (service.residual_before < 0) {
                throw std::logic_error("Through passengers exceed train capacity");
            }
            auto& queue = queues[key];
            if (details) {
                service.waiting = queue;
            }
            service.residual_after = service.residual_before;
            const auto served = [&](int id) {
                auto& passenger = passengers.at(id);
                auto& result = *passenger.result;
                auto& episode = result.episodes.back();
                const auto& block = episode.block;
                if (block.size() > train.links.size() - segment
                    || !std::equal(block.begin(), block.end(), train.links.begin() + segment)) {
                    return false;
                }
                if (details) {
                    service.eligible.push_back(id);
                }
                if (service.residual_after == 0) {
                    return false;
                }
                --service.residual_after;
                service.boarded.push_back(id);
                ++passenger.next;
                record_node(passenger, passenger.node, time);
                if (details) {
                    result.links.push_back(link.id);
                }
                episode.board = time;
                episode.train = train_id;
                episode.boarding_link = link.id;
                passenger.queued = false;
                return true;
            };
            auto retained = queue.begin();
            for (int id : queue) {
                if (!served(id)) {
                    *retained++ = id;
                }
            }
            queue.erase(retained, queue.end());

            service.onboard = service.through;
            service.onboard.insert(service.onboard.end(), service.boarded.begin(), service.boarded.end());
            for (int id : service.onboard) {
                record_node(passengers.at(id), link.to, arrival);
            }
            if (!service.onboard.empty()) {
                auto& events = arrivals.at(arrival);
                if (details) {
                    events.push_back({{train_id, segment + 1}, service.onboard});
                } else {
                    events.push_back({{train_id, segment + 1}, std::move(service.onboard)});
                }
            }
            if (details) {
                snapshot.services.push_back(std::move(service));
            }
        }
    }

    for (auto& [id, passenger] : passengers) {
        auto& result = *passenger.result;
        if (is_active(passenger)) {
            if (passenger.queued) {
                result.failure_reason = "no_usable_train_after_queue_"
                    + result.episodes.back().queue.label() + "_by_horizon";
            } else if (passenger.next < passenger.path->links.size()) {
                result.failure_reason = "unfinished_path_at_horizon";
            } else {
                result.failure_reason = "destination_not_reached";
            }
        }
        compute_metrics(result, *passenger.demand, data_, passenger.last_time);
        snapshot.feasible = snapshot.feasible && result.complete;
        snapshot.travel += result.travel;
        snapshot.waiting += result.waiting;
        snapshot.nonwaiting += result.nonwaiting;
        snapshot.early += result.early;
        snapshot.late += result.late;
        snapshot.schedule_cost += result.schedule_cost;
        snapshot.objective += result.objective;
    }
    for (const auto& [od, expected] : expected_counts) {
        snapshot.feasible = snapshot.feasible && observed_counts[od] == expected;
    }
    if (!snapshot.feasible) {
        snapshot.travel = snapshot.waiting = snapshot.nonwaiting = infinity;
        snapshot.early = snapshot.late = snapshot.schedule_cost = snapshot.objective = infinity;
    }
    if (!details) {
        snapshot.agents.clear();
    }
    return snapshot;
}

}  // namespace metro
