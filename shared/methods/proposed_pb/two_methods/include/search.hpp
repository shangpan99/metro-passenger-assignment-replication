#pragma once

#include "assignment.hpp"
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace metro {
using Deadline = std::chrono::steady_clock::time_point;

struct Update {
    int od = 0, source_path = 0, source_dep = 0, target_path = 0, target_dep = 0;
    int units = 0, depth = 0;
    double objective_before = 0, objective_after = 0;
    bool two_step = false;
    int distinct_changed = 0;
    int second_od = 0, second_source_path = 0, second_source_dep = 0;
    int second_target_path = 0, second_target_dep = 0, second_units = 0, second_depth = 0;
    std::vector<int> moved_ids, second_moved_ids;
    double total_travel_time = 0, total_waiting_time = 0;
    double elapsed_seconds = 0;
    std::string accepted_by;
    std::optional<double> surrogate_objective_delta;
};

struct SearchResult {
    Assignment agents;
    double initial_objective = 0, final_objective = 0;
    std::string status;
    int max_order_checked = 0;
    std::vector<Update> updates;
};

SearchResult search_proposed(const Data&, const Assignment&, Deadline, int max_updates = -1);
SearchResult search_sar(const Data&, const Assignment&, Deadline, int max_updates = -1);
}
