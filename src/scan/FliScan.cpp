#include "scan/FliScan.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace hinscan {
namespace {

class DisjointSet {
public:
    explicit DisjointSet(std::size_t size) : parent_(size), rank_(size, 0) {
        std::iota(parent_.begin(), parent_.end(), 0);
    }

    VertexId find(VertexId vertex) {
        if (parent_[vertex] != vertex) {
            parent_[vertex] = find(parent_[vertex]);
        }
        return parent_[vertex];
    }

    void unite(VertexId left, VertexId right) {
        left = find(left);
        right = find(right);
        if (left == right) {
            return;
        }
        if (rank_[left] < rank_[right]) {
            std::swap(left, right);
        }
        parent_[right] = left;
        if (rank_[left] == rank_[right]) {
            ++rank_[left];
        }
    }

private:
    std::vector<VertexId> parent_;
    std::vector<std::uint8_t> rank_;
};

class SimilarityWorkspace {
public:
    SimilarityWorkspace(const FactorIndex& index,
                        const SimilarityThreshold& threshold)
        : index_(index),
          threshold_(threshold),
          left_marks_(static_cast<std::size_t>(index.vertex_count()), 0),
          seen_(static_cast<std::size_t>(index.vertex_count()), 0),
          candidate_seen_(static_cast<std::size_t>(index.vertex_count()), 0) {}

    std::vector<VertexId> generate_left_neighborhood(VertexId left,
                                                      FliScanStats* stats) {
        advance_epoch(&candidate_epoch_, &candidate_seen_);
        std::vector<VertexId> neighborhood;
        neighborhood.reserve(static_cast<std::size_t>(index_.degree(left)));

        auto append_once = [&](VertexId candidate) {
            if (candidate_seen_[candidate] != candidate_epoch_) {
                candidate_seen_[candidate] = candidate_epoch_;
                neighborhood.push_back(candidate);
            }
        };
        append_once(left);
        for (const auto witness : index_.witnesses(left)) {
            for (const auto candidate : index_.posting(witness)) {
                ++stats->candidate_generation_posting_entries_read;
                append_once(candidate);
            }
        }
        if (neighborhood.size() != index_.degree(left)) {
            throw std::logic_error(
                "timestamp candidate generation disagrees with stored degree");
        }
        return neighborhood;
    }

    void set_left_neighborhood(
        const std::vector<VertexId>& left_neighborhood) {
        advance_epoch(&left_epoch_, &left_marks_);
        for (const auto vertex : left_neighborhood) {
            left_marks_[vertex] = left_epoch_;
        }
    }

    bool check(VertexId left, VertexId right, FliScanStats* stats) {
        const auto left_degree = index_.degree(left);
        const auto right_degree = index_.degree(right);
        if (threshold_.fails_degree_ratio(left_degree, right_degree)) {
            ++stats->degree_pruned_checks;
            return false;
        }
        ++stats->exact_intersection_checks;
        const auto required =
            threshold_.required_common_neighbors(left_degree, right_degree);
        advance_epoch(&seen_epoch_, &seen_);

        std::uint64_t remaining_entries = 1;
        for (const auto witness : index_.witnesses(right)) {
            remaining_entries += index_.posting(witness).size();
        }

        std::uint64_t common = 0;
        auto visit = [&](VertexId candidate, bool posting_entry) {
            --remaining_entries;
            if (posting_entry) {
                ++stats->posting_entries_read;
            }
            if (seen_[candidate] != seen_epoch_) {
                seen_[candidate] = seen_epoch_;
                if (left_marks_[candidate] == left_epoch_) {
                    ++common;
                }
            }
        };

        visit(right, false);
        if (common >= required) {
            ++stats->early_accept_checks;
            return true;
        }

        for (const auto witness : index_.witnesses(right)) {
            for (const auto candidate : index_.posting(witness)) {
                visit(candidate, true);
                if (common >= required) {
                    ++stats->early_accept_checks;
                    return true;
                }
                if (common + remaining_entries < required) {
                    ++stats->early_reject_checks;
                    return false;
                }
            }
        }
        return common >= required;
    }

    std::uint64_t bytes() const noexcept {
        return (left_marks_.size() + seen_.size() + candidate_seen_.size()) *
               sizeof(std::uint32_t);
    }

private:
    static void advance_epoch(std::uint32_t* epoch,
                              std::vector<std::uint32_t>* values) {
        ++(*epoch);
        if (*epoch == 0) {
            std::fill(values->begin(), values->end(), 0);
            *epoch = 1;
        }
    }

    const FactorIndex& index_;
    const SimilarityThreshold& threshold_;
    std::vector<std::uint32_t> left_marks_;
    std::vector<std::uint32_t> seen_;
    std::vector<std::uint32_t> candidate_seen_;
    std::uint32_t left_epoch_ = 0;
    std::uint32_t seen_epoch_ = 0;
    std::uint32_t candidate_epoch_ = 0;
};

}  // namespace

const char* role_name(VertexRole role) noexcept {
    switch (role) {
        case VertexRole::Core:
            return "core";
        case VertexRole::Border:
            return "border";
        case VertexRole::Hub:
            return "hub";
        case VertexRole::Outlier:
            return "outlier";
    }
    return "unknown";
}

FliScanResult run_fli_scan(const FactorIndex& index,
                           const SimilarityThreshold& threshold,
                           std::uint64_t mu) {
    if (mu == 0) {
        throw std::invalid_argument("mu must be positive");
    }
    if (index.vertex_count() > std::numeric_limits<VertexId>::max()) {
        throw std::overflow_error("FLI-0 SCAN uses 32-bit vertex ids");
    }

    const auto begin = std::chrono::steady_clock::now();
    FliScanResult result;
    const auto vertex_count = static_cast<std::size_t>(index.vertex_count());
    std::vector<std::uint64_t> similar_degree(vertex_count, 0);
    std::vector<std::pair<VertexId, VertexId>> positive_certificates;
    SimilarityWorkspace similarity_workspace(index, threshold);
    result.stats.timestamp_workspace_bytes = similarity_workspace.bytes();

    for (std::uint64_t left64 = 0; left64 < index.vertex_count(); ++left64) {
        const auto left = static_cast<VertexId>(left64);
        const auto left_neighborhood =
            similarity_workspace.generate_left_neighborhood(left, &result.stats);
        result.stats.peak_ephemeral_neighborhood_bytes = std::max(
            result.stats.peak_ephemeral_neighborhood_bytes,
            static_cast<std::uint64_t>(left_neighborhood.size() *
                                       sizeof(VertexId)));
        similarity_workspace.set_left_neighborhood(left_neighborhood);

        for (const auto right : left_neighborhood) {
            if (right <= left) {
                continue;
            }
            ++result.stats.pass1_candidate_edges;
            if (similarity_workspace.check(left, right, &result.stats)) {
                ++similar_degree[left];
                ++similar_degree[right];
                ++result.stats.similar_edges_pass1;
                positive_certificates.emplace_back(left, right);
            }
        }
    }
    result.stats.positive_certificate_bytes =
        positive_certificates.capacity() * sizeof(std::pair<VertexId, VertexId>);

    result.is_core.resize(vertex_count, false);
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        result.is_core[vertex] = similar_degree[vertex] >= mu;
        result.stats.core_vertices += result.is_core[vertex] ? 1 : 0;
    }

    DisjointSet components(vertex_count);
    for (const auto& [left, right] : positive_certificates) {
        if (!result.is_core[left] || !result.is_core[right]) {
            continue;
        }
        ++result.stats.positive_core_core_edges;
        components.unite(left, right);
    }

    std::vector<VertexId> root_minimum(
        vertex_count, std::numeric_limits<VertexId>::max());
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        if (!result.is_core[vertex]) {
            continue;
        }
        const auto root = components.find(static_cast<VertexId>(vertex));
        root_minimum[root] =
            std::min(root_minimum[root], static_cast<VertexId>(vertex));
    }

    result.core_cluster.assign(vertex_count,
                               std::numeric_limits<VertexId>::max());
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        if (!result.is_core[vertex]) {
            continue;
        }
        const auto root = components.find(static_cast<VertexId>(vertex));
        result.core_cluster[vertex] = root_minimum[root];
        if (root_minimum[root] == vertex) {
            ++result.stats.clusters;
        }
    }

    result.noncore_clusters.resize(vertex_count);
    for (const auto& [left, right] : positive_certificates) {
        if (result.is_core[left] == result.is_core[right]) {
            continue;
        }
        ++result.stats.positive_core_noncore_edges;
        const auto core = result.is_core[left] ? left : right;
        const auto noncore = result.is_core[left] ? right : left;
        result.noncore_clusters[noncore].push_back(result.core_cluster[core]);
    }

    result.roles.resize(vertex_count, VertexRole::Outlier);
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        if (result.is_core[vertex]) {
            result.roles[vertex] = VertexRole::Core;
            continue;
        }
        auto& clusters = result.noncore_clusters[vertex];
        std::sort(clusters.begin(), clusters.end());
        clusters.erase(std::unique(clusters.begin(), clusters.end()), clusters.end());
        if (clusters.empty()) {
            result.roles[vertex] = VertexRole::Outlier;
            ++result.stats.outlier_vertices;
        } else if (clusters.size() == 1) {
            result.roles[vertex] = VertexRole::Border;
            ++result.stats.border_vertices;
        } else {
            result.roles[vertex] = VertexRole::Hub;
            ++result.stats.hub_vertices;
        }
    }

    const auto end = std::chrono::steady_clock::now();
    result.stats.query_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
    return result;
}

void write_fli_scan_results(const std::filesystem::path& output_directory,
                            const std::string& epsilon_text,
                            std::uint64_t mu,
                            const FliScanResult& result) {
    std::filesystem::create_directories(output_directory);
    const auto suffix = epsilon_text + "-" + std::to_string(mu) + ".txt";
    const auto result_path = output_directory / ("result-" + suffix);
    const auto roles_path = output_directory / ("roles-" + suffix);

    std::ofstream output(result_path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create FLI-SCAN result file");
    }
    output << "c/n vertex_id cluster_id\n";
    for (std::size_t vertex = 0; vertex < result.is_core.size(); ++vertex) {
        if (result.is_core[vertex]) {
            output << "c " << vertex << ' ' << result.core_cluster[vertex] << '\n';
        }
    }

    std::vector<std::pair<VertexId, VertexId>> memberships;
    for (std::size_t vertex = 0; vertex < result.noncore_clusters.size(); ++vertex) {
        for (const auto cluster : result.noncore_clusters[vertex]) {
            memberships.emplace_back(cluster, static_cast<VertexId>(vertex));
        }
    }
    std::sort(memberships.begin(), memberships.end());
    for (const auto& [cluster, vertex] : memberships) {
        output << "n " << vertex << ' ' << cluster << '\n';
    }

    std::ofstream roles(roles_path, std::ios::trunc);
    if (!roles) {
        throw std::runtime_error("cannot create FLI-SCAN roles file");
    }
    roles << "vertex_id role cluster_count clusters\n";
    for (std::size_t vertex = 0; vertex < result.roles.size(); ++vertex) {
        roles << vertex << ' ' << role_name(result.roles[vertex]) << ' ';
        if (result.is_core[vertex]) {
            roles << 1 << ' ' << result.core_cluster[vertex];
        } else {
            roles << result.noncore_clusters[vertex].size();
            for (const auto cluster : result.noncore_clusters[vertex]) {
                roles << ' ' << cluster;
            }
        }
        roles << '\n';
    }
}

}  // namespace hinscan
