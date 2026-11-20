#include "report.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace metro {
namespace fs = std::filesystem;
namespace {
constexpr auto constant_method = "Constant_PMC_DiminishingStep";
constexpr auto constant_algorithm = "CONSTANT_FIRST_UNIT_PMC_GLOBAL_PAIR_DIMINISHING_DONOR_FLOW";

std::ofstream output(const fs::path& file) {
    std::ofstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.open(file, std::ios::binary);
    out.imbue(std::locale::classic());
    out << std::setprecision(15);
    return out;
}

template<class Range>
std::string join(const Range& values) {
    std::ostringstream out;
    const char* sep = "";
    for (const auto& value : values) {
        out << sep << value;
        sep = ";";
    }
    return out.str();
}

const char* boolean(bool value) { return value ? "true" : "false"; }

void cycle_json(std::ostream& out, const std::optional<ConstantCycle>& cycle) {
    out << ",\n  \"cycle_detected\": " << boolean(cycle.has_value());
    if (!cycle) return;
    out << ",\n  \"cycle_start_iteration\": " << cycle->start_iteration
        << ",\n  \"cycle_detection_iteration\": " << cycle->detection_iteration
        << ",\n  \"cycle_length\": " << cycle->detection_iteration - cycle->start_iteration
        << ",\n  \"cycle_state_hash\": " << json_string(cycle->state_hash)
        << ",\n  \"cycle_step_condition\": \"ALL_FUTURE_STEPS_ARE_ONE\""
        << ",\n  \"cycle_objective_min\": " << cycle->objective_min
        << ",\n  \"cycle_objective_max\": " << cycle->objective_max;
}

void constant_summary_json(std::ostream& out, const ConstantStepSummary& s) {
    out << ",\n  \"algorithm\": " << json_string(constant_algorithm)
        << ",\n  \"best_objective\": " << s.best_objective
        << ",\n  \"best_iteration\": " << s.best_iteration
        << ",\n  \"iterations\": " << s.iterations
        << ",\n  \"infeasible_steps\": " << s.infeasible_steps
        << ",\n  \"uphill_updates\": " << s.uphill_updates
        << ",\n  \"equal_cost_updates\": " << s.equal_cost_updates
        << ",\n  \"downhill_updates\": " << s.downhill_updates
        << ",\n  \"moved_agents_total\": " << s.moved_agents_total
        << ",\n  \"pmc_probe_count\": " << s.pmc_probe_count
        << ",\n  \"infeasible_unit_diagnostics\": " << s.infeasible_unit_diagnostics
        << ",\n  \"solver_calls\": 0"
        << ",\n  \"diagnostic_backend\": \"native_exact_fast_or_full_fifo\""
        << ",\n  \"accepts_nonimproving_feasible_moves\": true"
        << ",\n  \"step_rule\": \"min(n,max(1,n//(k+1)))\""
        << ",\n  \"k_start\": 0"
        << ",\n  \"k_counts\": \"COMPLETED_ATTEMPTED_STEPS_INCLUDING_INFEASIBLE\"";
    if (s.max_iterations >= 0) out << ",\n  \"max_iterations\": " << s.max_iterations;
    cycle_json(out, s.cycle);
}

void summary_json(std::ostream& out, const MethodSummary& s) {
    out << "{\n"
        << "  \"instance_id\": " << json_string(s.instance) << ",\n"
        << "  \"method\": " << json_string(s.method) << ",\n"
        << "  \"status\": " << json_string(s.status) << ",\n"
        << "  \"initial_assignment_sha256\": " << json_string(s.assignment_sha256) << ",\n"
        << "  \"initial_objective\": " << s.initial << ",\n"
        << "  \"final_objective\": " << s.final << ",\n"
        << "  \"accepted_updates\": " << s.accepted << ",\n"
        << "  \"max_order_checked\": " << s.max_order << ",\n"
        << "  \"local_optimum_certified\": " << boolean(s.certified) << ",\n"
        << "  \"global_optimum_proven\": false,\n"
        << "  \"computation_seconds\": " << s.search_seconds << ",\n"
        << "  \"complete_wall_time_seconds\": " << s.wall_seconds << ",\n"
        << "  \"parallel_methods\": " << boolean(s.parallel);
    if (s.method == "Proposed") out << ",\n  \"J\": 1,\n  \"Bmax\": 11";
    else if (s.method == "Approximate_PMC") out << ",\n  \"ell_max\": 8";
    if (s.constant_step) constant_summary_json(out, *s.constant_step);
    out << ",\n  \"initialization_code\": \"SHORTEST_MIDDLE_EARLY\"";
}

void write_agents(const fs::path& path, const Snapshot& state) {
    auto out = output(path);
    out << "agent_id,fixed_rank,od_id,path_id,departure_time,arrival_time,complete,failure_reason,"
           "travel_time,waiting_time,nonwaiting_time,early_time,late_time,schedule_cost,objective_cost,"
           "node_sequence,link_sequence,time_sequence\n";
    for (const auto& [id, row] : state.agents) {
        out << id << ',' << row.agent.rank << ',' << row.agent.od << ',' << row.agent.path << ','
            << row.agent.dep << ',' << row.arrival << ',' << (row.complete ? "True" : "False") << ','
            << row.failure_reason << ',' << row.travel << ',' << row.waiting << ',' << row.nonwaiting << ','
            << row.early << ',' << row.late << ',' << row.schedule_cost << ',' << row.objective << ','
            << join(row.nodes) << ',' << join(row.links) << ',' << join(row.times) << '\n';
    }
    out.close();
}

void write_flow(const fs::path& path, const Snapshot& state) {
    auto out = output(path);
    out << "od_id,path_id,departure_time,flow\n";
    std::map<std::tuple<int, int, int>, int> flows;
    for (const auto& [id, row] : state.agents)
        ++flows[{row.agent.od, row.agent.path, row.agent.dep}];
    for (const auto& [key, count] : flows) {
        const auto& [od, route, dep] = key;
        out << od << ',' << route << ',' << dep << ',' << count << '\n';
    }
    out.close();
}

void write_assignment(const fs::path& path, const Data& data, const Snapshot& state) {
    auto out = output(path);
    out << "agent_id,fixed_rank,od_id,path_id,from_node_id,to_node_id,departure_time,volume,link_list\n";
    for (const auto& [id, row] : state.agents) {
        const auto& agent = row.agent;
        const auto& path_data = data.paths.at(agent.path);
        out << id << ',' << agent.rank << ',' << agent.od << ',' << agent.path << ','
            << path_data.from << ',' << path_data.to << ',' << agent.dep << ",1,"
            << join(path_data.links) << '\n';
    }
    out.close();
}

void write_episodes(const fs::path& path, const Data& data, const Snapshot& state) {
    auto out = output(path);
    out << "agent_id,od_id,path_id,episode_index,platform_node,queue_entry_link_id,ready_time,"
           "selected_train_id,boarding_time,train_arrival_at_end_of_block,block_links\n";
    for (const auto& [id, row] : state.agents) {
        for (std::size_t i = 0; i < row.episodes.size(); ++i) {
            const auto& e = row.episodes[i];
            out << id << ',' << row.agent.od << ',' << row.agent.path << ',' << i << ','
                << e.queue.platform << ',' << e.source_link << ',' << e.ready << ',' << e.train << ','
                << e.board << ',';
            const auto train = data.trains.find(e.train);
            if (train != data.trains.end()) {
                const auto& run = train->second;
                for (std::size_t j = 0; j + e.block.size() <= run.links.size(); ++j) {
                    if (run.nodes.at(j) == e.queue.platform && run.times.at(j) == e.board &&
                        std::equal(e.block.begin(), e.block.end(), run.links.begin() + j)) {
                        out << run.times.at(j + e.block.size());
                        break;
                    }
                }
            }
            out << ',' << join(e.block) << '\n';
        }
    }
    out.close();
}

void write_legacy_trajectory(const fs::path& path, const Snapshot& state) {
    auto out = output(path);
    out << "agent_id,node_sequence,link_sequence,time_sequence,early_time,delay_time,time_cost,volume,total_travel_time\n";
    for (const auto& [id, row] : state.agents) {
        std::vector<int> nodes{row.nodes.front()}, links, times{row.times.front()};
        std::size_t physical = 0;
        for (std::size_t i = 1; i < row.nodes.size(); ++i) {
            if (row.nodes[i] == row.nodes[i - 1]) {
                for (int time = row.times[i - 1] + 1; time <= row.times[i]; ++time) {
                    nodes.push_back(row.nodes[i]);
                    links.push_back(row.links.at(physical - 1));
                    times.push_back(time);
                }
            } else {
                nodes.push_back(row.nodes[i]);
                links.push_back(row.links.at(physical++));
                times.push_back(row.times[i]);
            }
        }
        out << id << ',' << join(nodes) << ',' << join(links) << ',' << join(times) << ','
            << row.early << ',' << row.late << ',' << row.schedule_cost << ",1," << row.travel << '\n';
    }
    out.close();
}

void write_services(const fs::path& raw, const Snapshot& state) {
    auto out = output(raw / "train_occupancy_residual_capacity.csv");
    auto audit = output(raw / "capacity_audit.csv");
    out << "event_id,train_id,stop_index,platform_node,departure_time,outgoing_link_id,alight,"
           "occ_minus_after_alighting,residual_boarding_capacity,new_board,occ_plus_after_boarding,train_capacity\n";
    audit << "train_id,link_id,load,capacity,remaining_capacity,violation\n";
    std::map<std::pair<int, int>, const Service*> ordered;
    for (const auto& s : state.services)
        ordered.emplace(std::make_pair(s.train, s.segment), &s);
    std::map<int, int> previous;
    std::size_t i = 0;
    for (const auto& [key, service] : ordered) {
        const auto& s = *service;
        const int through = static_cast<int>(s.through.size());
        const int load = static_cast<int>(s.onboard.size());
        out << i++ << ',' << s.train << ',' << s.segment << ',' << s.queue.platform << ',' << s.dep
            << ',' << s.link << ',' << previous[s.train] - through << ',' << through
            << ',' << s.residual_before << ',' << s.boarded.size() << ',' << load << ',' << s.capacity << '\n';
        audit << s.train << ',' << s.link << ',' << load << ',' << s.capacity << ','
              << s.capacity - load << ',' << std::max(0, load - s.capacity) << '\n';
        previous[s.train] = load;
    }
    out.close();
    audit.close();
}

void write_history(const fs::path& path, const Snapshot& initial, const SearchResult& result) {
    auto out = output(path);
    out << "iteration,objective,total_travel_time,total_waiting_time,state_version,active_od,"
           "neighborhood_order,moved_agent_count,elapsed_seconds,record_type,accepted_by,h_order,q,"
           "donor_path,donor_departure,receiver_path,receiver_departure,moved_agent_ids,actual_objective_delta,"
           "objective_before,second_od,second_donor_path,second_donor_departure,second_receiver_path,"
           "second_receiver_departure,second_q,second_depth,second_moved_agent_ids,move_type,surrogate_objective_delta\n";
    out << "0," << initial.objective << ',' << initial.travel << ',' << initial.waiting
        << ",0,,0,0,0,INITIAL,INITIAL,0,0" << std::string(17, ',') << '\n';
    std::size_t iteration = 0;
    for (const auto& u : result.updates) {
        ++iteration;
        out << iteration << ',' << u.objective_after << ',' << u.total_travel_time << ','
            << u.total_waiting_time << ',' << iteration << ',' << u.od;
        if (u.two_step) out << ';' << u.second_od;
        const int moved = u.two_step ? u.distinct_changed : u.units;
        out << ',' << u.depth << ',' << moved << ',' << u.elapsed_seconds << ",ACCEPTED,"
            << (u.accepted_by.empty() ? (u.two_step ? "TWO_STEP" : "SINGLE") : u.accepted_by) << ',' << u.depth << ',' << u.units << ','
            << u.source_path << ',' << u.source_dep << ',' << u.target_path << ',' << u.target_dep << ','
            << join(u.moved_ids) << ',' << u.objective_after - u.objective_before << ','
            << u.objective_before << ',';
        if (u.two_step) {
            out << u.second_od << ',' << u.second_source_path << ',' << u.second_source_dep << ','
                << u.second_target_path << ',' << u.second_target_dep << ',' << u.second_units << ','
                << u.second_depth << ',' << join(u.second_moved_ids);
        } else {
            out << ",,,,,,,";
        }
        out << ',' << (u.two_step ? "TWO_STEP" : "SINGLE") << ',';
        if (u.surrogate_objective_delta) out << *u.surrogate_objective_delta;
        out << '\n';
    }
    out.close();
}

void write_constant_attempts(const fs::path& path, const ConstantStepDetails& details, double initial_objective) {
    auto out = output(path);
    out << "record_type,iteration,step_k,step_fraction,od_id,donor_path,donor_departure,receiver_path,"
           "receiver_departure,donor_count,q,moved_agent_ids,unit_minus_benefit,unit_plus_cost,unit_net_gain,"
           "predicted_objective_delta,actual_objective_delta,objective_before,accepted,objective,best_objective,"
           "best_iteration,elapsed_seconds,candidate_objective\n";
    int best_iteration = 0;
    double best = initial_objective;
    for (const auto& a : details.attempts) {
        if (a.best_objective < best) {
            best = a.best_objective;
            best_iteration = a.iteration;
        }
        out << (a.accepted ? "ACCEPTED" : "INFEASIBLE_STEP") << ',' << a.iteration << ','
            << a.iteration - 1 << ',' << 1.0 / a.iteration << ',' << a.od << ','
            << a.source_path << ',' << a.source_dep << ',' << a.target_path << ',' << a.target_dep << ','
            << a.donor_count << ',' << a.units << ',' << join(a.moved_ids) << ',' << a.unit_minus_benefit
            << ',' << a.unit_plus_cost << ',' << a.unit_gain << ',' << -a.units * a.unit_gain << ',';
        if (a.candidate_objective) out << *a.candidate_objective - a.objective_before;
        out << ',' << a.objective_before << ',' << (a.accepted ? "True" : "False") << ','
            << a.objective_after << ',' << a.best_objective << ',' << best_iteration << ','
            << a.elapsed_seconds << ',';
        if (a.candidate_objective) out << *a.candidate_objective;
        out << '\n';
    }
    out.close();
}

void write_constant_details(const fs::path& raw, const Data& data, const Snapshot& final,
                            const Snapshot& best, const SearchResult& result) {
    const auto& details = *result.constant_step;
    write_constant_attempts(raw / "constant_step_attempts.csv", details, result.initial_objective);
    write_assignment(raw / "final_agents.csv", data, final);
    const auto best_dir = raw / "best_observed";
    fs::create_directory(best_dir);
    write_assignment(best_dir / "best_agents.csv", data, best);
    write_agents(best_dir / "optimized_agent_assignment_detailed.csv", best);
    write_legacy_trajectory(best_dir / "output_agent_reservation_timetable_optimized.csv", best);
    write_flow(best_dir / "optimized_path_departure_flow.csv", best);
    write_episodes(best_dir / "optimized_boarding_episodes.csv", data, best);
    write_services(best_dir, best);
    auto out = output(raw / "cycle_detection.json");
    out << "{\n  \"status\": " << json_string(result.status)
        << ",\n  \"local_optimum_certified\": false";
    cycle_json(out, details.cycle);
    out << "\n}\n";
    out.close();
}
}

std::string json_string(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 32) out << "\\u" << std::setfill('0') << std::setw(4) << std::hex << static_cast<int>(c);
            else out << c;
        }
    }
    out << '"';
    return out.str();
}

std::optional<MethodSummary> read_completion(const fs::path& dir) {
    const auto file = dir / "completed.txt";
    if (!fs::exists(file)) return std::nullopt;
    std::ifstream in(file);
    in.imbue(std::locale::classic());
    MethodSummary s;
    if (!(in >> s.instance >> s.method >> s.status >> s.assignment_sha256 >> s.initial >> s.final
          >> s.search_seconds >> s.wall_seconds >> s.accepted >> s.max_order >> s.certified >> s.parallel))
        throw std::runtime_error("Invalid completion record: " + file.string());
    if (s.method == constant_method) {
        const auto raw = dir / "raw";
        if (!fs::is_regular_file(raw / "constant_step_attempts.csv") ||
            !fs::is_regular_file(raw / "final_agents.csv") ||
            !fs::is_regular_file(raw / "best_observed/best_agents.csv") ||
            !fs::is_regular_file(raw / "cycle_detection.json"))
            throw std::runtime_error("Missing Constant PMC reports in saved task: " + dir.u8string());
        ConstantStepSummary details;
        bool has_cycle = false;
        if (!(in >> details.best_objective >> details.best_iteration >> details.iterations
              >> details.infeasible_steps >> details.uphill_updates >> details.equal_cost_updates
              >> details.downhill_updates >> details.moved_agents_total >> details.pmc_probe_count
              >> details.infeasible_unit_diagnostics >> details.max_iterations >> has_cycle))
            throw std::runtime_error("Invalid Constant PMC completion record: " + file.u8string());
        if (has_cycle) {
            ConstantCycle cycle;
            if (!(in >> cycle.start_iteration >> cycle.detection_iteration >> cycle.state_hash
                  >> cycle.objective_min >> cycle.objective_max))
                throw std::runtime_error("Invalid Constant PMC cycle record: " + file.u8string());
            details.cycle = std::move(cycle);
        }
        s.constant_step = std::move(details);
    }
    return s;
}

void write_task(const fs::path& dir, const Data& data, const Snapshot& initial, const Snapshot& final,
                const SearchResult& result, MethodSummary& s, double time_limit, Deadline started,
                const Snapshot* best, int constant_max_iterations) {
    if (result.constant_step.has_value() != (s.method == constant_method))
        throw std::logic_error("Constant PMC result and method disagree");
    if (result.constant_step) {
        const auto& c = *result.constant_step;
        if (!best || !best->feasible || best->objective != c.best_objective)
            throw std::logic_error("Missing or inconsistent best-observed replay");
        s.constant_step = ConstantStepSummary{c.best_objective, c.best_iteration, c.iterations,
            c.infeasible_steps, c.uphill_updates, c.equal_cost_updates, c.downhill_updates,
            c.moved_agents_total, c.pmc_probe_count, c.infeasible_unit_diagnostics, c.cycle,
            constant_max_iterations};
    }
    const auto raw = dir / "raw";
    fs::create_directories(raw / "initial_state");
    fs::create_directories(raw / "final_state");
    write_agents(raw / "initial_state/output_agent_reservation_timetable.csv", initial);
    write_agents(raw / "final_state/output_agent_reservation_timetable.csv", final);
    write_legacy_trajectory(raw / "output_agent_reservation_timetable_optimized.csv", final);
    write_flow(raw / "initial_state/path_departure_flow.csv", initial);
    write_flow(raw / "optimized_path_departure_flow.csv", final);
    write_episodes(raw / "optimized_boarding_episodes.csv", data, final);
    write_services(raw, final);
    write_history(raw / "iteration_summary.csv", initial, result);
    if (s.constant_step) write_constant_details(raw, data, final, *best, result);
    s.wall_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    {
        auto out = output(dir / "run_summary.json");
        summary_json(out, s);
        out << ",\n  \"computation_time_limit_seconds\": " << time_limit
            << ",\n  \"total_travel_time\": " << final.travel
            << ",\n  \"total_waiting_time\": " << final.waiting
            << ",\n  \"total_nonwaiting_time\": " << final.nonwaiting
            << ",\n  \"total_passengers\": " << final.agents.size()
            << ",\n  \"final_full_loading_verified\": true";
        if (s.constant_step)
            out << ",\n  \"best_full_loading_verified\": true"
                << ",\n  \"best_total_travel_time\": " << best->travel
                << ",\n  \"best_total_waiting_time\": " << best->waiting
                << ",\n  \"best_total_nonwaiting_time\": " << best->nonwaiting;
        out << "\n}\n";
        out.close();
    }
    auto out = output(dir / "completed.txt");
    out << s.instance << ' ' << s.method << ' ' << s.status << ' ' << s.assignment_sha256 << ' '
        << s.initial << ' ' << s.final << ' ' << s.search_seconds << ' ' << s.wall_seconds << ' '
        << s.accepted << ' ' << s.max_order << ' ' << s.certified << ' ' << s.parallel;
    if (s.constant_step) {
        const auto& c = *s.constant_step;
        out << ' ' << c.best_objective << ' ' << c.best_iteration << ' ' << c.iterations << ' '
            << c.infeasible_steps << ' ' << c.uphill_updates << ' ' << c.equal_cost_updates << ' '
            << c.downhill_updates << ' ' << c.moved_agents_total << ' ' << c.pmc_probe_count << ' '
            << c.infeasible_unit_diagnostics << ' ' << c.max_iterations << ' ' << c.cycle.has_value();
        if (c.cycle)
            out << ' ' << c.cycle->start_iteration << ' ' << c.cycle->detection_iteration << ' '
                << c.cycle->state_hash << ' ' << c.cycle->objective_min << ' ' << c.cycle->objective_max;
    }
    out << '\n';
    out.close();
}

void write_batch(const fs::path& dir, const std::vector<MethodSummary>& rows) {
    auto csv = output(dir / "experiment_summary.csv");
    auto json = output(dir / "all_run_summaries.json");
    auto compare = output(dir / "comparison_instances.csv");
    csv << "instance_id,method,status,initial_objective,final_objective,accepted_updates,local_optimum_certified,"
           "computation_seconds,complete_wall_time_seconds,initial_assignment_sha256,result_directory,parallel_methods\n";
    compare << "instance_id,Proposed_final_objective,Proposed_complete_wall_time_seconds,Proposed_status,"
               "Approximate_PMC_final_objective,Approximate_PMC_complete_wall_time_seconds,Approximate_PMC_status,"
               "Constant_PMC_DiminishingStep_final_objective,Constant_PMC_DiminishingStep_best_objective,"
               "Constant_PMC_DiminishingStep_best_iteration,Constant_PMC_DiminishingStep_complete_wall_time_seconds,"
               "Constant_PMC_DiminishingStep_status,Constant_PMC_DiminishingStep_cycle_detected\n";
    std::map<std::string, std::map<std::string, MethodSummary>> by_case;
    json << "[\n";
    bool first = true;
    for (const auto& s : rows) {
        csv << s.instance << ',' << s.method << ',' << s.status << ',' << s.initial << ',' << s.final << ','
            << s.accepted << ',' << boolean(s.certified) << ',' << s.search_seconds << ',' << s.wall_seconds
            << ',' << s.assignment_sha256 << ',' << s.instance << '/' << s.method << ',' << boolean(s.parallel) << '\n';
        if (!first) json << ",\n";
        first = false;
        summary_json(json, s);
        json << "\n}";
        by_case[s.instance][s.method] = s;
    }
    json << "\n]\n";
    for (const auto& [id, methods] : by_case) {
        compare << id;
        for (const auto* method : {"Proposed", "Approximate_PMC"}) {
            const auto it = methods.find(method);
            if (it == methods.end()) compare << ",,,";
            else compare << ',' << it->second.final << ',' << it->second.wall_seconds << ',' << it->second.status;
        }
        const auto it = methods.find(constant_method);
        if (it == methods.end()) compare << ",,,,,,";
        else {
            const auto& s = it->second;
            const auto& c = s.constant_step.value();
            compare << ',' << s.final << ',' << c.best_objective << ',' << c.best_iteration << ','
                    << s.wall_seconds << ',' << s.status << ',' << boolean(c.cycle.has_value());
        }
        compare << '\n';
    }
    csv.close();
    json.close();
    compare.close();
}
}
