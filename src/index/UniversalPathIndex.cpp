#include "index/UniversalPathIndex.h"

#include "index/EquivalenceIndex.h"
#include "index/FactorIndex.h"
#include "index/SimilarityCertificateIndex.h"
#include "index/SelectiveEquivalenceIndex.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace hinscan {
namespace {

constexpr const char* kManifestName = "manifest.upi";
using Wide = unsigned __int128;

bool threshold_less(const SimilarityThreshold& left,
                    const SimilarityThreshold& right) {
    return static_cast<Wide>(left.numerator) * right.denominator <
           static_cast<Wide>(right.numerator) * left.denominator;
}

bool threshold_equal(const SimilarityThreshold& left,
                     const SimilarityThreshold& right) {
    return !threshold_less(left, right) && !threshold_less(right, left);
}

void normalize_coverage_floors(
    std::vector<SimilarityThreshold>* coverage_floors) {
    if (coverage_floors->empty()) {
        throw std::invalid_argument("at least one SECI coverage floor is required");
    }
    for (const auto& floor : *coverage_floors) {
        if (floor.numerator == 0 || floor.denominator == 0 ||
            floor.numerator > floor.denominator) {
            throw std::invalid_argument("SECI coverage floors must be in (0,1]");
        }
    }
    std::sort(coverage_floors->begin(), coverage_floors->end(), threshold_less);
    coverage_floors->erase(
        std::unique(coverage_floors->begin(), coverage_floors->end(),
                    threshold_equal),
        coverage_floors->end());
}

std::filesystem::path selective_relative_file(
    const UniversalPathEntry& entry,
    const SimilarityThreshold& coverage_floor) {
    auto file = entry.relative_file;
    file.replace_extension(".seci-" +
                           std::to_string(coverage_floor.numerator) + "-" +
                           std::to_string(coverage_floor.denominator));
    return file;
}

std::string path_key(const std::vector<std::uint32_t>& path) {
    std::ostringstream output;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i != 0) {
            output << '-';
        }
        output << path[i];
    }
    return output.str();
}

std::vector<std::uint32_t> parse_path_key(const std::string& key) {
    std::vector<std::uint32_t> path;
    std::istringstream input(key);
    std::string token;
    while (std::getline(input, token, '-')) {
        if (token.empty()) {
            throw std::runtime_error("invalid empty component in UPI path key");
        }
        std::size_t consumed = 0;
        const auto value = std::stoul(token, &consumed);
        if (consumed != token.size()) {
            throw std::runtime_error("invalid UPI path key: " + key);
        }
        path.push_back(static_cast<std::uint32_t>(value));
    }
    if (path.size() < 2) {
        throw std::runtime_error("UPI half-path must contain a relation step");
    }
    return path;
}

std::vector<std::string> split_meta_path(const std::string& specification) {
    std::vector<std::string> tokens;
    std::istringstream input(specification);
    std::string token;
    while (std::getline(input, token, '-')) {
        if (token.empty()) {
            throw std::invalid_argument("meta-path contains an empty type token");
        }
        tokens.push_back(token);
    }
    return tokens;
}

void require_label(std::istream& input,
                   const std::string& expected,
                   const std::filesystem::path& manifest) {
    std::string actual;
    if (!(input >> actual) || actual != expected) {
        throw std::runtime_error(manifest.string() +
                                 ": expected manifest field " + expected);
    }
}

void enumerate_walks(const std::vector<std::vector<std::uint32_t>>& adjacency,
                     std::uint32_t max_half_length,
                     std::uint64_t max_paths,
                     std::vector<std::vector<std::uint32_t>>* paths) {
    if (max_paths == 0) { return; }
    std::vector<std::vector<std::uint32_t>> frontier;
    frontier.reserve(adjacency.size());
    for (std::uint32_t start = 0; start < adjacency.size(); ++start) {
        frontier.push_back({start});
    }
    for (std::uint32_t length = 1; length <= max_half_length; ++length) {
        std::vector<std::vector<std::uint32_t>> next;
        for (const auto& prefix : frontier) {
            for (const auto neighbor : adjacency[prefix.back()]) {
                // The offline fast tier remains non-backtracking. Paths not
                // selected here are still supported exactly through BRI.
                if (prefix.size() >= 2 &&
                    neighbor == prefix[prefix.size() - 2]) {
                    continue;
                }
                auto path = prefix;
                path.push_back(neighbor);
                paths->push_back(path);
                next.push_back(std::move(path));
                if (paths->size() >= max_paths) {
                    std::sort(paths->begin(), paths->end());
                    return;
                }
            }
        }
        frontier.swap(next);
        if (frontier.empty()) { break; }
    }
    std::sort(paths->begin(), paths->end());
    paths->erase(std::unique(paths->begin(), paths->end()), paths->end());
}

std::vector<std::uint32_t> symmetric_path(
    const std::vector<std::uint32_t>& half_path) {
    auto result = half_path;
    for (std::size_t i = half_path.size() - 1; i-- > 0;) {
        result.push_back(half_path[i]);
    }
    return result;
}

}  // namespace

UniversalPathIndex UniversalPathIndex::build(
    const HinGraph& graph,
    const std::filesystem::path& index_directory,
    std::uint32_t max_half_length,
    std::uint32_t certificate_budget_percent,
    std::vector<SimilarityThreshold> selective_coverage_floors,
    std::uint64_t max_precomputed_paths) {
    if (max_half_length == 0) {
        throw std::invalid_argument("max half-path length must be positive");
    }

    const auto begin = std::chrono::steady_clock::now();
    UniversalPathIndex result;
    result.directory_ = std::filesystem::absolute(index_directory);
    result.vertex_types_ = graph.vertex_types();
    result.stats_.max_half_length = max_half_length;
    result.stats_.certificate_budget_percent = certificate_budget_percent;
    result.stats_.precomputed_path_limit = max_precomputed_paths;
    normalize_coverage_floors(&selective_coverage_floors);
    result.selective_coverage_floors_ = selective_coverage_floors;
    result.stats_.selective_coverage_numerator =
        selective_coverage_floors.front().numerator;
    result.stats_.selective_coverage_denominator =
        selective_coverage_floors.front().denominator;
    if (std::filesystem::exists(result.directory_) &&
        !std::filesystem::is_empty(result.directory_)) {
        throw std::invalid_argument(
            "UPI output directory must be empty: " + result.directory_.string());
    }
    std::filesystem::create_directories(result.directory_ / "paths");
    result.base_relation_relative_file_ = "base.bri";
    graph.save_binary(result.directory_ /
                      result.base_relation_relative_file_);
    result.stats_.base_relation_file_bytes = std::filesystem::file_size(
        result.directory_ / result.base_relation_relative_file_);
    result.stats_.total_file_bytes += result.stats_.base_relation_file_bytes;

    const auto type_count = graph.vertex_types().size();
    std::vector<std::vector<std::uint32_t>> adjacency(type_count);
    std::unordered_map<std::uint64_t, std::uint32_t> pair_counts;
    for (const auto& relation : graph.relations()) {
        const auto small = std::min(relation.source_type, relation.target_type);
        const auto large = std::max(relation.source_type, relation.target_type);
        const auto key = (static_cast<std::uint64_t>(small) << 32U) | large;
        ++pair_counts[key];
    }
    for (const auto& item : pair_counts) {
        if (item.second != 1) {
            continue;
        }
        const auto left = static_cast<std::uint32_t>(item.first >> 32U);
        const auto right = static_cast<std::uint32_t>(item.first);
        adjacency[left].push_back(right);
        if (left != right) {
            adjacency[right].push_back(left);
        }
    }
    for (auto& neighbors : adjacency) {
        std::sort(neighbors.begin(), neighbors.end());
    }

    std::vector<std::vector<std::uint32_t>> half_paths;
    enumerate_walks(adjacency, max_half_length, max_precomputed_paths,
                    &half_paths);
    result.entries_.reserve(half_paths.size());
    for (const auto& half_path : half_paths) {
        UniversalPathEntry entry;
        entry.half_path = half_path;
        entry.relative_file = std::filesystem::path("paths") /
                              (path_key(half_path) + ".fli");
        const auto index = FactorIndex::build(graph, symmetric_path(half_path));
        index.save(result.directory_ / entry.relative_file);
        entry.file_bytes = std::filesystem::file_size(
            result.directory_ / entry.relative_file);
        entry.half_path_incidences = index.stats().half_path_incidences;
        entry.projected_edges = index.stats().exact_projected_edges;
        entry.build_milliseconds = index.stats().build_milliseconds;
        result.stats_.total_file_bytes += entry.file_bytes;
        result.stats_.total_half_path_incidences += entry.half_path_incidences;

        // Dense projections are the cases where repeated neighborhoods can
        // dominate online time. Build an exact quotient only after a cheap
        // density gate, and retain it only when it gives strong compression.
        if (index.stats().exact_projected_edges >=
            128 * std::max<std::uint64_t>(1, index.vertex_count())) {
            const auto equivalence = EquivalenceIndex::build(index);
            const bool class_compresses =
                equivalence.stats().classes * 4 <= index.vertex_count();
            const bool edge_compresses =
                equivalence.stats().quotient_edges * 16 <=
                index.stats().exact_projected_edges;
            if (class_compresses && edge_compresses) {
                auto equivalence_file = entry.relative_file;
                equivalence_file.replace_extension(".eqi");
                equivalence.save(result.directory_ / equivalence_file);
                const auto bytes = std::filesystem::file_size(
                    result.directory_ / equivalence_file);
                ++result.stats_.equivalence_entries;
                result.stats_.equivalence_file_bytes += bytes;
                result.stats_.total_file_bytes += bytes;
            }
        }
        result.entries_.push_back(std::move(entry));
    }
    result.stats_.path_entries = result.entries_.size();

    // Use at most a configurable fraction of the already-built FLI/EQI size
    // for all time-oriented auxiliary structures. Dense projections first try
    // threshold-addressable SECI; other paths use a full SCI when it fits.
    // One SECI file stores the lowest affordable floor and query loading reads
    // only the certificate prefix required by epsilon. Larger projections are
    // considered first because they offer the largest potential time saving.
    const auto base_index_bytes = result.stats_.total_file_bytes;
    const auto certificate_budget =
        base_index_bytes > std::numeric_limits<std::uint64_t>::max() /
                               std::max<std::uint32_t>(1,
                                                       certificate_budget_percent)
            ? std::numeric_limits<std::uint64_t>::max()
            : base_index_bytes * certificate_budget_percent / 100;
    std::vector<std::size_t> certificate_candidates(result.entries_.size());
    std::iota(certificate_candidates.begin(), certificate_candidates.end(), 0);
    std::sort(certificate_candidates.begin(), certificate_candidates.end(),
              [&](std::size_t left, std::size_t right) {
                  return result.entries_[left].projected_edges >
                         result.entries_[right].projected_edges;
              });
    for (const auto entry_id : certificate_candidates) {
        const auto& entry = result.entries_[entry_id];
        auto equivalence_file = entry.relative_file;
        equivalence_file.replace_extension(".eqi");
        if (std::filesystem::exists(result.directory_ / equivalence_file)) {
            continue;
        }
        const auto auxiliary_bytes = result.stats_.certificate_file_bytes +
                                     result.stats_.selective_file_bytes;
        auto remaining = auxiliary_bytes >= certificate_budget
                             ? 0
                             : certificate_budget - auxiliary_bytes;
        const auto vertices = result.vertex_types_[entry.half_path.front()].count;
        const long double materialized_bytes =
            12.0L + static_cast<long double>(vertices) * 4.0L +
            static_cast<long double>(entry.projected_edges) * 8.0L;

        std::optional<FactorIndex> loaded_factor;
        const bool dense_projection =
            entry.projected_edges >=
            16 * std::max<std::uint64_t>(1, vertices);
        if (remaining != 0 && dense_projection) {
            loaded_factor.emplace(FactorIndex::load(
                result.directory_ / entry.relative_file));
            const auto selective_base = SelectiveEquivalenceIndex::build(
                *loaded_factor, selective_coverage_floors.front());
            bool saved_selective_index = false;
            for (std::size_t floor_id = 0;
                 floor_id < selective_coverage_floors.size(); ++floor_id) {
                const auto& floor = selective_coverage_floors[floor_id];
                if (floor_id == 0) {
                    const auto bytes = selective_base.serialized_bytes();
                    if (bytes > remaining ||
                        static_cast<long double>(bytes) > materialized_bytes) {
                        continue;
                    }
                    const auto relative = selective_relative_file(entry, floor);
                    selective_base.save(result.directory_ / relative);
                    const auto actual_bytes = std::filesystem::file_size(
                        result.directory_ / relative);
                    ++result.stats_.selective_entries;
                    result.stats_.selective_file_bytes += actual_bytes;
                    result.stats_.total_file_bytes += actual_bytes;
                    saved_selective_index = true;
                    break;
                }

                const auto layer = selective_base.derive_coverage_layer(floor);
                const auto bytes = layer.serialized_bytes();
                if (bytes > remaining ||
                    static_cast<long double>(bytes) > materialized_bytes) {
                    continue;
                }
                const auto relative = selective_relative_file(entry, floor);
                layer.save(result.directory_ / relative);
                const auto actual_bytes = std::filesystem::file_size(
                    result.directory_ / relative);
                ++result.stats_.selective_entries;
                result.stats_.selective_file_bytes += actual_bytes;
                result.stats_.total_file_bytes += actual_bytes;
                saved_selective_index = true;
                break;
            }
            if (saved_selective_index) {
                continue;
            }
        }

        std::uint32_t id_bits = 0;
        auto maximum_id = vertices == 0 ? 0 : vertices - 1;
        do {
            ++id_bits;
            maximum_id >>= 1U;
        } while (maximum_id != 0);
        constexpr std::uint64_t sci_header_bytes = 56;
        const long double minimum_sci_bytes =
            static_cast<long double>(sci_header_bytes) +
            static_cast<long double>(vertices) / 8.0L +
            static_cast<long double>(entry.projected_edges) *
                static_cast<long double>(2 * id_bits + 1) / 8.0L;
        if (minimum_sci_bytes > static_cast<long double>(remaining) ||
            minimum_sci_bytes > materialized_bytes) {
            continue;
        }

        if (!loaded_factor) {
            loaded_factor.emplace(FactorIndex::load(
                result.directory_ / entry.relative_file));
        }
        const auto certificates =
            SimilarityCertificateIndex::build(*loaded_factor);
        const auto bytes = certificates.serialized_bytes();
        if (bytes > remaining ||
            static_cast<long double>(bytes) > materialized_bytes) {
            continue;
        }
        auto certificate_file = entry.relative_file;
        certificate_file.replace_extension(".sci");
        certificates.save(result.directory_ / certificate_file);
        const auto actual_bytes = std::filesystem::file_size(
            result.directory_ / certificate_file);
        ++result.stats_.certificate_entries;
        result.stats_.certificate_file_bytes += actual_bytes;
        result.stats_.total_file_bytes += actual_bytes;
    }
    const auto end = std::chrono::steady_clock::now();
    result.stats_.build_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
    const auto factor_bytes = result.stats_.total_file_bytes;
    result.save_manifest();
    for (int iteration = 0; iteration < 3; ++iteration) {
        const auto total = factor_bytes +
                           std::filesystem::file_size(result.directory_ /
                                                      kManifestName);
        if (total == result.stats_.total_file_bytes) {
            break;
        }
        result.stats_.total_file_bytes = total;
        result.save_manifest();
    }
    return result;
}

UniversalPathIndex UniversalPathIndex::load(
    const std::filesystem::path& index_directory) {
    UniversalPathIndex result;
    result.directory_ = std::filesystem::absolute(index_directory);
    const auto manifest = result.directory_ / kManifestName;
    std::ifstream input(manifest);
    if (!input) {
        throw std::runtime_error("cannot open UPI manifest: " + manifest.string());
    }

    std::string magic;
    std::uint32_t version = 0;
    if (!(input >> magic >> version) || magic != "UPI" ||
        (version != 1 && version != 2 && version != 3 && version != 4 &&
         version != 5 && version != 6)) {
        throw std::runtime_error("unsupported UPI manifest: " + manifest.string());
    }
    require_label(input, "max_half_length", manifest);
    input >> result.stats_.max_half_length;
    require_label(input, "path_policy", manifest);
    std::string path_policy;
    input >> path_policy;
    if (path_policy != "non_backtracking") {
        throw std::runtime_error("unsupported UPI path policy: " + path_policy);
    }
    require_label(input, "total_file_bytes", manifest);
    input >> result.stats_.total_file_bytes;
    require_label(input, "total_half_path_incidences", manifest);
    input >> result.stats_.total_half_path_incidences;
    require_label(input, "build_milliseconds", manifest);
    input >> result.stats_.build_milliseconds;
    if (version >= 2) {
        require_label(input, "equivalence_entries", manifest);
        input >> result.stats_.equivalence_entries;
        require_label(input, "equivalence_file_bytes", manifest);
        input >> result.stats_.equivalence_file_bytes;
    }
    if (version >= 3) {
        require_label(input, "certificate_entries", manifest);
        input >> result.stats_.certificate_entries;
        require_label(input, "certificate_file_bytes", manifest);
        input >> result.stats_.certificate_file_bytes;
        require_label(input, "certificate_budget_percent", manifest);
        input >> result.stats_.certificate_budget_percent;
    }
    if (version >= 4) {
        require_label(input, "selective_entries", manifest);
        input >> result.stats_.selective_entries;
        require_label(input, "selective_file_bytes", manifest);
        input >> result.stats_.selective_file_bytes;
        require_label(input, "selective_coverage_numerator", manifest);
        input >> result.stats_.selective_coverage_numerator;
        require_label(input, "selective_coverage_denominator", manifest);
        input >> result.stats_.selective_coverage_denominator;
        if (result.stats_.selective_coverage_denominator == 0) {
            throw std::runtime_error("invalid UPI selective coverage floor");
        }
        if (version == 4 && result.stats_.selective_entries != 0) {
            result.selective_coverage_floors_.push_back(
                {result.stats_.selective_coverage_numerator,
                 result.stats_.selective_coverage_denominator});
        }
    }
    if (version >= 5) {
        require_label(input, "selective_coverage_count", manifest);
        std::uint64_t floor_count = 0;
        input >> floor_count;
        if (floor_count == 0 || floor_count > 1024) {
            throw std::runtime_error("invalid UPI SECI coverage floor count");
        }
        result.selective_coverage_floors_.reserve(
            static_cast<std::size_t>(floor_count));
        for (std::uint64_t i = 0; i < floor_count; ++i) {
            require_label(input, "selective_coverage", manifest);
            SimilarityThreshold floor;
            input >> floor.numerator >> floor.denominator;
            result.selective_coverage_floors_.push_back(floor);
        }
        normalize_coverage_floors(&result.selective_coverage_floors_);
    }
    if (version >= 6) {
        require_label(input, "base_relation_file", manifest);
        std::string relative_file;
        input >> std::quoted(relative_file);
        result.base_relation_relative_file_ = relative_file;
        require_label(input, "base_relation_file_bytes", manifest);
        input >> result.stats_.base_relation_file_bytes;
        require_label(input, "precomputed_path_limit", manifest);
        input >> result.stats_.precomputed_path_limit;
        if (!std::filesystem::exists(result.base_relation_file()) ||
            std::filesystem::file_size(result.base_relation_file()) !=
                result.stats_.base_relation_file_bytes) {
            throw std::runtime_error("UPI base relation index is missing or changed");
        }
    }

    require_label(input, "type_count", manifest);
    std::uint64_t type_count = 0;
    input >> type_count;
    result.vertex_types_.resize(static_cast<std::size_t>(type_count));
    for (std::uint64_t expected_id = 0; expected_id < type_count; ++expected_id) {
        require_label(input, "type", manifest);
        std::uint64_t type_id = 0;
        input >> type_id >> std::quoted(result.vertex_types_[expected_id].name) >>
            result.vertex_types_[expected_id].count;
        if (type_id != expected_id) {
            throw std::runtime_error("UPI manifest type ids are not contiguous");
        }
    }

    require_label(input, "entry_count", manifest);
    input >> result.stats_.path_entries;
    result.entries_.resize(static_cast<std::size_t>(result.stats_.path_entries));
    for (auto& entry : result.entries_) {
        require_label(input, "entry", manifest);
        std::string key;
        std::string relative_file;
        input >> key >> std::quoted(relative_file) >> entry.file_bytes >>
            entry.half_path_incidences >> entry.projected_edges >>
            entry.build_milliseconds;
        entry.half_path = parse_path_key(key);
        entry.relative_file = relative_file;
        for (const auto type : entry.half_path) {
            if (type >= result.vertex_types_.size()) {
                throw std::runtime_error("UPI entry refers to an unknown type");
            }
        }
        if (!std::filesystem::exists(result.entry_file(entry))) {
            throw std::runtime_error("UPI entry file is missing: " +
                                     result.entry_file(entry).string());
        }
    }
    if (!input) {
        throw std::runtime_error("truncated UPI manifest: " + manifest.string());
    }
    return result;
}

std::vector<std::uint32_t> UniversalPathIndex::parse_symmetric_meta_path(
    const std::string& meta_path) const {
    const auto tokens = split_meta_path(meta_path);
    if (tokens.size() < 3 || tokens.size() % 2 == 0) {
        throw std::invalid_argument(
            "online query requires an odd-length symmetric meta-path");
    }
    std::vector<std::uint32_t> path;
    path.reserve(tokens.size());
    for (const auto& token : tokens) {
        path.push_back(type_id(token));
    }
    for (std::size_t i = 0, j = path.size() - 1; i < j; ++i, --j) {
        if (path[i] != path[j]) {
            throw std::invalid_argument("online meta-path is not symmetric");
        }
    }
    return path;
}

const UniversalPathEntry* UniversalPathIndex::find(
    const std::string& meta_path) const {
    const auto path = parse_symmetric_meta_path(meta_path);
    const auto half_size = path.size() / 2 + 1;
    const std::vector<std::uint32_t> half(path.begin(), path.begin() + half_size);
    const auto match = std::lower_bound(
        entries_.begin(), entries_.end(), half,
        [](const UniversalPathEntry& entry,
           const std::vector<std::uint32_t>& value) {
            return entry.half_path < value;
        });
    if (match == entries_.end() || match->half_path != half) {
        return nullptr;
    }
    return &*match;
}

const UniversalPathEntry& UniversalPathIndex::resolve(
    const std::string& meta_path) const {
    const auto* match = find(meta_path);
    if (match == nullptr) {
        throw std::invalid_argument(
            has_base_relation_index()
                ? "meta-path is not precomputed; use adaptive BRI fallback"
                : "meta-path is not supported by this legacy UPI index");
    }
    return *match;
}

std::filesystem::path UniversalPathIndex::entry_file(
    const UniversalPathEntry& entry) const {
    const auto candidate = (directory_ / entry.relative_file).lexically_normal();
    const auto relative = candidate.lexically_relative(directory_);
    if (relative.empty() || *relative.begin() == "..") {
        throw std::runtime_error("UPI entry escapes the index directory");
    }
    return candidate;
}

std::filesystem::path UniversalPathIndex::selective_file(
    const UniversalPathEntry& entry,
    const SimilarityThreshold& coverage_floor) const {
    const auto candidate =
        (directory_ / selective_relative_file(entry, coverage_floor))
            .lexically_normal();
    const auto relative = candidate.lexically_relative(directory_);
    if (relative.empty() || *relative.begin() == "..") {
        throw std::runtime_error("UPI SECI entry escapes the index directory");
    }
    return candidate;
}

bool UniversalPathIndex::has_base_relation_index() const noexcept {
    return !base_relation_relative_file_.empty();
}

std::filesystem::path UniversalPathIndex::base_relation_file() const {
    if (!has_base_relation_index()) {
        throw std::runtime_error("UPI does not contain a base relation index");
    }
    const auto candidate =
        (directory_ / base_relation_relative_file_).lexically_normal();
    const auto relative = candidate.lexically_relative(directory_);
    if (relative.empty() || *relative.begin() == "..") {
        throw std::runtime_error("UPI base relation index escapes its directory");
    }
    return candidate;
}

std::filesystem::path UniversalPathIndex::cached_factor_file(
    const std::string& meta_path) const {
    const auto path = parse_symmetric_meta_path(meta_path);
    const auto half_size = path.size() / 2 + 1;
    const std::vector<std::uint32_t> half(path.begin(),
                                          path.begin() + half_size);
    return (directory_ / "cache" / (path_key(half) + ".fli"))
        .lexically_normal();
}

std::filesystem::path UniversalPathIndex::cached_selective_file(
    const std::string& meta_path,
    const SimilarityThreshold& coverage_floor) const {
    auto file = cached_factor_file(meta_path);
    file.replace_extension(".seci-" +
                           std::to_string(coverage_floor.numerator) + "-" +
                           std::to_string(coverage_floor.denominator));
    return file;
}

const std::vector<VertexType>& UniversalPathIndex::vertex_types() const noexcept {
    return vertex_types_;
}

const std::vector<UniversalPathEntry>& UniversalPathIndex::entries() const noexcept {
    return entries_;
}

const UniversalPathIndexStats& UniversalPathIndex::stats() const noexcept {
    return stats_;
}

const std::vector<SimilarityThreshold>&
UniversalPathIndex::selective_coverage_floors() const noexcept {
    return selective_coverage_floors_;
}

const std::filesystem::path& UniversalPathIndex::directory() const noexcept {
    return directory_;
}

void UniversalPathIndex::save_manifest() const {
    const auto manifest = directory_ / kManifestName;
    std::ofstream output(manifest, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create UPI manifest: " + manifest.string());
    }
    output << "UPI 6\n"
           << "max_half_length " << stats_.max_half_length << '\n'
           << "path_policy non_backtracking\n"
           << "total_file_bytes " << stats_.total_file_bytes << '\n'
           << "total_half_path_incidences "
           << stats_.total_half_path_incidences << '\n'
           << "build_milliseconds " << stats_.build_milliseconds << '\n'
           << "equivalence_entries " << stats_.equivalence_entries << '\n'
           << "equivalence_file_bytes " << stats_.equivalence_file_bytes << '\n'
           << "certificate_entries " << stats_.certificate_entries << '\n'
           << "certificate_file_bytes " << stats_.certificate_file_bytes << '\n'
           << "certificate_budget_percent "
           << stats_.certificate_budget_percent << '\n'
           << "selective_entries " << stats_.selective_entries << '\n'
           << "selective_file_bytes " << stats_.selective_file_bytes << '\n'
           << "selective_coverage_numerator "
           << stats_.selective_coverage_numerator << '\n'
           << "selective_coverage_denominator "
           << stats_.selective_coverage_denominator << '\n'
           << "selective_coverage_count "
           << selective_coverage_floors_.size() << '\n';
    for (const auto& floor : selective_coverage_floors_) {
        output << "selective_coverage " << floor.numerator << ' '
               << floor.denominator << '\n';
    }
    output << "base_relation_file "
           << std::quoted(base_relation_relative_file_.generic_string()) << '\n'
           << "base_relation_file_bytes "
           << stats_.base_relation_file_bytes << '\n'
           << "precomputed_path_limit "
           << stats_.precomputed_path_limit << '\n';
    output << "type_count " << vertex_types_.size() << '\n';
    for (std::size_t id = 0; id < vertex_types_.size(); ++id) {
        output << "type " << id << ' ' << std::quoted(vertex_types_[id].name)
               << ' ' << vertex_types_[id].count << '\n';
    }
    output << "entry_count " << entries_.size() << '\n';
    for (const auto& entry : entries_) {
        output << "entry " << path_key(entry.half_path) << ' '
               << std::quoted(entry.relative_file.generic_string()) << ' '
               << entry.file_bytes << ' ' << entry.half_path_incidences << ' '
               << entry.projected_edges << ' ' << entry.build_milliseconds << '\n';
    }
    if (!output) {
        throw std::runtime_error("failed to write UPI manifest: " +
                                 manifest.string());
    }
}

std::uint32_t UniversalPathIndex::type_id(const std::string& token) const {
    for (std::uint32_t i = 0; i < vertex_types_.size(); ++i) {
        if (vertex_types_[i].name == token) {
            return i;
        }
    }
    std::size_t consumed = 0;
    try {
        const auto numeric = std::stoul(token, &consumed);
        if (consumed == token.size() && numeric < vertex_types_.size()) {
            return static_cast<std::uint32_t>(numeric);
        }
    } catch (const std::exception&) {
    }
    throw std::invalid_argument("unknown vertex type in meta-path: " + token);
}

}  // namespace hinscan
