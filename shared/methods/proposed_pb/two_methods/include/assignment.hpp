#pragma once

#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace metro {

constexpr int access_link = 1;
constexpr int egress_link = 2;
constexpr int run_link = 3;
constexpr int transfer_link = 5;

struct Link {
    int id, from, to, time, capacity, type;
};

struct Path {
    int id, from, to;
    std::vector<int> nodes, links;
    double free_time;
};

struct Demand {
    int id, origin, destination, count, earliest_dep, latest_dep;
    std::vector<int> paths;
    double earliest_arr, latest_arr;
};

struct Train {
    int id, line, direction, capacity;
    std::vector<int> nodes, links, times;
};

struct Data {
    std::map<int, Link> links;
    std::map<int, Path> paths;
    std::map<int, Demand> demands;
    std::map<int, Train> trains;
    double travel_weight = 1.0;
    double early_weight = 0.0;
    double late_weight = 0.0;
};

struct Agent {
    int id, rank, od, path, dep;
};
using Assignment = std::vector<Agent>;

struct QueueKey {
    int platform, line, direction;
    auto tie() const { return std::tie(platform, line, direction); }
    bool operator<(const QueueKey& other) const { return tie() < other.tie(); }
    bool operator==(const QueueKey& other) const { return tie() == other.tie(); }
    std::string label() const;
};

struct Episode {
    QueueKey queue;
    int source_link, source_type;
    std::vector<int> block;
    int ready;
    int board = -1;
    int train = -1;
    int boarding_link = -1;
};

struct AgentResult {
    Agent agent;
    bool complete = false;
    std::string failure_reason;
    double arrival = 0, travel = 0, waiting = 0, nonwaiting = 0;
    double early = 0, late = 0, schedule_cost = 0, objective = 0;
    std::vector<int> nodes, links, times;
    std::vector<Episode> episodes;
};

struct Service {
    int train, segment;
    QueueKey queue;
    int dep, arr, link, capacity;
    int residual_before, residual_after;
    std::vector<int> waiting, eligible, boarded, through, onboard;
};

struct Snapshot {
    bool feasible = true;
    std::map<int, AgentResult> agents;
    std::vector<Service> services;
    double travel = 0, waiting = 0, nonwaiting = 0, early = 0, late = 0;
    double schedule_cost = 0, objective = 0;
};

class Loader {
public:
    explicit Loader(const Data& data);
    // Compact scoring mode keeps aggregate metrics and skips trace export.
    Snapshot evaluate(const Assignment& assignment, bool details = true) const;
    Snapshot evaluate(const Assignment& assignment,
                      const std::map<int, int>& expected_counts, bool details = true) const;

private:
    const Data& data_;
    int horizon_;
    std::map<int, int> original_counts_;
    std::map<std::vector<int>, QueueKey> queue_by_block_;
    std::vector<std::vector<std::pair<int, int>>> departures_;
};

}  // namespace metro
