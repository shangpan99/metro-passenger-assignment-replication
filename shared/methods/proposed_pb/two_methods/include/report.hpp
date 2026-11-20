#pragma once

#include "search.hpp"
#include <filesystem>
#include <optional>

namespace metro {
struct MethodSummary {
    std::string instance, method, status, assignment_sha256;
    double initial = 0, final = 0, search_seconds = 0, wall_seconds = 0;
    int accepted = 0, max_order = 0;
    bool certified = false, parallel = false;
};

std::string json_string(const std::string&);
std::optional<MethodSummary> read_completion(const std::filesystem::path&);
void write_task(const std::filesystem::path&, const Data&, const Snapshot&, const Snapshot&,
                const SearchResult&, MethodSummary&, double time_limit, Deadline started);
void write_batch(const std::filesystem::path&, const std::vector<MethodSummary>&);
}
