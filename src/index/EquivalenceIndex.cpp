#include "index/EquivalenceIndex.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

namespace hinscan {
namespace {

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
    return Signature{first, second, neighborhood.size()};
}

std::uint64_t intersection_size(const std::vector<VertexId>& left,
                                const std::vector<VertexId>& right) {
    std::uint64_t common = 0;
    auto l = left.begin();
    auto r = right.begin();
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
    return common;
}

template <typename T>
void write_value(std::ofstream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if (!output) {
        throw std::runtime_error("failed to write equivalence index");
    }
}

template <typename T>
T read_value(std::ifstream& input, const char* field) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!input) {
        throw std::runtime_error(std::string("truncated equivalence index at ") +
                                 field);
    }
    return value;
}

class DisjointSet {
public:
    explicit DisjointSet(std::size_t size) : parent_(size), rank_(size, 0) {
        std::iota(parent_.begin(), parent_.end(), 0);
    }

    VertexId find(VertexId value) {
        if (parent_[value] != value) {
            parent_[value] = find(parent_[value]);
        }
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

}  // namespace

EquivalenceIndex EquivalenceIndex::build(const FactorIndex& factor) {
    const auto begin = std::chrono::steady_clock::now();
    EquivalenceIndex result;
    result.stats_.vertices = factor.vertex_count();
    result.stats_.projected_edges = factor.stats().exact_projected_edges;

    std::vector<std::vector<VertexId>> representatives;
    std::vector<VertexId> class_of(static_cast<std::size_t>(factor.vertex_count()));
    std::unordered_map<Signature, std::vector<VertexId>, SignatureHash> buckets;

    for (std::uint64_t vertex64 = 0; vertex64 < factor.vertex_count(); ++vertex64) {
        const auto vertex = static_cast<VertexId>(vertex64);
        auto neighborhood = factor.collect_closed_neighborhood(vertex);
        const auto key = signature(neighborhood);
        auto& candidates = buckets[key];
        VertexId class_id = std::numeric_limits<VertexId>::max();
        for (const auto candidate : candidates) {
            if (representatives[candidate] == neighborhood) {
                class_id = candidate;
                break;
            }
        }
        if (class_id == std::numeric_limits<VertexId>::max()) {
            class_id = static_cast<VertexId>(representatives.size());
            representatives.push_back(std::move(neighborhood));
            candidates.push_back(class_id);
            result.members_.emplace_back();
        }
        class_of[vertex] = class_id;
        result.members_[class_id].push_back(vertex);
    }

    result.stats_.classes = result.members_.size();
    result.degrees_.resize(result.members_.size());
    result.adjacency_.resize(result.members_.size());
    for (VertexId class_id = 0; class_id < result.members_.size(); ++class_id) {
        result.degrees_[class_id] = representatives[class_id].size();
        std::vector<VertexId> neighbor_classes;
        neighbor_classes.reserve(representatives[class_id].size());
        for (const auto vertex : representatives[class_id]) {
            neighbor_classes.push_back(class_of[vertex]);
        }
        std::sort(neighbor_classes.begin(), neighbor_classes.end());
        neighbor_classes.erase(
            std::unique(neighbor_classes.begin(), neighbor_classes.end()),
            neighbor_classes.end());
        for (const auto other : neighbor_classes) {
            if (other <= class_id) { continue; }
            const auto common = intersection_size(representatives[class_id],
                                                  representatives[other]);
            result.adjacency_[class_id].push_back({other, common});
            result.adjacency_[other].push_back({class_id, common});
            ++result.stats_.quotient_edges;
        }
    }
    for (auto& neighbors : result.adjacency_) {
        std::sort(neighbors.begin(), neighbors.end(),
                  [](const EquivalenceEdge& left,
                     const EquivalenceEdge& right) {
                      return left.neighbor_class < right.neighbor_class;
                  });
    }
    const auto end = std::chrono::steady_clock::now();
    result.stats_.build_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
    return result;
}

void EquivalenceIndex::save(const std::filesystem::path& file) const {
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path());
    }
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create equivalence index: " +
                                 file.string());
    }
    constexpr char magic[8] = {'E', 'Q', 'I', '0', 'I', 'D', 'X', '\0'};
    output.write(magic, sizeof(magic));
    write_value(output, static_cast<std::uint32_t>(1));
    write_value(output, static_cast<std::uint32_t>(0x01020304U));
    write_value(output, stats_.vertices);
    write_value(output, stats_.classes);
    write_value(output, stats_.projected_edges);
    write_value(output, stats_.quotient_edges);
    write_value(output, stats_.build_milliseconds);
    for (std::size_t class_id = 0; class_id < members_.size(); ++class_id) {
        write_value(output, degrees_[class_id]);
        write_value(output, static_cast<std::uint64_t>(members_[class_id].size()));
        for (const auto vertex : members_[class_id]) { write_value(output, vertex); }
    }
    for (const auto& neighbors : adjacency_) {
        write_value(output, static_cast<std::uint64_t>(neighbors.size()));
        for (const auto& edge : neighbors) {
            write_value(output, edge.neighbor_class);
            write_value(output, edge.common_neighbors);
        }
    }
}

EquivalenceIndex EquivalenceIndex::load(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open equivalence index: " + file.string());
    }
    constexpr char expected[8] = {'E', 'Q', 'I', '0', 'I', 'D', 'X', '\0'};
    char magic[8]{};
    input.read(magic, sizeof(magic));
    const auto version = read_value<std::uint32_t>(input, "version");
    const auto endian = read_value<std::uint32_t>(input, "endian marker");
    if (std::memcmp(magic, expected, sizeof(magic)) != 0 || version != 1 ||
        endian != 0x01020304U) {
        throw std::runtime_error("unsupported equivalence index format");
    }
    EquivalenceIndex result;
    result.stats_.vertices = read_value<std::uint64_t>(input, "vertices");
    result.stats_.classes = read_value<std::uint64_t>(input, "classes");
    result.stats_.projected_edges = read_value<std::uint64_t>(input, "edges");
    result.stats_.quotient_edges = read_value<std::uint64_t>(input, "quotient edges");
    result.stats_.build_milliseconds = read_value<std::uint64_t>(input, "build time");
    if (result.stats_.vertices > std::numeric_limits<VertexId>::max() ||
        result.stats_.classes > std::numeric_limits<VertexId>::max()) {
        throw std::runtime_error("equivalence index exceeds 32-bit ids");
    }
    result.members_.resize(static_cast<std::size_t>(result.stats_.classes));
    result.degrees_.resize(result.members_.size());
    std::uint64_t member_total = 0;
    for (std::size_t class_id = 0; class_id < result.members_.size(); ++class_id) {
        result.degrees_[class_id] = read_value<std::uint64_t>(input, "degree");
        const auto count = read_value<std::uint64_t>(input, "member count");
        if (count > result.stats_.vertices - member_total) {
            throw std::runtime_error("invalid equivalence class member count");
        }
        auto& members = result.members_[class_id];
        members.resize(static_cast<std::size_t>(count));
        for (auto& vertex : members) {
            vertex = read_value<VertexId>(input, "member");
            if (vertex >= result.stats_.vertices) {
                throw std::runtime_error("equivalence member is out of range");
            }
        }
        member_total += count;
    }
    if (member_total != result.stats_.vertices) {
        throw std::runtime_error("equivalence classes do not cover all vertices");
    }
    result.adjacency_.resize(result.members_.size());
    std::uint64_t directed_edges = 0;
    for (auto& neighbors : result.adjacency_) {
        const auto count = read_value<std::uint64_t>(input, "adjacency count");
        neighbors.resize(static_cast<std::size_t>(count));
        for (auto& edge : neighbors) {
            edge.neighbor_class = read_value<VertexId>(input, "neighbor class");
            edge.common_neighbors = read_value<std::uint64_t>(input, "common count");
            if (edge.neighbor_class >= result.stats_.classes) {
                throw std::runtime_error("equivalence neighbor is out of range");
            }
        }
        directed_edges += count;
    }
    if (directed_edges != 2 * result.stats_.quotient_edges) {
        throw std::runtime_error("equivalence edge count is inconsistent");
    }
    return result;
}

const std::vector<VertexId>& EquivalenceIndex::members(VertexId class_id) const {
    return members_.at(class_id);
}

std::uint64_t EquivalenceIndex::degree(VertexId class_id) const {
    return degrees_.at(class_id);
}

const std::vector<EquivalenceEdge>& EquivalenceIndex::neighbors(
    VertexId class_id) const {
    return adjacency_.at(class_id);
}

const EquivalenceIndexStats& EquivalenceIndex::stats() const noexcept {
    return stats_;
}

EquivalenceQueryResult run_pscan_on_equivalence_index(
    const EquivalenceIndex& index,
    const SimilarityThreshold& threshold,
    std::uint64_t mu) {
    if (mu == 0) { throw std::invalid_argument("mu must be positive"); }
    const auto begin = std::chrono::steady_clock::now();
    EquivalenceQueryResult output;
    const auto class_count = static_cast<std::size_t>(index.stats().classes);
    std::vector<std::uint64_t> similar_degree(class_count, 0);
    std::vector<std::vector<VertexId>> similar_neighbors(class_count);

    for (VertexId class_id = 0; class_id < class_count; ++class_id) {
        const auto own_size = index.members(class_id).size();
        similar_degree[class_id] = own_size == 0 ? 0 : own_size - 1;
    }
    for (VertexId left = 0; left < class_count; ++left) {
        const auto own_size = index.members(left).size();
        for (const auto& edge : index.neighbors(left)) {
            if (edge.neighbor_class <= left) { continue; }
            ++output.quotient_edges_checked;
            const auto required = threshold.required_common_neighbors(
                index.degree(left), index.degree(edge.neighbor_class));
            if (edge.common_neighbors < required) { continue; }
            ++output.similar_quotient_edges;
            similar_neighbors[left].push_back(edge.neighbor_class);
            similar_neighbors[edge.neighbor_class].push_back(left);
            similar_degree[left] += index.members(edge.neighbor_class).size();
            similar_degree[edge.neighbor_class] += own_size;
        }
    }

    std::vector<bool> core_class(class_count, false);
    DisjointSet components(class_count);
    for (std::size_t class_id = 0; class_id < class_count; ++class_id) {
        core_class[class_id] = similar_degree[class_id] >= mu;
    }
    for (VertexId left = 0; left < class_count; ++left) {
        if (!core_class[left]) { continue; }
        for (const auto right : similar_neighbors[left]) {
            if (right > left && core_class[right]) {
                components.unite(left, right);
            }
        }
    }

    std::vector<VertexId> component_minimum(
        class_count, std::numeric_limits<VertexId>::max());
    for (VertexId class_id = 0; class_id < class_count; ++class_id) {
        if (!core_class[class_id]) { continue; }
        const auto root = components.find(class_id);
        component_minimum[root] = std::min(component_minimum[root],
                                           index.members(class_id).front());
    }

    auto& result = output.clustering;
    const auto vertex_count = static_cast<std::size_t>(index.stats().vertices);
    result.is_core.assign(vertex_count, false);
    result.core_cluster.assign(vertex_count,
                               std::numeric_limits<VertexId>::max());
    result.noncore_clusters.resize(vertex_count);
    result.roles.assign(vertex_count, PscanVertexRole::Outlier);

    for (VertexId class_id = 0; class_id < class_count; ++class_id) {
        if (!core_class[class_id]) { continue; }
        const auto cluster = component_minimum[components.find(class_id)];
        for (const auto vertex : index.members(class_id)) {
            result.is_core[vertex] = true;
            result.core_cluster[vertex] = cluster;
            result.roles[vertex] = PscanVertexRole::Core;
            ++result.stats.core_vertices;
        }
    }
    for (VertexId class_id = 0; class_id < class_count; ++class_id) {
        if (core_class[class_id]) {
            const auto root = components.find(class_id);
            if (component_minimum[root] == index.members(class_id).front()) {
                ++result.stats.clusters;
            }
            continue;
        }
        std::vector<VertexId> memberships;
        for (const auto neighbor : similar_neighbors[class_id]) {
            if (core_class[neighbor]) {
                memberships.push_back(
                    component_minimum[components.find(neighbor)]);
            }
        }
        std::sort(memberships.begin(), memberships.end());
        memberships.erase(std::unique(memberships.begin(), memberships.end()),
                          memberships.end());
        for (const auto vertex : index.members(class_id)) {
            result.noncore_clusters[vertex] = memberships;
            if (memberships.empty()) {
                result.roles[vertex] = PscanVertexRole::Outlier;
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
    const auto end = std::chrono::steady_clock::now();
    result.stats.query_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
    result.stats.projected_edges_seen_in_prune = index.stats().projected_edges;
    result.stats.exact_similarity_checks = output.quotient_edges_checked;
    return output;
}

}  // namespace hinscan
