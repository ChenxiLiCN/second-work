#include "hin/HinGraph.h"
#include "index/FactorIndex.h"
#include "scan/FliScan.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void print_usage(const char* executable) {
    std::cerr << "Usage: " << executable
              << " <hin-dataset-directory> <meta-path> <epsilon> <mu> "
                 "<output-directory>\n";
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
    if (argc != 6) {
        print_usage(argv[0]);
        return 2;
    }

    try {
        const std::filesystem::path dataset_directory = argv[1];
        const std::string meta_path_text = argv[2];
        const std::string epsilon_text = argv[3];
        const auto mu = parse_mu(argv[4]);
        const std::filesystem::path output_directory = argv[5];

        const auto load_begin = std::chrono::steady_clock::now();
        const auto graph = hinscan::HinGraph::load(dataset_directory);
        const auto meta_path = hinscan::parse_meta_path(graph, meta_path_text);
        const auto load_end = std::chrono::steady_clock::now();
        const auto index = hinscan::FactorIndex::build(graph, meta_path);
        const auto threshold = hinscan::SimilarityThreshold::parse(epsilon_text);
        const auto result = hinscan::run_fli_scan(index, threshold, mu);
        hinscan::write_fli_scan_results(output_directory, epsilon_text, mu, result);

        const auto load_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(load_end -
                                                                  load_begin)
                .count();
        const auto& index_stats = index.stats();
        const auto& scan_stats = result.stats;
        std::cout << "dataset=" << dataset_directory.string() << '\n'
                  << "meta_path=" << meta_path_text << '\n'
                  << "epsilon=" << epsilon_text << '\n'
                  << "mu=" << mu << '\n'
                  << "load_ms=" << load_ms << '\n'
                  << "index_build_ms=" << index_stats.build_milliseconds << '\n'
                  << "index_bytes_estimate=" << index_stats.estimated_index_bytes
                  << '\n'
                  << "projected_edges=" << index_stats.exact_projected_edges
                  << '\n'
                  << "query_ms=" << scan_stats.query_milliseconds << '\n'
                  << "pass1_candidate_edges="
                  << scan_stats.pass1_candidate_edges << '\n'
                  << "positive_core_core_edges="
                  << scan_stats.positive_core_core_edges << '\n'
                  << "positive_core_noncore_edges="
                  << scan_stats.positive_core_noncore_edges << '\n'
                  << "degree_pruned_checks="
                  << scan_stats.degree_pruned_checks << '\n'
                  << "exact_intersection_checks="
                  << scan_stats.exact_intersection_checks << '\n'
                  << "early_accept_checks=" << scan_stats.early_accept_checks
                  << '\n'
                  << "early_reject_checks=" << scan_stats.early_reject_checks
                  << '\n'
                  << "candidate_generation_posting_entries_read="
                  << scan_stats.candidate_generation_posting_entries_read
                  << '\n'
                  << "posting_entries_read=" << scan_stats.posting_entries_read
                  << '\n'
                  << "similar_edges=" << scan_stats.similar_edges_pass1 << '\n'
                  << "positive_certificate_bytes="
                  << scan_stats.positive_certificate_bytes << '\n'
                  << "peak_ephemeral_neighborhood_bytes="
                  << scan_stats.peak_ephemeral_neighborhood_bytes << '\n'
                  << "timestamp_workspace_bytes="
                  << scan_stats.timestamp_workspace_bytes << '\n'
                  << "core_vertices=" << scan_stats.core_vertices << '\n'
                  << "clusters=" << scan_stats.clusters << '\n'
                  << "border_vertices=" << scan_stats.border_vertices << '\n'
                  << "hub_vertices=" << scan_stats.hub_vertices << '\n'
                  << "outlier_vertices=" << scan_stats.outlier_vertices << '\n'
                  << "output=" << output_directory.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fli_scan: " << error.what() << '\n';
        return 1;
    }
}
