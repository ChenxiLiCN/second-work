#include "hin/HinGraph.h"
#include "index/FactorIndex.h"
#include "scan/PscanOnFli.h"
#include "scan/QueryProfile.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

#ifndef HINSCAN_BLOCK_MODE
#define HINSCAN_BLOCK_MODE 0
#endif
#ifndef HINSCAN_CACHE_MIB
#define HINSCAN_CACHE_MIB 32
#endif

// Always a first-path query: no FLI, class index, similarity file, or query
// result is read or persisted. Only the schema-independent BRI is persistent.
int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "Usage: " << argv[0]
                  << " <base.bri> <meta-path> <epsilon> <mu> <output-directory>\n";
        return 2;
    }
    try {
        using Clock = std::chrono::steady_clock;
        auto ms = [](auto duration) { return std::chrono::duration<double, std::milli>(duration).count(); };
        const auto start = Clock::now();
        hinscan::profile::reset();
        const auto threshold = hinscan::SimilarityThreshold::parse(argv[3]);
        std::size_t consumed = 0;
        const auto mu = std::stoull(argv[4], &consumed);
        if (consumed != std::string(argv[4]).size() || mu == 0 || argv[4][0] == '-')
            throw std::invalid_argument("mu must be positive");
        const auto graph = hinscan::HinGraph::load_binary(argv[1]);
        const auto path = hinscan::parse_meta_path(graph, argv[2]);
        const auto loaded = Clock::now();
        const auto factor = hinscan::FactorIndex::build(graph, path);
        const auto built = Clock::now();
        const auto result = hinscan::run_pscan_on_fli(factor, threshold, mu,
            HINSCAN_CACHE_MIB * 1024ULL * 1024, nullptr, nullptr, true,
            static_cast<hinscan::BlockExecutionMode>(HINSCAN_BLOCK_MODE));
        const auto queried = Clock::now();
        hinscan::write_pscan_on_fli_results(argv[5], argv[3], mu, result);
        const auto saved = Clock::now();
        const auto& f = factor.stats();
        const auto& q = result.stats;
        std::cout << "index_mode=base_relation_on_demand_fli\n"
                  << "block_mode=" << HINSCAN_BLOCK_MODE << '\n'
                  << "single_pass=" << (HINSCAN_BLOCK_MODE == 10 || HINSCAN_BLOCK_MODE == 11 || HINSCAN_BLOCK_MODE == 12) << '\n'
                  << "lean_workspaces=" << (HINSCAN_BLOCK_MODE == 11 || HINSCAN_BLOCK_MODE == 12) << '\n'
                  << "anchor_filter=" << (HINSCAN_BLOCK_MODE == 12) << '\n'
                  << "anchor_prepare_ms=" << q.anchor.prepare_ms << '\n'
                  << "anchor_filter_ms=" << q.anchor.filter_ms << '\n'
                  << "anchor_calls=" << q.anchor.calls << '\n'
                  << "anchor_ineligible=" << q.anchor.ineligible << '\n'
                  << "anchor_no_anchor=" << q.anchor.no_anchor << '\n'
                  << "anchor_eligible=" << q.anchor.eligible << '\n'
                  << "anchor_rejects=" << q.anchor.rejects << '\n'
                  << "anchor_fallbacks=" << q.anchor.fallbacks << '\n'
                  << "anchor_same_anchor=" << q.anchor.same_anchor << '\n'
                  << "anchor_cache_hits=" << q.anchor.cache_hits << '\n'
                  << "anchor_cache_misses=" << q.anchor.cache_misses << '\n'
                  << "anchor_replacements=" << q.anchor.replacements << '\n'
                  << "anchor_resumed=" << q.anchor.resumed << '\n'
                  << "anchor_cached_rejects=" << q.anchor.cached_rejects << '\n'
                  << "anchor_cached_fallbacks=" << q.anchor.cached_fallbacks << '\n'
                  << "anchor_comparisons=" << q.anchor.comparisons << '\n'
                  << "anchor_entries_advanced=" << q.anchor.entries_advanced << '\n'
                  << "anchor_selection_entries=" << q.anchor.selection_entries << '\n'
                  << "anchor_vertices=" << q.anchor.anchored_vertices << '\n'
                  << "anchor_coverage_55_vertices=" << q.anchor.coverage_55_vertices << '\n'
                  << "anchor_coverage_75_vertices=" << q.anchor.coverage_75_vertices << '\n'
                  << "anchor_mapping_bytes=" << q.anchor.mapping_bytes << '\n'
                  << "anchor_state_bytes=" << q.anchor.state_bytes << '\n'
                  << "used_roundtrip_metadata=" << f.used_roundtrip_metadata << '\n'
                  << "cache_budget_mib=" << HINSCAN_CACHE_MIB << '\n'
                  << "block_discovery_ms=" << q.block_discovery_ms << '\n'
                  << "certified_blocks=" << q.certified_blocks << '\n'
                  << "block_core_vertices=" << q.block_core_vertices << '\n'
                  << "block_incidences=" << q.block_incidences << '\n'
                  << "block_workspace_bytes=" << q.block_workspace_bytes << '\n'
                  << "block_prefix_entries_skipped=" << q.block_prefix_entries_skipped << '\n'
                  << "seed_component_entries_skipped=" << q.seed_component_entries_skipped << '\n'
                  << "final_core_entries_skipped=" << q.final_core_entries_skipped << '\n'
                  << "candidate_vertices_emitted=" << q.candidate_vertices_emitted << '\n'
                  << "candidate_posting_entries_read=" << q.degree_bucket_posting_entries_read << '\n'
                  << "certificate_entries=" << q.sparse_certificate_entries << '\n'
                  << "certificate_bytes=" << q.sparse_certificate_table_bytes << '\n'
                  << "core_vertices=" << q.core_vertices << '\n'
                  << "clusters=" << q.clusters << '\n'
                  << "base_relation_load_ms=" << ms(loaded-start) << '\n'
                  << "on_demand_fli_build_ms=" << ms(built-loaded) << '\n'
                  << "half_expansion_ms=" << f.half_expansion_ms << '\n'
                  << "degree_compute_ms=" << f.degree_compute_ms << '\n'
                  << "posting_order_ms=" << f.posting_order_ms << '\n'
                  << "query_call_ms=" << ms(queried-built) << '\n'
                  << "prune_ms=" << q.prune_ms << '\n'
                  << "core_ms=" << q.core_ms << '\n'
                  << "noncore_ms=" << q.noncore_ms << '\n'
                  << "output_write_ms=" << ms(saved-queried) << '\n'
                  << "online_with_output_ms=" << ms(saved-start) << '\n'
                  << "adaptive_generation_ms=" << q.adaptive_cache.generation_ms << '\n'
                  << "adaptive_peak_bytes=" << q.adaptive_cache.peak_bytes << '\n'
                  << "adaptive_workspace_bytes=" << q.adaptive_workspace_bytes << '\n'
                  << "adaptive_posting_entries=" << q.adaptive_cache.posting_entries << '\n'
                  << "adaptive_hits=" << q.adaptive_cache.hits << '\n'
                  << "adaptive_misses=" << q.adaptive_cache.misses << '\n'
                  << "adaptive_evictions=" << q.adaptive_cache.evictions << '\n'
                  << "adaptive_distinct_factor_rows=" << q.adaptive_cache.distinct_factor_rows << '\n'
                  << "adaptive_equal_factor_checks=" << q.adaptive_cache.equal_factor_checks << '\n'
                  << "adaptive_pair_bound_hits=" << q.adaptive_cache.pair_bound_hits << '\n'
                  << "adaptive_pair_bound_bytes=" << q.adaptive_cache.pair_bound_bytes << '\n'
                  << "adaptive_streaming_checks=" << q.adaptive_cache.streaming_checks << '\n'
                  << "single_pass_complete=" << q.adaptive_cache.single_pass_complete << '\n'
                  << "single_pass_partial=" << q.adaptive_cache.single_pass_partial << '\n'
                  << "activation_calls=" << q.adaptive_cache.activation_calls << '\n'
                  << "activation_full_words_written=" << q.adaptive_cache.activation_full_words_written << '\n'
                  << "activation_sparse_words_cleared=" << q.adaptive_cache.activation_sparse_words_cleared << '\n'
                  << "streaming_bound_evaluations=" << q.adaptive_cache.streaming_bound_evaluations << '\n'
                  << "streaming_duplicate_visits=" << q.adaptive_cache.streaming_duplicate_visits << '\n'
                  << "single_pass_early_accepts=" << q.adaptive_cache.single_pass_early_accepts << '\n'
                  << "single_pass_early_rejects=" << q.adaptive_cache.single_pass_early_rejects << '\n'
                  << "adaptive_list_checks=" << q.adaptive_cache.list_checks << '\n'
                  << "adaptive_bitmap_checks=" << q.adaptive_cache.bitmap_checks << '\n'
                  << "adaptive_full_checks=" << (q.adaptive_cache.streaming_checks + q.adaptive_cache.list_checks + q.adaptive_cache.bitmap_checks) << '\n'
                  << "adaptive_intersection_units=" << q.adaptive_cache.intersection_units << '\n'
                  << "adaptive_streaming_entries=" << q.adaptive_cache.streaming_posting_entries << '\n'
                  << "adaptive_streaming_ms=" << q.adaptive_cache.streaming_ms << '\n'
                  << "adaptive_factor_sharing_ms=" << q.adaptive_cache.factor_sharing_ms << '\n'
                  << "witness_bound_checks=" << q.adaptive_cache.witness_bound_checks << '\n'
                  << "witness_bound_accepts=" << q.adaptive_cache.witness_bound_accepts << '\n'
                  << "witness_bound_rejects=" << q.adaptive_cache.witness_bound_rejects << '\n'
                  << "witness_count_hits=" << q.adaptive_cache.witness_count_hits << '\n'
                  << "witness_counts_built=" << q.adaptive_cache.witness_counts_built << '\n'
                  << "witness_entries_read=" << q.adaptive_cache.witness_entries_read << '\n'
                  << "witness_bitmap_bytes=" << q.adaptive_cache.witness_bitmap_bytes << '\n'
                  << "witness_bitmap_build_ms=" << q.adaptive_cache.witness_bitmap_build_ms << '\n'
                  << "witness_bitmap_build_entries=" << q.adaptive_cache.witness_bitmap_build_entries << '\n'
                  << "witness_bitmap_checks=" << q.adaptive_cache.witness_bitmap_checks << '\n'
                  << "witness_bitmap_words=" << q.adaptive_cache.witness_bitmap_words << '\n'
                  << "witness_exclusion_rejects=" << q.adaptive_cache.witness_exclusion_rejects << '\n'
                  << "exact_similarity_checks=" << q.exact_similarity_checks << '\n'
                  << "projected_edges=" << f.exact_projected_edges << '\n';
        hinscan::profile::write(std::cout);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bri_query_index: " << error.what() << '\n';
        return 1;
    }
}
