#include "input.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace metro {
namespace {
namespace fs = std::filesystem;
using Row = std::map<std::string, std::string>;

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::vector<std::string> csv_fields(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                field += '"';
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (ch == ',' && !quoted) {
            fields.push_back(std::move(field));
            field.clear();
        } else {
            field += ch;
        }
    }
    if (quoted) throw std::runtime_error("Unclosed quote in CSV row");
    fields.push_back(std::move(field));
    return fields;
}

std::vector<Row> csv_rows(const fs::path& path) {
    auto content = read_file(path);
    if (content.compare(0, 3, "\xef\xbb\xbf") == 0) content.erase(0, 3);
    std::istringstream stream(content);
    std::string line;
    if (!std::getline(stream, line)) throw std::runtime_error("Empty CSV: " + path.string());
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto columns = csv_fields(line);
    if (std::set<std::string>(columns.begin(), columns.end()).size() != columns.size()) {
        throw std::runtime_error("Duplicate CSV headers: " + path.string());
    }
    std::vector<Row> result;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto fields = csv_fields(line);
        if (fields.size() != columns.size()) {
            throw std::runtime_error("Wrong CSV field count: " + path.string());
        }
        Row row;
        for (std::size_t i = 0; i < columns.size(); ++i) {
            row.emplace(columns[i], std::move(fields[i]));
        }
        result.push_back(std::move(row));
    }
    return result;
}

const std::string& field(const Row& row, const std::string& key) {
    const auto it = row.find(key);
    if (it == row.end()) throw std::runtime_error("Missing CSV column: " + key);
    return it->second;
}

double parse_number(const std::string& value) {
    std::size_t consumed = 0;
    const double parsed = std::stod(value, &consumed);
    if (!std::isfinite(parsed) ||
        value.find_first_not_of(" \t", consumed) != std::string::npos) {
        throw std::runtime_error("Invalid finite number: " + value);
    }
    return parsed;
}

int parse_integer(const std::string& value) {
    const double parsed = parse_number(value);
    const double nearest = std::round(parsed);
    if (std::abs(parsed - nearest) > 1e-9 || nearest < std::numeric_limits<int>::min() ||
        nearest > std::numeric_limits<int>::max()) {
        throw std::runtime_error("Invalid integer: " + value);
    }
    return static_cast<int>(nearest);
}

int integer(const Row& row, const std::string& key) {
    return parse_integer(field(row, key));
}

double number(const Row& row, const std::string& key) {
    return parse_number(field(row, key));
}

template <typename Parser>
auto parse_list(const std::string& value, Parser parse) {
    std::vector<decltype(parse(value))> result;
    std::istringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ';')) {
        if (item.find_first_not_of(" \t") != std::string::npos) result.push_back(parse(item));
    }
    return result;
}

std::vector<int> integers(const Row& row, const std::string& key) {
    return parse_list(field(row, key), parse_integer);
}

std::string metadata_string(const std::string& json, const std::string& key) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch value;
    if (!std::regex_search(json, value, pattern)) {
        throw std::runtime_error("Missing initialization metadata: " + key);
    }
    return value[1];
}

double metadata_number(const std::string& json, const std::string& key) {
    const std::regex pattern("\"" + key + "\"\\s*:\\s*([0-9.+eE-]+)");
    std::smatch value;
    if (!std::regex_search(json, value, pattern)) {
        throw std::runtime_error("Missing initialization metadata: " + key);
    }
    return parse_number(value[1]);
}

std::uint32_t rotate_right(std::uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32 - amount));
}

// SHA-256, FIPS 180-4. Used to verify the byte-for-byte saved initializations.
std::string sha256(const fs::path& path) {
    static constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    auto bytes = read_file(path);
    const std::uint64_t length_bits = static_cast<std::uint64_t>(bytes.size()) * 8;
    bytes.push_back(static_cast<char>(0x80));
    while (bytes.size() % 64 != 56) bytes.push_back('\0');
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<char>((length_bits >> shift) & 0xff));
    }

    std::array<std::uint32_t, 8> state{
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    for (std::size_t offset = 0; offset < bytes.size(); offset += 64) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            for (std::size_t byte = 0; byte < 4; ++byte) {
                words[i] = (words[i] << 8) |
                    static_cast<unsigned char>(bytes[offset + 4 * i + byte]);
            }
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const auto x = words[i - 15];
            const auto y = words[i - 2];
            const auto s0 = rotate_right(x, 7) ^ rotate_right(x, 18) ^ (x >> 3);
            const auto s1 = rotate_right(y, 17) ^ rotate_right(y, 19) ^ (y >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        auto [a, b, c, d, e, f, g, h] = state;
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto t1 = h + s1 + choose + constants[i] + words[i];
            const auto s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
    std::ostringstream hash;
    hash << std::hex << std::setfill('0');
    for (auto value : state) hash << std::setw(8) << value;
    return hash.str();
}

std::string check_hash(const fs::path& path, const std::string& metadata, const std::string& key) {
    const auto actual = sha256(path);
    if (actual != metadata_string(metadata, key)) {
        throw std::runtime_error("Inputs/initialization changed: " + path.string());
    }
    return actual;
}

void require(bool condition, const std::string& reason, int id) {
    if (!condition) throw std::runtime_error(reason + " (ID " + std::to_string(id) + ")");
}

int instance_passengers(const std::string& instance_id) {
    static const std::regex pattern("N([0-9]{4,})_R[0-9]{2}_W[1-5]");
    std::smatch match;
    if (!std::regex_match(instance_id, match, pattern)) {
        throw std::runtime_error("Unknown instance: " + instance_id);
    }
    const int passengers = parse_integer(match[1]);
    if (passengers <= 0) {
        throw std::runtime_error("Invalid passenger scale in instance: " + instance_id);
    }
    return passengers;
}

} // namespace

std::vector<std::string> list_cases(const fs::path& data_root) {
    std::vector<std::string> cases;
    std::set<std::string> seen;
    for (const auto& row : csv_rows(data_root / "INSTANCE_INDEX.csv")) {
        auto id = field(row, "instance_id");
        const int passengers = instance_passengers(id);
        if (row.count("passengers") && integer(row, "passengers") != passengers) {
            throw std::runtime_error("Passenger count differs from instance ID: " + id);
        }
        if (!seen.insert(id).second) throw std::runtime_error("Duplicate instance: " + id);
        cases.push_back(std::move(id));
    }
    return cases;
}

CaseInput read_case(const fs::path& data_root, const std::string& instance_id) {
    const int passengers = instance_passengers(instance_id);
    const auto inputs = data_root / "instances" / instance_id;
    const auto initial = data_root / "initializations" / instance_id;
    if (!fs::is_directory(inputs) || !fs::is_directory(initial)) {
        throw std::runtime_error("Unknown instance: " + instance_id);
    }

    CaseInput result;
    result.initialization_metadata = read_file(initial / "metadata.json");
    const auto& meta = result.initialization_metadata;
    if (metadata_string(meta, "instance_id") != instance_id ||
        metadata_number(meta, "passengers") != passengers ||
        metadata_string(meta, "initialization_code") != "SHORTEST_MIDDLE_EARLY") {
        throw std::runtime_error("Initialization metadata does not match instance: " + instance_id);
    }
    result.initial_objective = metadata_number(meta, "initial_objective");
    for (const auto* filename : {"input_node.csv", "input_link.csv", "input_path.csv",
                                 "input_demand.csv", "input_train_timetable.csv"}) {
        result.input_sha256.emplace(filename, check_hash(inputs / filename, meta, filename));
    }
    check_hash(inputs / "agent_demand.csv", meta, "agent_demand_sha256");
    check_hash(initial / "mip_start.json", meta, "mip_start_sha256");
    result.assignment_sha256 =
        check_hash(initial / "initial_agents.csv", meta, "initial_agents_sha256");

    std::set<int> nodes;
    for (const auto& row : csv_rows(inputs / "input_node.csv")) {
        const int id = integer(row, "node_id");
        require(nodes.insert(id).second, "Duplicate node", id);
    }

    auto& data = result.data;
    for (const auto& row : csv_rows(inputs / "input_link.csv")) {
        Link link{integer(row, "link_id"), integer(row, "from_node_id"),
                  integer(row, "to_node_id"), integer(row, "travel_time"),
                  integer(row, "capacity"), integer(row, "type")};
        require(nodes.count(link.from) && nodes.count(link.to), "Unknown link endpoint", link.id);
        require(link.time >= 0 && link.capacity >= 0, "Negative link time or capacity", link.id);
        require(data.links.emplace(link.id, link).second, "Duplicate link", link.id);
    }

    for (const auto& row : csv_rows(inputs / "input_path.csv")) {
        const auto* destination_column = row.count("to_node_id") ? "to_node_id" : "to_node_d";
        Path path{integer(row, "path_id"), integer(row, "from_node_id"),
                  integer(row, destination_column), integers(row, "node_list"),
                  integers(row, "link_list"), number(row, "free_travel_time")};
        require(path.nodes.size() == path.links.size() + 1 &&
                path.nodes.front() == path.from && path.nodes.back() == path.to,
                "Invalid path node sequence", path.id);
        double link_time = 0;
        for (std::size_t i = 0; i < path.links.size(); ++i) {
            const auto& link = data.links.at(path.links[i]);
            require(link.from == path.nodes[i] && link.to == path.nodes[i + 1],
                    "Path links and nodes disagree", path.id);
            link_time += link.time;
        }
        require(std::abs(link_time - path.free_time) <= 1e-6, "Invalid free-travel time", path.id);
        const auto id = path.id;
        require(data.paths.emplace(id, std::move(path)).second, "Duplicate path", id);
    }

    for (const auto& row : csv_rows(inputs / "input_demand.csv")) {
        Demand demand{integer(row, "od_id"), integer(row, "origion"), integer(row, "destination"),
                      integer(row, "demand"), integer(row, "departure_time_earliest"),
                      integer(row, "departure_time_latest"), integers(row, "path_id_list"),
                      number(row, "arrival_time_earliest"), number(row, "arrival_time_latest")};
        require(demand.count >= 0 && demand.latest_dep >= demand.earliest_dep &&
                !demand.paths.empty(), "Invalid OD demand or window", demand.id);
        std::sort(demand.paths.begin(), demand.paths.end());
        require(std::adjacent_find(demand.paths.begin(), demand.paths.end()) == demand.paths.end(),
                "Duplicate OD path", demand.id);
        for (const int pid : demand.paths) {
            const auto& path = data.paths.at(pid);
            require(path.from == demand.origin && path.to == demand.destination,
                    "OD and path endpoints disagree", demand.id);
        }
        const auto id = demand.id;
        require(data.demands.emplace(id, std::move(demand)).second, "Duplicate OD", id);
    }

    for (const auto& row : csv_rows(inputs / "input_train_timetable.csv")) {
        Train train{integer(row, "train_id"), integer(row, "line_id"),
                    integer(row, "direction_id"), integer(row, "capacity"),
                    integers(row, "node_sequence"), integers(row, "link_sequence"),
                    integers(row, "time_sequence")};
        require(train.direction != 0 && train.capacity >= 0, "Invalid train direction/capacity",
                train.id);
        require(train.nodes.size() == train.links.size() + 1 &&
                train.nodes.size() == train.times.size() &&
                std::is_sorted(train.times.begin(), train.times.end()),
                "Invalid train timetable sequence", train.id);
        require(train.nodes.front() == integer(row, "from_node_id") &&
                train.nodes.back() == integer(row, "to_node_id") &&
                train.times.front() == integer(row, "departure_time") &&
                train.times.back() == integer(row, "arrival_time"),
                "Train endpoints do not match its timetable sequence", train.id);
        for (std::size_t i = 0; i < train.links.size(); ++i) {
            const auto& link = data.links.at(train.links[i]);
            require(link.type == run_link && link.from == train.nodes[i] &&
                    link.to == train.nodes[i + 1], "Train links and nodes disagree", train.id);
        }
        const auto id = train.id;
        require(data.trains.emplace(id, std::move(train)).second, "Duplicate train", id);
    }

    std::set<int> agent_ids, ranks;
    std::map<int, int> od_counts;
    for (const auto& row : csv_rows(initial / "initial_agents.csv")) {
        const int id = integer(row, "agent_id");
        const int rank = row.count("fixed_rank") ? integer(row, "fixed_rank") : id;
        Agent agent{id, rank, integer(row, "od_id"), integer(row, "path_id"),
                    integer(row, "departure_time")};
        require(agent.id > 0 && agent_ids.insert(agent.id).second, "Invalid or duplicate agent",
                agent.id);
        require(agent.rank > 0 && ranks.insert(agent.rank).second, "Invalid or duplicate rank",
                agent.id);
        require(integer(row, "volume") == 1, "Expected one passenger per agent", agent.id);
        const auto& od = data.demands.at(agent.od);
        require(std::binary_search(od.paths.begin(), od.paths.end(), agent.path) &&
                od.earliest_dep <= agent.dep && agent.dep <= od.latest_dep,
                "Illegal initial alternative", agent.id);
        const auto& path = data.paths.at(agent.path);
        require(integer(row, "from_node_id") == od.origin &&
                integer(row, "to_node_id") == od.destination &&
                integers(row, "link_list") == path.links,
                "Agent path or endpoints disagree", agent.id);
        ++od_counts[agent.od];
        result.agents.push_back(agent);
    }
    for (const auto& [id, demand] : data.demands) {
        require(od_counts[id] == demand.count, "Passenger count differs from OD demand", id);
    }
    if (result.agents.size() != static_cast<std::size_t>(passengers)) {
        throw std::runtime_error("Initial assignment count differs from instance: " + instance_id);
    }
    std::sort(result.agents.begin(), result.agents.end(),
              [](const Agent& left, const Agent& right) { return left.id < right.id; });
    return result;
}


std::map<int, std::map<int, std::set<int>>> realized_od_node_trains(
    const Data& data, std::chrono::steady_clock::time_point deadline) {
    struct Ride {
        int train, departure, arrival;
    };
    struct Block {
        std::vector<int> links;
        std::size_t next;
    };
    std::map<std::vector<int>, std::vector<Ride>> ride_cache;
    std::map<int, std::map<std::size_t, Block>> path_blocks;
    const auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };
    for (const auto& [id, path] : data.paths) {
        for (std::size_t pos = 0; pos < path.links.size();) {
            if (expired()) return {};
            if (data.links.at(path.links[pos]).type != run_link) {
                ++pos;
                continue;
            }
            auto end = pos + 1;
            while (end < path.links.size() && data.links.at(path.links[end]).type == run_link) {
                ++end;
            }
            const auto block_begin = path.links.begin() + pos;
            const auto block_end = path.links.begin() + end;
            std::vector<int> block(block_begin, block_end);
            auto [cache, inserted] = ride_cache.try_emplace(block);
            if (inserted) {
                for (const auto& [train_id, train] : data.trains) {
                    if (expired()) return {};
                    for (std::size_t start = 0; start + block.size() <= train.links.size(); ++start) {
                        if (std::equal(block.begin(), block.end(), train.links.begin() + start)) {
                            cache->second.push_back(
                                {train_id, train.times[start], train.times[start + block.size()]});
                        }
                    }
                }
                if (cache->second.empty()) {
                    throw std::runtime_error("No train covers run-link block on path " +
                                             std::to_string(id));
                }
            }
            path_blocks[id].emplace(pos, Block{std::move(block), end});
            pos = end;
        }
    }

    std::map<int, std::map<int, std::set<int>>> result;
    for (const auto& [od, demand] : data.demands) {
        for (int pid : demand.paths) {
            const auto& path = data.paths.at(pid);
            std::map<std::pair<std::size_t, int>, bool> feasible_suffix;
            // Any trains reachable by a later departure are also reachable by an
            // earlier departure with additional waiting. Collect train
            // resources only when a complete suffix exists.
            const auto visit = [&](const auto& self, std::size_t pos, int ready) -> bool {
                if (expired()) return false;
                if (pos == path.links.size()) return true;
                const auto key = std::make_pair(pos, ready);
                const auto found = feasible_suffix.find(key);
                if (found != feasible_suffix.end()) return found->second;
                const auto& link = data.links.at(path.links[pos]);
                bool feasible = false;
                if (link.type != run_link) {
                    feasible = self(self, pos + 1, ready + link.time);
                } else {
                    const auto& block = path_blocks.at(pid).at(pos);
                    for (const auto& ride : ride_cache.at(block.links)) {
                        if (expired()) return false;
                        if (ride.departure < ready || !self(self, block.next, ride.arrival)) {
                            continue;
                        }
                        feasible = true;
                        for (const int lid : block.links) {
                            const auto& leg = data.links.at(lid);
                            result[od][leg.from].insert(ride.train);
                            result[od][leg.to].insert(ride.train);
                        }
                    }
                }
                feasible_suffix.emplace(key, feasible);
                return feasible;
            };
            visit(visit, 0, demand.earliest_dep);
            if (expired()) return {};
        }
    }
    return result;
}

}  // namespace metro
