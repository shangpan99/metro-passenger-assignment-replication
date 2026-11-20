#pragma once

#include "assignment.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace metro {

struct CaseInput {
    Data data;
    Assignment agents;
    double initial_objective = 0;
    std::string assignment_sha256;
    std::map<std::string, std::string> input_sha256;
    std::string initialization_metadata;
};

std::vector<std::string> list_cases(const std::filesystem::path& data_root);
CaseInput read_case(const std::filesystem::path& data_root, const std::string& instance_id);

std::map<int, std::map<int, std::set<int>>> realized_od_node_trains(
    const Data& data, std::chrono::steady_clock::time_point deadline);

}  // namespace metro
