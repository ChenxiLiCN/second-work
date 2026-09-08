#include "index/BudgetedEquivalenceIndex.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace hinscan {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::array<char, 8> kMagic{{'B', 'E', 'Q', 'I', '0', '1', '\n', 0}};
constexpr std::uint32_t kVersion = 1;

struct Signature {
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::uint64_t size = 0;
    bool operator==(const Signature& other) const noexcept {
        return first == other.first && second == other.second &&
               size == other.size;
    }
};

struct SignatureHash {
    std::size_t operator()(const Signature& value) const noexcept {
        return static_cast<std::size_t>(
            value.first ^ (value.second + 0x9e3779b97f4a7c15ULL +
                           (value.first << 6U) + (value.first >> 2U)) ^
            value.size);
    }
};

Signature signature(const std::vector<VertexId>& neighborhood) {
    std::uint64_t first = 1469598103934665603ULL;
    std::uint64_t second = 0x9e3779b97f4a7c15ULL;
    for (const auto vertex : neighborhood) {
        first ^= static_cast<std::uint64_t>(vertex) + 1;
        first *= 1099511628211ULL;
        second ^= static_cast<std::uint64_t>(vertex) +
                  0x9e3779b97f4a7c15ULL + (second << 6U) + (second >> 2U);
    }
    return {first, second, neighborhood.size()};
}

std::uint64_t edge_key(VertexId left, VertexId right) noexcept {
    if (left > right) { std::swap(left, right); }
    return (static_cast<std::uint64_t>(left) << 32U) | right;
}

std::uint32_t intersection_size(const std::vector<VertexId>& left,
                                const std::vector<VertexId>& right) {
    auto l = left.begin();
    auto r = right.begin();
    std::uint64_t common = 0;
    while (l != left.end() && r != right.end()) {
        if (*l < *r) {
            ++l;
        } else if (*r < *l) {
            ++r;
        } else {
            ++common;
            ++l;
            ++r;
        }
    }
    if (common > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("BEQI common-neighbor count exceeds 32 bits");
    }
    return static_cast<std::uint32_t>(common);
}

template <typename T>
void write_value(std::ostream& output, T value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if (!output) { throw std::runtime_error("failed to write BEQI"); }
}

template <typename T>
T read_value(std::istream& input, const char* field) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) {
        throw std::runtime_error(std::string("truncated BEQI at ") + field);
    }
    return value;
}

template <typename T>
void write_vector(std::ostream& output, const std::vector<T>& values) {
    output.write(reinterpret_cast<const char*>(values.data()),
                 static_cast<std::streamsize>(values.size() * sizeof(T)));
    if (!output) { throw std::runtime_error("failed to write BEQI payload"); }
}

template <typename T>
void read_vector(std::istream& input, std::vector<T>* values,
                 const char* field) {
    input.read(reinterpret_cast<char*>(values->data()),
               static_cast<std::streamsize>(values->size() * sizeof(T)));
    if (!input) {
        throw std::runtime_error(std::string("truncated BEQI at ") + field);
    }
}

struct Candidate {
    std::uint64_t score = 0;
    VertexId left = 0;
    VertexId right = 0;
};

struct CandidateGreater {
    bool operator()(const Candidate& left, const Candidate& right) const {
        if (left.score != right.score) { return left.score > right.score; }
        return edge_key(left.left, left.right) > edge_key(right.left, right.right);
    }
};

class DisjointSet {
public:
    explicit DisjointSet(std::size_t size) : parent_(size), rank_(size, 0) {
        std::iota(parent_.begin(), parent_.end(), 0);
    }
    VertexId find(VertexId value) {
        if (parent_[value] != value) { parent_[value] = find(parent_[value]); }
        return parent_[value];
    }
    void unite(VertexId left, VertexId right) {
        left = find(left);
        right = find(right);
        if (left == right) { return; }
        if (rank_[left] < rank_[right]) { std::swap(left, right); }
        parent_[right] = left;
        if (rank_[left] == rank_[right]) { ++rank_[left]; }
    }
private:
    std::vector<VertexId> parent_;
    std::vector<std::uint8_t> rank_;
};

enum class EdgeState : std::int8_t { Dissimilar = -1, Unknown = 0, Similar = 1 };

}  // namespace

BudgetedEquivalenceIndex BudgetedEquivalenceIndex::build(
    const FactorIndex& factor, std::uint64_t maximum_count_entries) {
    const auto begin = Clock::now();
    BudgetedEquivalenceIndex result;
    result.stats_.vertices = factor.vertex_count();
    result.stats_.projected_edges = factor.stats().exact_projected_edges;

    std::vector<std::vector<VertexId>> representative_neighborhoods;
    std::vector<std::vector<VertexId>> class_members;
    std::vector<VertexId> class_of(static_cast<std::size_t>(factor.vertex_count()));
    std::unordered_map<Signature, std::vector<VertexId>, SignatureHash> buckets;
    buckets.reserve(static_cast<std::size_t>(factor.vertex_count() / 2));
    for (std::uint64_t vertex64 = 0; vertex64 < factor.vertex_count(); ++vertex64) {
        const auto vertex = static_cast<VertexId>(vertex64);
        auto neighborhood = factor.collect_closed_neighborhood(vertex);
        auto& candidates = buckets[signature(neighborhood)];
        VertexId class_id = std::numeric_limits<VertexId>::max();
        for (const auto candidate : candidates) {
            if (representative_neighborhoods[candidate] == neighborhood) {
                class_id = candidate;
                break;
            }
        }
        if (class_id == std::numeric_limits<VertexId>::max()) {
            class_id = static_cast<VertexId>(representative_neighborhoods.size());
            representative_neighborhoods.push_back(std::move(neighborhood));
            candidates.push_back(class_id);
            class_members.emplace_back();
            result.representatives_.push_back(vertex);
        }
        class_of[vertex] = class_id;
        class_members[class_id].push_back(vertex);
    }

    result.stats_.classes = representative_neighborhoods.size();
    result.degrees_.reserve(representative_neighborhoods.size());
    result.member_offsets_.push_back(0);
    for (std::size_t class_id = 0;
         class_id < representative_neighborhoods.size(); ++class_id) {
        if (representative_neighborhoods[class_id].size() >
            std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("BEQI class degree exceeds 32 bits");
        }
        result.degrees_.push_back(static_cast<std::uint32_t>(
            representative_neighborhoods[class_id].size()));
        result.members_flat_.insert(result.members_flat_.end(),
                                    class_members[class_id].begin(),
                                    class_members[class_id].end());
        result.member_offsets_.push_back(result.members_flat_.size());
    }

    std::vector<std::vector<VertexId>> adjacency(result.stats_.classes);
    for (VertexId class_id = 0; class_id < result.stats_.classes; ++class_id) {
        std::vector<VertexId> neighbor_classes;
        neighbor_classes.reserve(representative_neighborhoods[class_id].size());
        for (const auto vertex : representative_neighborhoods[class_id]) {
            neighbor_classes.push_back(class_of[vertex]);
        }
        std::sort(neighbor_classes.begin(), neighbor_classes.end());
        neighbor_classes.erase(
            std::unique(neighbor_classes.begin(), neighbor_classes.end()),
            neighbor_classes.end());
        for (const auto other : neighbor_classes) {
            if (other <= class_id) { continue; }
            adjacency[class_id].push_back(other);
            adjacency[other].push_back(class_id);
            ++result.stats_.quotient_edges;
        }
    }

    result.adjacency_offsets_.push_back(0);
    for (auto& neighbors : adjacency) {
        std::sort(neighbors.begin(), neighbors.end());
        result.neighbors_flat_.insert(result.neighbors_flat_.end(),
                                      neighbors.begin(), neighbors.end());
        result.adjacency_offsets_.push_back(result.neighbors_flat_.size());
    }

    std::priority_queue<Candidate, std::vector<Candidate>, CandidateGreater>
        selected;
    for (VertexId left = 0; left < result.stats_.classes; ++left) {
        for (const auto right : adjacency[left]) {
            if (right <= left) { continue; }
            const auto smaller_degree =
                std::min(result.degrees_[left], result.degrees_[right]);
            const auto larger_degree =
                std::max(result.degrees_[left], result.degrees_[right]);
            const auto degree_closeness =
                (static_cast<std::uint64_t>(smaller_degree) + 1) *
                (std::uint64_t{1} << 20U) /
                (static_cast<std::uint64_t>(larger_degree) + 1);
            const auto intersection_work =
                static_cast<std::uint64_t>(result.degrees_[left]) +
                result.degrees_[right];
            const auto score = intersection_work >
                    std::numeric_limits<std::uint64_t>::max() /
                        std::max<std::uint64_t>(degree_closeness, 1)
                ? std::numeric_limits<std::uint64_t>::max()
                : intersection_work *
                      std::max<std::uint64_t>(degree_closeness, 1);
            const Candidate candidate{score, left, right};
            if (selected.size() < maximum_count_entries) {
                selected.push(candidate);
            } else if (maximum_count_entries != 0 &&
                       candidate.score > selected.top().score) {
                selected.pop();
                selected.push(candidate);
            }
        }
    }
    result.counts_.reserve(selected.size());
    while (!selected.empty()) {
        const auto candidate = selected.top();
        selected.pop();
        result.counts_.push_back({
            edge_key(candidate.left, candidate.right),
            intersection_size(representative_neighborhoods[candidate.left],
                              representative_neighborhoods[candidate.right])});
    }
    std::sort(result.counts_.begin(), result.counts_.end(),
              [](const auto& left, const auto& right) {
                  return left.edge_key < right.edge_key;
              });
    result.stats_.retained_counts = result.counts_.size();
    result.stats_.build_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin)
            .count());
    return result;
}

void BudgetedEquivalenceIndex::save(
    const std::filesystem::path& index_file) const {
    if (!index_file.parent_path().empty()) {
        std::filesystem::create_directories(index_file.parent_path());
    }
    std::ofstream output(index_file, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create BEQI: " + index_file.string());
    }
    output.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    write_value(output, kVersion);
    write_value(output, stats_.vertices);
    write_value(output, stats_.classes);
    write_value(output, stats_.projected_edges);
    write_value(output, stats_.quotient_edges);
    write_value(output, stats_.retained_counts);
    write_value(output, stats_.build_milliseconds);
    write_vector(output, degrees_);
    write_vector(output, representatives_);
    write_vector(output, member_offsets_);
    write_vector(output, members_flat_);
    write_vector(output, adjacency_offsets_);
    write_vector(output, neighbors_flat_);
    for (const auto& entry : counts_) {
        write_value(output, entry.edge_key);
        write_value(output, entry.common_neighbors);
    }
}

BudgetedEquivalenceIndex BudgetedEquivalenceIndex::load(
    const std::filesystem::path& index_file) {
    std::ifstream input(index_file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open BEQI: " + index_file.string());
    }
    std::array<char, 8> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != kMagic ||
        read_value<std::uint32_t>(input, "version") != kVersion) {
        throw std::runtime_error("unsupported BEQI format");
    }
    BudgetedEquivalenceIndex result;
    result.stats_.vertices = read_value<std::uint64_t>(input, "vertices");
    result.stats_.classes = read_value<std::uint64_t>(input, "classes");
    result.stats_.projected_edges =
        read_value<std::uint64_t>(input, "projected edges");
    result.stats_.quotient_edges =
        read_value<std::uint64_t>(input, "quotient edges");
    result.stats_.retained_counts =
        read_value<std::uint64_t>(input, "retained counts");
    result.stats_.build_milliseconds =
        read_value<std::uint64_t>(input, "build time");
    if (result.stats_.classes > std::numeric_limits<std::size_t>::max() ||
        result.stats_.vertices > std::numeric_limits<std::size_t>::max() ||
        result.stats_.quotient_edges >
            std::numeric_limits<std::size_t>::max() / 2 ||
        result.stats_.retained_counts >
            std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error("BEQI exceeds this machine");
    }
    const auto classes = static_cast<std::size_t>(result.stats_.classes);
    result.degrees_.resize(classes);
    result.representatives_.resize(classes);
    result.member_offsets_.resize(classes + 1);
    result.members_flat_.resize(static_cast<std::size_t>(result.stats_.vertices));
    result.adjacency_offsets_.resize(classes + 1);
    result.neighbors_flat_.resize(
        static_cast<std::size_t>(result.stats_.quotient_edges * 2));
    result.counts_.resize(
        static_cast<std::size_t>(result.stats_.retained_counts));
    read_vector(input, &result.degrees_, "degrees");
    read_vector(input, &result.representatives_, "representatives");
    read_vector(input, &result.member_offsets_, "member offsets");
    read_vector(input, &result.members_flat_, "members");
    read_vector(input, &result.adjacency_offsets_, "adjacency offsets");
    read_vector(input, &result.neighbors_flat_, "neighbors");
    for (auto& entry : result.counts_) {
        entry.edge_key = read_value<std::uint64_t>(input, "count edge");
        entry.common_neighbors = read_value<std::uint32_t>(input, "count value");
    }
    if (result.member_offsets_.front() != 0 ||
        result.member_offsets_.back() != result.stats_.vertices ||
        !std::is_sorted(result.member_offsets_.begin(),
                        result.member_offsets_.end()) ||
        result.adjacency_offsets_.front() != 0 ||
        result.adjacency_offsets_.back() != result.neighbors_flat_.size() ||
        !std::is_sorted(result.adjacency_offsets_.begin(),
                        result.adjacency_offsets_.end()) ||
        !std::is_sorted(result.counts_.begin(), result.counts_.end(),
                        [](const auto& left, const auto& right) {
                            return left.edge_key < right.edge_key;
                        })) {
        throw std::runtime_error("invalid BEQI layout");
    }
    return result;
}

VertexRange BudgetedEquivalenceIndex::members(VertexId class_id) const {
    if (class_id >= stats_.classes) { throw std::out_of_range("BEQI class"); }
    return {members_flat_.data() + member_offsets_[class_id],
            members_flat_.data() + member_offsets_[class_id + 1]};
}

VertexRange BudgetedEquivalenceIndex::neighbors(VertexId class_id) const {
    if (class_id >= stats_.classes) { throw std::out_of_range("BEQI class"); }
    return {neighbors_flat_.data() + adjacency_offsets_[class_id],
            neighbors_flat_.data() + adjacency_offsets_[class_id + 1]};
}

VertexId BudgetedEquivalenceIndex::representative(VertexId class_id) const {
    return representatives_.at(class_id);
}

std::uint32_t BudgetedEquivalenceIndex::degree(VertexId class_id) const {
    return degrees_.at(class_id);
}

bool BudgetedEquivalenceIndex::find_count(
    VertexId left_class, VertexId right_class,
    std::uint32_t* common_neighbors) const {
    const auto key = edge_key(left_class, right_class);
    const auto iterator = std::lower_bound(
        counts_.begin(), counts_.end(), key,
        [](const BudgetedSimilarityEntry& entry, std::uint64_t value) {
            return entry.edge_key < value;
        });
    if (iterator == counts_.end() || iterator->edge_key != key) { return false; }
    *common_neighbors = iterator->common_neighbors;
    return true;
}

std::uint64_t BudgetedEquivalenceIndex::estimated_bytes() const noexcept {
    return degrees_.size() * sizeof(std::uint32_t) +
           representatives_.size() * sizeof(VertexId) +
           member_offsets_.size() * sizeof(std::uint64_t) +
           members_flat_.size() * sizeof(VertexId) +
           adjacency_offsets_.size() * sizeof(std::uint64_t) +
           neighbors_flat_.size() * sizeof(VertexId) +
           counts_.size() * (sizeof(std::uint64_t) + sizeof(std::uint32_t));
}

const BudgetedEquivalenceStats& BudgetedEquivalenceIndex::stats() const noexcept {
    return stats_;
}

BudgetedEquivalenceQueryResult run_pscan_on_budgeted_equivalence(
    const BudgetedEquivalenceIndex& index, const FactorIndex& factor,
    const SimilarityThreshold& threshold, std::uint64_t mu) {
    if (mu == 0) { throw std::invalid_argument("mu must be positive"); }
    if (factor.vertex_count() != index.stats().vertices) {
        throw std::invalid_argument("BEQI and FLI vertex counts disagree");
    }
    const auto begin = Clock::now();
    BudgetedEquivalenceQueryResult output;
    const auto classes = static_cast<std::size_t>(index.stats().classes);
    std::vector<std::int64_t> similar_degree(classes, 0);
    std::vector<std::int64_t> effective_degree(classes, 0);
    std::vector<std::uint8_t> processed(classes, 0);
    std::vector<std::vector<VertexId>> cached_neighborhoods(classes);
    std::vector<std::uint8_t> neighborhood_ready(classes, 0);
    std::unordered_map<std::uint64_t, EdgeState> states;
    states.reserve(classes * 4);
    DisjointSet components(classes);

    auto class_size = [&](VertexId value) {
        return static_cast<std::int64_t>(index.members(value).size());
    };
    auto default_state = [&](VertexId left, VertexId right) {
        if (threshold.fails_degree_ratio(index.degree(left), index.degree(right))) {
            return EdgeState::Dissimilar;
        }
        if (threshold.required_common_neighbors(index.degree(left),
                                                index.degree(right)) <= 2) {
            return EdgeState::Similar;
        }
        return EdgeState::Unknown;
    };
    auto state = [&](VertexId left, VertexId right) {
        const auto iterator = states.find(edge_key(left, right));
        return iterator == states.end() ? default_state(left, right)
                                        : iterator->second;
    };
    auto neighborhood = [&](VertexId class_id) -> const std::vector<VertexId>& {
        if (neighborhood_ready[class_id] == 0) {
            cached_neighborhoods[class_id] = factor.collect_closed_neighborhood(
                index.representative(class_id));
            neighborhood_ready[class_id] = 1;
        }
        return cached_neighborhoods[class_id];
    };
    auto exact_check = [&](VertexId left, VertexId right) {
        const auto key = edge_key(left, right);
        const auto existing = states.find(key);
        if (existing != states.end()) { return existing->second; }
        std::uint32_t common = 0;
        ++output.exact_class_checks;
        if (index.find_count(left, right, &common)) {
            ++output.indexed_count_hits;
        } else {
            ++output.fli_fallback_checks;
            common = intersection_size(neighborhood(left), neighborhood(right));
        }
        const auto required = threshold.required_common_neighbors(
            index.degree(left), index.degree(right));
        const auto result = common >= required ? EdgeState::Similar
                                               : EdgeState::Dissimilar;
        states.emplace(key, result);
        return result;
    };

    std::vector<VertexId> core_stack;
    struct QueueEntry {
        std::int64_t degree = 0;
        VertexId vertex = 0;
        bool operator<(const QueueEntry& other) const {
            return degree != other.degree ? degree < other.degree
                                          : vertex < other.vertex;
        }
    };
    std::priority_queue<QueueEntry> queue;
    auto push_effective = [&](VertexId value) {
        if (!processed[value] &&
            effective_degree[value] >= static_cast<std::int64_t>(mu)) {
            queue.push({effective_degree[value], value});
        }
    };

    for (VertexId class_id = 0; class_id < classes; ++class_id) {
        similar_degree[class_id] = class_size(class_id) - 1;
        effective_degree[class_id] = similar_degree[class_id];
    }
    for (VertexId left = 0; left < classes; ++left) {
        for (const auto right : index.neighbors(left)) {
            if (right <= left) { continue; }
            ++output.class_edges_considered;
            const auto initial = default_state(left, right);
            if (initial == EdgeState::Dissimilar) { continue; }
            effective_degree[left] += class_size(right);
            effective_degree[right] += class_size(left);
            if (initial == EdgeState::Similar) {
                similar_degree[left] += class_size(right);
                similar_degree[right] += class_size(left);
                states.emplace(edge_key(left, right), initial);
            }
        }
    }
    for (VertexId class_id = 0; class_id < classes; ++class_id) {
        if (similar_degree[class_id] >= static_cast<std::int64_t>(mu)) {
            core_stack.push_back(class_id);
        }
        push_effective(class_id);
    }

    VertexId current = 0;
    auto next_class = [&]() {
        while (!core_stack.empty()) {
            const auto candidate = core_stack.back();
            core_stack.pop_back();
            if (!processed[candidate]) { current = candidate; return true; }
        }
        while (!queue.empty()) {
            const auto candidate = queue.top();
            queue.pop();
            if (!processed[candidate.vertex] &&
                effective_degree[candidate.vertex] == candidate.degree &&
                candidate.degree >= static_cast<std::int64_t>(mu)) {
                current = candidate.vertex;
                return true;
            }
        }
        return false;
    };

    while (next_class()) {
        const auto neighbors = index.neighbors(current);
        for (const auto other : neighbors) {
            if (similar_degree[current] >= static_cast<std::int64_t>(mu) ||
                effective_degree[current] < static_cast<std::int64_t>(mu)) {
                break;
            }
            auto checked = state(current, other);
            if (checked == EdgeState::Dissimilar) { continue; }
            if (checked == EdgeState::Unknown) {
                checked = exact_check(current, other);
                if (checked == EdgeState::Similar) {
                    similar_degree[current] += class_size(other);
                    if (!processed[other]) {
                        const auto was_core =
                            similar_degree[other] >= static_cast<std::int64_t>(mu);
                        similar_degree[other] += class_size(current);
                        if (!was_core && similar_degree[other] >=
                                             static_cast<std::int64_t>(mu)) {
                            core_stack.push_back(other);
                        }
                    }
                } else {
                    effective_degree[current] -= class_size(other);
                    if (!processed[other]) {
                        effective_degree[other] -= class_size(current);
                        push_effective(other);
                    }
                }
            }
        }
        processed[current] = 1;
        if (similar_degree[current] < static_cast<std::int64_t>(mu)) { continue; }
        for (const auto other : neighbors) {
            if (similar_degree[other] < static_cast<std::int64_t>(mu) ||
                components.find(current) == components.find(other)) {
                continue;
            }
            auto checked = state(current, other);
            if (checked == EdgeState::Unknown) {
                checked = exact_check(current, other);
                if (!processed[other]) {
                    if (checked == EdgeState::Similar) {
                        const auto was_core =
                            similar_degree[other] >= static_cast<std::int64_t>(mu);
                        similar_degree[other] += class_size(current);
                        if (!was_core && similar_degree[other] >=
                                             static_cast<std::int64_t>(mu)) {
                            core_stack.push_back(other);
                        }
                    } else {
                        effective_degree[other] -= class_size(current);
                        push_effective(other);
                    }
                }
            }
            if (checked == EdgeState::Similar &&
                similar_degree[other] >= static_cast<std::int64_t>(mu)) {
                components.unite(current, other);
            }
        }
    }

    std::vector<std::uint8_t> core_class(classes, 0);
    std::vector<VertexId> component_minimum(
        classes, std::numeric_limits<VertexId>::max());
    for (VertexId class_id = 0; class_id < classes; ++class_id) {
        if (similar_degree[class_id] < static_cast<std::int64_t>(mu)) { continue; }
        core_class[class_id] = 1;
        const auto root = components.find(class_id);
        component_minimum[root] =
            std::min(component_minimum[root], *index.members(class_id).begin());
    }

    auto& result = output.clustering;
    result.is_core.assign(index.stats().vertices, false);
    result.core_cluster.assign(index.stats().vertices,
                               std::numeric_limits<VertexId>::max());
    result.noncore_clusters.resize(index.stats().vertices);
    result.roles.assign(index.stats().vertices, PscanVertexRole::Outlier);
    for (VertexId class_id = 0; class_id < classes; ++class_id) {
        if (!core_class[class_id]) { continue; }
        const auto cluster = component_minimum[components.find(class_id)];
        if (cluster == *index.members(class_id).begin()) { ++result.stats.clusters; }
        for (const auto vertex : index.members(class_id)) {
            result.is_core[vertex] = true;
            result.core_cluster[vertex] = cluster;
            result.roles[vertex] = PscanVertexRole::Core;
            ++result.stats.core_vertices;
        }
    }
    for (VertexId class_id = 0; class_id < classes; ++class_id) {
        if (core_class[class_id]) { continue; }
        std::vector<VertexId> memberships;
        for (const auto other : index.neighbors(class_id)) {
            if (!core_class[other]) { continue; }
            auto checked = state(class_id, other);
            if (checked == EdgeState::Unknown) {
                checked = exact_check(class_id, other);
            }
            if (checked == EdgeState::Similar) {
                memberships.push_back(component_minimum[components.find(other)]);
            }
        }
        std::sort(memberships.begin(), memberships.end());
        memberships.erase(std::unique(memberships.begin(), memberships.end()),
                          memberships.end());
        for (const auto vertex : index.members(class_id)) {
            result.noncore_clusters[vertex] = memberships;
            if (memberships.empty()) {
                ++result.stats.outlier_vertices;
            } else if (memberships.size() == 1) {
                result.roles[vertex] = PscanVertexRole::Border;
                ++result.stats.border_vertices;
            } else {
                result.roles[vertex] = PscanVertexRole::Hub;
                ++result.stats.hub_vertices;
            }
        }
    }
    result.stats.query_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin)
            .count());
    result.stats.projected_edges_seen_in_prune = index.stats().projected_edges;
    result.stats.exact_similarity_checks = output.exact_class_checks;
    return output;
}

}  // namespace hinscan
