#ifndef HINSCAN_FLI_SCAN_H
#define HINSCAN_FLI_SCAN_H

#include "index/FactorIndex.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hinscan {

enum class VertexRole {
    Core,
    Border,
    Hub,
    Outlier,
};

const char* role_name(VertexRole role) noexcept;

struct FliScanStats {
    std::uint64_t pass1_candidate_edges = 0;
    std::uint64_t positive_core_core_edges = 0;
    std::uint64_t positive_core_noncore_edges = 0;
    std::uint64_t degree_pruned_checks = 0;
    std::uint64_t exact_intersection_checks = 0;
    std::uint64_t early_accept_checks = 0;
    std::uint64_t early_reject_checks = 0;
    std::uint64_t candidate_generation_posting_entries_read = 0;
    std::uint64_t posting_entries_read = 0;
    std::uint64_t similar_edges_pass1 = 0;
    std::uint64_t positive_certificate_bytes = 0;
    std::uint64_t peak_ephemeral_neighborhood_bytes = 0;
    std::uint64_t timestamp_workspace_bytes = 0;
    std::uint64_t core_vertices = 0;
    std::uint64_t clusters = 0;
    std::uint64_t border_vertices = 0;
    std::uint64_t hub_vertices = 0;
    std::uint64_t outlier_vertices = 0;
    std::uint64_t query_milliseconds = 0;
};

struct FliScanResult {
    std::vector<bool> is_core;
    std::vector<VertexId> core_cluster;
    std::vector<std::vector<VertexId>> noncore_clusters;
    std::vector<VertexRole> roles;
    FliScanStats stats;
};

FliScanResult run_fli_scan(const FactorIndex& index,
                           const SimilarityThreshold& threshold,
                           std::uint64_t mu);

void write_fli_scan_results(const std::filesystem::path& output_directory,
                            const std::string& epsilon_text,
                            std::uint64_t mu,
                            const FliScanResult& result);

}  // namespace hinscan

#endif
