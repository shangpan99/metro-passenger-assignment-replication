#include "batch_lock.hpp"
#include "input.hpp"
#include "report.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace metro;
using Clock = std::chrono::steady_clock;

struct Options {
    fs::path data = "data";
    fs::path output;
    std::vector<std::string> selections;
    std::vector<std::string> methods = {"Approximate_PMC", "Proposed"};
    double time_limit = 1800;
    bool parallel = false, list = false, dry_run = false, help = false;
};

struct CaseCatalog {
    std::vector<std::string> ids;
    std::map<std::string, fs::path> roots;
};

CaseCatalog read_catalog(const fs::path& data) {
    std::vector<fs::path> directories;
    if (fs::is_regular_file(data / "INSTANCE_INDEX.csv")) {
        directories.push_back(data);
    } else {
        for (const auto& entry : fs::directory_iterator(data)) {
            if (entry.is_directory() && fs::is_regular_file(entry.path() / "INSTANCE_INDEX.csv"))
                directories.push_back(entry.path());
        }
        std::sort(directories.begin(), directories.end());
    }
    CaseCatalog catalog;
    for (const auto& dir : directories) {
        const auto ids = list_cases(dir);
        if (ids.empty()) throw std::runtime_error("Empty instance index: " + dir.u8string());
        for (const auto& id : ids) {
            if (!catalog.roots.emplace(id, dir).second)
                throw std::runtime_error("Duplicate instance ID across data directories: " + id);
            catalog.ids.push_back(id);
        }
    }
    if (catalog.ids.empty())
        throw std::runtime_error("No INSTANCE_INDEX.csv found under: " + data.u8string());
    return catalog;
}

bool flag(const std::string& s) { return s.rfind("--", 0) == 0 || s == "-h"; }

Options options(int argc, char** argv) {
    Options o;
    bool mode_set = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto take = [&] {
            if (i + 1 == argc || flag(argv[i + 1]))
                throw std::runtime_error("Missing value for " + arg);
            return std::string(argv[++i]);
        };
        if (arg == "--help" || arg == "-h") o.help = true;
        else if (arg == "--list") o.list = true;
        else if (arg == "--dry-run") o.dry_run = true;
        else if (arg == "--data") o.data = fs::u8path(take());
        else if (arg == "--output") o.output = fs::u8path(take());
        else if (arg == "--time-limit") {
            const auto value = take();
            std::size_t consumed = 0;
            o.time_limit = std::stod(value, &consumed);
            const double maximum = std::chrono::duration<double>(Clock::duration::max()).count() / 4;
            if (consumed != value.size() || !std::isfinite(o.time_limit) ||
                o.time_limit <= 0 || o.time_limit > maximum)
                throw std::runtime_error("Invalid --time-limit");
        } else if (arg == "--parallel" || arg == "--serial") {
            if (mode_set) throw std::runtime_error("Specify --parallel or --serial only once");
            mode_set = true;
            o.parallel = arg == "--parallel";
        } else if (arg == "--cases" || arg == "--instances") {
            o.selections.push_back(take());
            while (i + 1 < argc && !flag(argv[i + 1]))
                o.selections.emplace_back(argv[++i]);
        } else if (arg == "--method") {
            const auto method = take();
            if (method == "sar") o.methods = {"Approximate_PMC"};
            else if (method == "proposed") o.methods = {"Proposed"};
            else if (method == "both") o.methods = {"Approximate_PMC", "Proposed"};
            else throw std::runtime_error("Use --method sar, proposed, or both");
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }
    return o;
}

std::string upper(std::string value) {
    for (char& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

std::string short_name(const std::string& id) {
    const auto r = id.find("_R");
    const auto w = id.find("_W", r);
    if (r == std::string::npos || w == std::string::npos)
        throw std::runtime_error("Invalid case directory: " + id);
    return id.substr(r + 1, w - r - 1) + id.substr(w + 1);
}

std::vector<std::string> resolve(std::vector<std::string> selection,
                                 const std::vector<std::string>& available) {
    if (selection.empty()) return available;
    std::vector<std::string> tokens;
    for (auto s : selection) {
        for (const std::string delim : {",", ";", "，", "；", "、"}) {
            std::size_t pos = 0;
            while ((pos = s.find(delim, pos)) != std::string::npos)
                s.replace(pos++, delim.size(), " ");
        }
        std::istringstream in(s);
        std::string token;
        while (in >> token) tokens.push_back(upper(token));
    }
    if (tokens.empty()) throw std::runtime_error("Select at least one case");
    std::set<std::string> wanted;
    const std::regex alias("R([0-9]+)[_-]?W([0-9]+)");
    for (const auto& token : tokens) {
        if (token == "ALL" || token == "全部") {
            if (tokens.size() != 1) throw std::runtime_error("ALL must be used alone");
            return available;
        }
        std::string key = token;
        std::smatch match;
        if (std::regex_match(token, match, alias)) {
            std::ostringstream s;
            s << 'R' << std::setw(2) << std::setfill('0') << std::stoi(match[1]) << 'W' << std::stoi(match[2]);
            key = s.str();
        }
        bool matched = false;
        for (const auto& id : available) {
            if (upper(id) == key || short_name(id) == key) {
                wanted.insert(id);
                matched = true;
            }
        }
        if (!matched) throw std::runtime_error("Unknown case: " + token + " (see --list)");
    }
    std::vector<std::string> cases;
    for (const auto& id : available)
        if (wanted.count(id)) cases.push_back(id);
    return cases;
}

fs::path default_output(const std::vector<std::string>& cases, const Options& o) {
    std::ostringstream label;
    if (cases.size() <= 3) {
        const char* sep = "";
        for (const auto& id : cases) {
            label << sep << id;
            sep = "_";
        }
    } else {
        // Stable FNV-1a identifier. The full selection is checked in batch_config.json.
        std::uint64_t hash = 14695981039346656037ull;
        for (const auto& id : cases) {
            for (const unsigned char c : id) hash = (hash ^ c) * 1099511628211ull;
            hash = (hash ^ 0) * 1099511628211ull;
        }
        label << cases.size() << "cases_" << std::hex << hash << std::dec;
    }
    label << (o.parallel ? "_parallel" : "_serial");
    if (o.methods.size() == 1) label << '_' << o.methods.front();
    if (o.time_limit != 1800) label << "_T" << std::setprecision(15) << o.time_limit;
    return fs::path("outputs") / "multiscale_cpp" / label.str();
}

std::string configuration(const std::vector<std::string>& cases, const CaseCatalog& catalog,
                          const Options& o) {
    std::ostringstream out;
    out << "{\n  \"schema\": \"cpp-multiscale-v1\",\n  \"data_directory\": "
        << json_string(fs::canonical(o.data).generic_u8string())
        << ",\n  \"time_limit\": " << std::setprecision(17) << o.time_limit
        << ",\n  \"parallel_methods\": " << (o.parallel ? "true" : "false") << ",\n  \"methods\": [";
    for (std::size_t i = 0; i < o.methods.size(); ++i)
        out << (i ? ", " : "") << json_string(o.methods[i]);
    out << "],\n  \"cases\": [";
    for (std::size_t i = 0; i < cases.size(); ++i)
        out << (i ? ", " : "") << json_string(cases[i]);
    out << "],\n  \"case_data_directories\": {";
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& id = cases[i];
        out << (i ? ",\n    " : "\n    ") << json_string(id) << ": "
            << json_string(fs::canonical(catalog.roots.at(id)).generic_u8string());
    }
    out << "\n  }\n}\n";
    return out.str();
}

void ensure_config(const fs::path& path, const std::string& expected) {
    if (fs::exists(path)) {
        std::ifstream in(path, std::ios::binary);
        const std::string saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (saved != expected)
            throw std::runtime_error("Output contains a different batch configuration. Use another --output directory.");
        return;
    }
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << expected;
    out.close();
    fs::rename(temporary, path);
}

std::string fingerprint(const CaseInput& in) {
    std::ostringstream s;
    s << in.assignment_sha256 << '\n';
    for (const auto& [name, hash] : in.input_sha256)
        s << name << ' ' << hash << '\n';
    return s.str();
}

MethodSummary run_method(const CaseInput& input, const std::string& id,
                         const std::string& method, const Options& o, Clock::time_point started) {
    const fs::path dir = o.output / id / method;
    const auto signature = fingerprint(input);
    if (auto previous = read_completion(dir)) {
        std::ifstream file(dir / "input_fingerprint.txt", std::ios::binary);
        const std::string stored((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (stored != signature || previous->instance != id || previous->method != method ||
            previous->assignment_sha256 != input.assignment_sha256 ||
            !fs::is_regular_file(dir / "run_summary.json") ||
            !fs::is_regular_file(dir / "raw/iteration_summary.csv"))
            throw std::runtime_error("Invalid saved task: " + dir.u8string() + ". Use another --output directory.");
        return *previous;
    }
    if (fs::exists(dir))
        throw std::runtime_error("Output task directory already exists: " + dir.u8string());
    fs::path staging = dir;
    staging += ".incomplete";
    fs::remove_all(staging);
    fs::create_directories(staging);
    const auto deadline = started + std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(o.time_limit));
    const bool proposed = method == "Proposed";
    const auto result = proposed ? search_proposed(input.data, input.agents, deadline)
                                 : search_sar(input.data, input.agents, deadline);
    const auto computation = std::chrono::duration<double>(Clock::now() - started).count();
    const Loader loader(input.data);
    const auto initial = loader.evaluate(input.agents);
    const auto final = loader.evaluate(result.agents);
    if (!initial.feasible || !final.feasible || initial.objective != input.initial_objective ||
        initial.objective != result.initial_objective || final.objective != result.final_objective)
        throw std::runtime_error("Assignment replay failed: " + id + "/" + method);
    MethodSummary summary;
    summary.instance = id;
    summary.method = method;
    summary.status = result.status;
    summary.assignment_sha256 = input.assignment_sha256;
    summary.initial = initial.objective;
    summary.final = final.objective;
    summary.search_seconds = computation;
    summary.accepted = static_cast<int>(result.updates.size());
    summary.max_order = result.max_order_checked;
    summary.certified = result.status == "BLOCK_AND_NEUTRAL_TWO_STEP_LOCAL_OPTIMUM";
    summary.parallel = o.parallel && o.methods.size() == 2;
    {
        std::ofstream file(staging / "input_fingerprint.txt", std::ios::binary);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file << signature;
        file.close();
    }
    {
        std::ofstream file(staging / "initialization_metadata.json", std::ios::binary);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file << input.initialization_metadata;
        file.close();
    }
    write_task(staging, input.data, initial, final, result, summary, o.time_limit, started);
    fs::rename(staging, dir);
    return summary;
}

void show_help() {
    std::cout << "run_all_scales [--cases N0100_R02_W5 R03W1 | ALL] [--data PATH] [--output PATH]\n"
                 "            [--parallel | --serial] [--time-limit SECONDS] [--method both|sar|proposed]\n"
                 "            [--list] [--dry-run]\n"
                 "Defaults: all cases, both methods in sequence, 1800 seconds per method.\n"
                 "Short case IDs select matching cases at every loaded scale.\n";
}

int main_impl(int argc, char** argv) {
    auto o = options(argc, argv);
    if (o.help) {
        show_help();
        return 0;
    }
    const auto catalog = read_catalog(o.data);
    if (o.list) {
        for (const auto& id : catalog.ids) std::cout << id << '\n';
        return 0;
    }
    const auto cases = resolve(o.selections, catalog.ids);
    if (o.output.empty()) o.output = default_output(cases, o);
    std::cout << (o.dry_run ? "DRY RUN" : "RUN") << " | " << cases.size() << " cases | "
              << (o.parallel && o.methods.size() == 2 ? "parallel methods" : "serial methods")
              << " | " << o.time_limit << " s/method\n"
              << "OUTPUT " << fs::absolute(o.output).u8string() << std::endl;
    std::optional<BatchLock> lock;
    if (!o.dry_run) {
        fs::create_directories(o.output);
        lock.emplace(o.output / ".batch.lock");
        ensure_config(o.output / "batch_config.json", configuration(cases, catalog, o));
    }
    std::vector<MethodSummary> summaries;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& id = cases[i];
        const auto load_started = Clock::now();
        const auto input = read_case(catalog.roots.at(id), id);
        const auto load_time = Clock::now() - load_started;
        if (o.dry_run) {
            const auto initial = Loader(input.data).evaluate(input.agents);
            if (!initial.feasible || initial.objective != input.initial_objective)
                throw std::runtime_error("Initial assignment replay failed: " + id);
            for (const auto& method : o.methods)
                std::cout << id << ' ' << method << " | passengers " << input.agents.size()
                          << " | initial " << input.initial_objective << '\n';
            continue;
        }
        std::cout << "CASE " << (i + 1) << '/' << cases.size() << ' ' << id << " | "
                  << input.agents.size() << " passengers" << std::endl;
        const auto run = [&](const std::string& method) {
            return run_method(input, id, method, o, Clock::now() - load_time);
        };
        const auto record = [&](MethodSummary s) {
            std::cout << "FINISHED " << s.instance << ' ' << s.method << ' ' << s.status << ' '
                      << s.initial << " -> " << s.final << " (" << s.accepted << " updates)" << std::endl;
            summaries.push_back(std::move(s));
        };
        if (o.parallel && o.methods.size() == 2) {
            auto first = std::async(std::launch::async, run, o.methods.front());
            auto second = std::async(std::launch::async, run, o.methods.back());
            record(first.get());
            record(second.get());
        } else {
            for (const auto& method : o.methods) {
                std::cout << "START " << id << ' ' << method << std::endl;
                record(run(method));
                write_batch(o.output, summaries);
            }
        }
        if (o.parallel && o.methods.size() == 2) write_batch(o.output, summaries);
    }
    return 0;
}
}

int run(int argc, char** argv) {
    try {
        return main_impl(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "ERROR " << e.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                                               nullptr, 0, nullptr, nullptr);
        if (length <= 0) {
            std::cerr << "ERROR Invalid Unicode argument\n";
            return 1;
        }
        std::string value(static_cast<std::size_t>(length), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(), length,
                                 nullptr, nullptr)) {
            std::cerr << "ERROR Invalid Unicode argument\n";
            return 1;
        }
        value.pop_back();
        arguments.push_back(std::move(value));
    }
    std::vector<char*> pointers;
    pointers.reserve(arguments.size());
    for (auto& value : arguments) pointers.push_back(value.data());
    return run(argc, pointers.data());
}
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
