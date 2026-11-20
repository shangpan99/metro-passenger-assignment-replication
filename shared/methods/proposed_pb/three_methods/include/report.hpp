#pragma once

#include "search.hpp"
#include <filesystem>
#include <optional>

namespace metro {
struct ConstantStepSummary {
    double best_objective = 0;
    int best_iteration = 0, iterations = 0, infeasible_steps = 0;
    int uphill_updates = 0, equal_cost_updates = 0, downhill_updates = 0;
    int moved_agents_total = 0, pmc_probe_count = 0, infeasible_unit_diagnostics = 0;
    std::optional<ConstantCycle> cycle;
    int max_iterations = -1;
};

struct MethodSummary {
    std::string instance, method, status, assignment_sha256;
    double initial = 0, final = 0, search_seconds = 0, wall_seconds = 0;
    int accepted = 0, max_order = 0;
    bool certified = false, parallel = false;
    std::optional<ConstantStepSummary> constant_step;
};

std::string json_string(const std::string&);
std::optional<MethodSummary> read_completion(const std::filesystem::path&);
void write_task(const std::filesystem::path&, const Data&, const Snapshot&, const Snapshot&,
                const SearchResult&, MethodSummary&, double time_limit, Deadline started,
                const Snapshot* best = nullptr, int constant_max_iterations = -1);
void write_batch(const std::filesystem::path&, const std::vector<MethodSummary>&);
}
