#include "index/FactorIndex.h"
#include "index/FingerprintNeighborhoodIndex.h"
#include "index/BudgetedSimilarityIndex.h"
#include "scan/PscanOnFli.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void print_usage(const char* executable) {
    std::cerr << "Usage: " << executable
              << " <index.fli> <epsilon> <mu> <output-directory>"
                 " [neighborhood-cache-mib] [fingerprint-index.fni|none]"
                 " [budgeted-index.bsi]\n";
}

std::uint64_t parse_mu(const std::string& value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed == 0) {
        throw std::invalid_argument("mu must be a positive integer");
    }
    return parsed;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5 || argc > 8) {
        print_usage(argv[0]);
        return 2;
    }

    try {
        const std::filesystem::path index_file = argv[1];
        const std::string epsilon_text = argv[2];
        const auto mu = parse_mu(argv[3]);
        const std::filesystem::path output_directory = argv[4];
        const auto cache_mib = argc >= 6 ? std::stoull(argv[5]) : 32;
        if (cache_mib > std::numeric_limits<std::uint64_t>::max() /
                            (1024ULL * 1024ULL)) {
            throw std::overflow_error("neighborhood cache size is too large");
        }
        const auto cache_bytes = cache_mib * 1024ULL * 1024ULL;

        const auto load_begin = std::chrono::steady_clock::now();
        const auto index = hinscan::FactorIndex::load(index_file);
        const auto factor_load_end = std::chrono::steady_clock::now();
        const auto fingerprint_index = argc >= 7 && std::string(argv[6]) != "none"
            ? std::make_unique<hinscan::FingerprintNeighborhoodIndex>(
                  hinscan::FingerprintNeighborhoodIndex::load(argv[6]))
            : nullptr;
        const auto budgeted_index = argc == 8
            ? std::make_unique<hinscan::BudgetedSimilarityIndex>(
                  hinscan::BudgetedSimilarityIndex::load(argv[7]))
            : nullptr;
        const auto load_end = std::chrono::steady_clock::now();
        const auto threshold = hinscan::SimilarityThreshold::parse(epsilon_text);
        const auto result =
            hinscan::run_pscan_on_fli(index, threshold, mu, cache_bytes,
                                      fingerprint_index.get(),
                                      budgeted_index.get());
        const auto output_begin = std::chrono::steady_clock::now();
        hinscan::write_pscan_on_fli_results(
            output_directory, epsilon_text, mu, result);
        const auto output_end = std::chrono::steady_clock::now();

        const auto load_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(load_end -
                                                                  load_begin)
                .count();
        const auto& stats = result.stats;
        std::cout << "prune_ms=" << stats.prune_ms << '\n'
                  << "core_ms=" << stats.core_ms << '\n'
                  << "noncore_ms=" << stats.noncore_ms << '\n'
                  << "adaptive_hits=" << stats.adaptive_cache.hits << '\n'
                  << "adaptive_misses=" << stats.adaptive_cache.misses << '\n'
                  << "adaptive_evictions=" << stats.adaptive_cache.evictions << '\n'
                  << "adaptive_peak_bytes=" << stats.adaptive_cache.peak_bytes << '\n'
                  << "adaptive_workspace_bytes=" << stats.adaptive_workspace_bytes << '\n'
                  << "adaptive_generation_ms=" << stats.adaptive_cache.generation_ms << '\n'
                  << "adaptive_posting_entries=" << stats.adaptive_cache.posting_entries << '\n'
                  << "adaptive_bitmap_checks=" << stats.adaptive_cache.bitmap_checks << '\n'
                  << "adaptive_list_checks=" << stats.adaptive_cache.list_checks << '\n'
                  << "adaptive_intersection_units=" << stats.adaptive_cache.intersection_units << '\n'
                  << "adaptive_distinct_factor_rows=" << stats.adaptive_cache.distinct_factor_rows << '\n'
                  << "adaptive_equal_factor_checks=" << stats.adaptive_cache.equal_factor_checks << '\n'
                  << "adaptive_pair_bound_hits=" << stats.adaptive_cache.pair_bound_hits << '\n'
                  << "adaptive_pair_bound_bytes=" << stats.adaptive_cache.pair_bound_bytes << '\n'
                  << "adaptive_streaming_checks=" << stats.adaptive_cache.streaming_checks << '\n'
                  << "adaptive_streaming_entries=" << stats.adaptive_cache.streaming_posting_entries << '\n'
                  << "adaptive_streaming_ms=" << stats.adaptive_cache.streaming_ms << '\n'
                  << "adaptive_factor_sharing_ms=" << stats.adaptive_cache.factor_sharing_ms << '\n'
                  << "output_write_ms=" << std::chrono::duration<double, std::milli>(output_end - output_begin).count() << '\n'
                  << "online_with_output_ms=" << std::chrono::duration<double, std::milli>(output_end - load_begin).count() << '\n';
        std::cout << "index_file=" << index_file.string() << '\n'
                  << "epsilon=" << epsilon_text << '\n'
                  << "mu=" << mu << '\n'
                  << "neighborhood_cache_mib=" << cache_mib << '\n'
                  << "index_load_ms=" << load_ms << '\n'
                  << "factor_load_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         factor_load_end - load_begin).count() << '\n'
                  << "fingerprint_index="
                  << (fingerprint_index ? argv[6] : "none") << '\n'
                  << "budgeted_index="
                  << (budgeted_index ? argv[7] : "none") << '\n'
                  << "query_ms=" << stats.query_milliseconds << '\n'
                  << "projected_edges_seen_in_prune="
                  << stats.projected_edges_seen_in_prune << '\n'
                  << "degree_pruned_edges=" << stats.degree_pruned_edges
                  << '\n'
                  << "automatically_similar_edges="
                  << stats.automatically_similar_edges << '\n'
                  << "initially_uncertain_edges="
                  << stats.initially_uncertain_edges << '\n'
                  << "exact_similarity_checks="
                  << stats.exact_similarity_checks << '\n'
                  << "certificate_hits=" << stats.certificate_hits << '\n'
                  << "sparse_certificate_entries="
                  << stats.sparse_certificate_entries << '\n'
                  << "sparse_certificate_table_bytes="
                  << stats.sparse_certificate_table_bytes << '\n'
                  << "similarity_posting_entries_read="
                  << stats.similarity_posting_entries_read << '\n'
                  << "early_accept_checks=" << stats.early_accept_checks << '\n'
                  << "early_reject_checks=" << stats.early_reject_checks << '\n'
                  << "fingerprint_bound_checks="
                  << stats.fingerprint_bound_checks << '\n'
                  << "fingerprint_bound_rejections="
                  << stats.fingerprint_bound_rejections << '\n'
                  << "fingerprint_entries_read="
                  << stats.fingerprint_entries_read << '\n'
                  << "budgeted_index_checks="
                  << stats.budgeted_index_checks << '\n'
                  << "budgeted_index_hits="
                  << stats.budgeted_index_hits << '\n'
                  << "cached_similarity_checks="
                  << stats.cached_similarity_checks << '\n'
                  << "cached_similarity_entries_read="
                  << stats.cached_similarity_entries_read << '\n'
                  << "neighborhood_generations="
                  << stats.neighborhood_generations << '\n'
                  << "neighborhood_posting_entries_read="
                  << stats.neighborhood_posting_entries_read << '\n'
                  << "degree_bucket_candidate_generations="
                  << stats.degree_bucket_candidate_generations << '\n'
                  << "degree_bucket_posting_entries_read="
                  << stats.degree_bucket_posting_entries_read << '\n'
                  << "degree_bucket_posting_entries_skipped="
                  << stats.degree_bucket_posting_entries_skipped << '\n'
                  << "degree_compatible_edges_enumerated="
                  << stats.degree_compatible_edges_enumerated << '\n'
                  << "neighborhood_cache_hits="
                  << stats.neighborhood_cache_hits << '\n'
                  << "neighborhood_cache_misses="
                  << stats.neighborhood_cache_misses << '\n'
                  << "neighborhood_cache_evictions="
                  << stats.neighborhood_cache_evictions << '\n'
                  << "neighborhood_cache_entries="
                  << stats.neighborhood_cache_entries << '\n'
                  << "neighborhood_cache_payload_bytes="
                  << stats.neighborhood_cache_payload_bytes << '\n'
                  << "neighborhood_cache_peak_payload_bytes="
                  << stats.neighborhood_cache_peak_payload_bytes << '\n'
                  << "timestamp_workspace_bytes="
                  << stats.timestamp_workspace_bytes << '\n'
                  << "peak_ephemeral_neighborhood_bytes="
                  << stats.peak_ephemeral_neighborhood_bytes << '\n'
                  << "core_vertices=" << stats.core_vertices << '\n'
                  << "clusters=" << stats.clusters << '\n'
                  << "border_vertices=" << stats.border_vertices << '\n'
                  << "hub_vertices=" << stats.hub_vertices << '\n'
                  << "outlier_vertices=" << stats.outlier_vertices << '\n'
                  << "output=" << output_directory.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pscan_on_fli: " << error.what() << '\n';
        return 1;
    }
}
