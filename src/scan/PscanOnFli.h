#ifndef HINSCAN_PSCAN_ON_FLI_H
#define HINSCAN_PSCAN_ON_FLI_H

#include "index/FactorIndex.h"
#include "index/FingerprintNeighborhoodIndex.h"
#include "index/BudgetedSimilarityIndex.h"
#include "scan/ExactNeighborhoodCache.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hinscan {

// Experimental execution variants; the default remains the verified V11 path.
enum class BlockExecutionMode { Disabled, SeedOnly, SkipCertified, WitnessBounds, AdaptiveWitnessBounds, WitnessBitmaps, WitnessExclusion };

enum class PscanVertexRole {
    Core,
    Border,
    Hub,
    Outlier,
};

const char* pscan_role_name(PscanVertexRole role) noexcept;

struct PscanOnFliStats {
    std::uint64_t projected_edges_seen_in_prune = 0;
    std::uint64_t degree_pruned_edges = 0;
    std::uint64_t automatically_similar_edges = 0;
    std::uint64_t initially_uncertain_edges = 0;
    std::uint64_t exact_similarity_checks = 0;
    std::uint64_t exact_similar_certificates = 0;
    std::uint64_t exact_dissimilar_certificates = 0;
    std::uint64_t certificate_hits = 0;
    std::uint64_t similarity_posting_entries_read = 0;
    std::uint64_t early_accept_checks = 0;
    std::uint64_t early_reject_checks = 0;
    std::uint64_t fingerprint_bound_checks = 0;
    std::uint64_t fingerprint_bound_rejections = 0;
    std::uint64_t fingerprint_entries_read = 0;
    std::uint64_t budgeted_index_checks = 0;
    std::uint64_t budgeted_index_hits = 0;
    std::uint64_t cached_similarity_checks = 0;
    std::uint64_t cached_similarity_entries_read = 0;
    std::uint64_t neighborhood_generations = 0;
    std::uint64_t neighborhood_posting_entries_read = 0;
    std::uint64_t degree_bucket_candidate_generations = 0;
    std::uint64_t degree_bucket_posting_entries_read = 0;
    std::uint64_t degree_bucket_posting_entries_skipped = 0;
    std::uint64_t degree_compatible_edges_enumerated = 0;
    std::uint64_t neighborhood_cache_hits = 0;
    std::uint64_t neighborhood_cache_misses = 0;
    std::uint64_t neighborhood_cache_evictions = 0;
    std::uint64_t neighborhood_cache_entries = 0;
    std::uint64_t neighborhood_cache_payload_bytes = 0;
    std::uint64_t neighborhood_cache_peak_payload_bytes = 0;
    std::uint64_t timestamp_workspace_bytes = 0;
    std::uint64_t peak_ephemeral_neighborhood_bytes = 0;
    std::uint64_t core_vertices = 0;
    std::uint64_t clusters = 0;
    std::uint64_t border_vertices = 0;
    std::uint64_t hub_vertices = 0;
    std::uint64_t outlier_vertices = 0;
    std::uint64_t sparse_certificate_entries = 0;
    std::uint64_t sparse_certificate_table_bytes = 0;
    std::uint64_t query_milliseconds = 0;
    double prune_ms = 0, core_ms = 0, noncore_ms = 0;
    ExactNeighborhoodCacheStats adaptive_cache;
    std::uint64_t adaptive_workspace_bytes = 0;
    double block_discovery_ms = 0;
    std::uint64_t certified_blocks = 0, block_core_vertices = 0;
    std::uint64_t block_incidences = 0, block_workspace_bytes = 0;
    std::uint64_t block_prefix_entries_skipped = 0, seed_component_entries_skipped = 0;
    std::uint64_t final_core_entries_skipped = 0, candidate_vertices_emitted = 0;
};

struct PscanOnFliResult {
    std::vector<bool> is_core;
    std::vector<VertexId> core_cluster;
    std::vector<std::vector<VertexId>> noncore_clusters;
    std::vector<PscanVertexRole> roles;
    PscanOnFliStats stats;
};

PscanOnFliResult run_pscan_on_fli(const FactorIndex& index,
                                  const SimilarityThreshold& threshold,
                                  std::uint64_t mu,
                                  std::uint64_t neighborhood_cache_bytes = 32ULL * 1024 * 1024,
                                  const FingerprintNeighborhoodIndex*
                                      fingerprint_index = nullptr,
                                  const BudgetedSimilarityIndex*
                                      budgeted_index = nullptr,
                                  bool adaptive_neighborhoods = true,
                                  BlockExecutionMode block_mode = BlockExecutionMode::Disabled);

void write_pscan_on_fli_results(
    const std::filesystem::path& output_directory,
    const std::string& epsilon_text,
    std::uint64_t mu,
    const PscanOnFliResult& result);

}  // namespace hinscan

#endif
