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
              << " <index.fli> <epsilon> <mu> <output-directory>\n";
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
    if (argc != 5) {
        print_usage(argv[0]);
        return 2;
    }

    try {
        const std::filesystem::path index_file = argv[1];
        const std::string epsilon_text = argv[2];
        const auto mu = parse_mu(argv[3]);
        const std::filesystem::path output_directory = argv[4];
        const auto load_begin = std::chrono::steady_clock::now();
        const auto index = hinscan::FactorIndex::load(index_file);
        const auto load_end = std::chrono::steady_clock::now();
        const auto threshold = hinscan::SimilarityThreshold::parse(epsilon_text);
        const auto result = hinscan::run_fli_scan(index, threshold, mu);
        hinscan::write_fli_scan_results(output_directory, epsilon_text, mu, result);

        const auto load_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(load_end -
                                                                  load_begin)
                .count();
        const auto& stats = result.stats;
        std::cout << "index_file=" << index_file.string() << '\n'
                  << "epsilon=" << epsilon_text << '\n'
                  << "mu=" << mu << '\n'
                  << "index_load_ms=" << load_ms << '\n'
                  << "query_ms=" << stats.query_milliseconds << '\n'
                  << "projected_edges="
                  << index.stats().exact_projected_edges << '\n'
                  << "degree_pruned_checks=" << stats.degree_pruned_checks
                  << '\n'
                  << "exact_intersection_checks="
                  << stats.exact_intersection_checks << '\n'
                  << "early_accept_checks=" << stats.early_accept_checks << '\n'
                  << "early_reject_checks=" << stats.early_reject_checks << '\n'
                  << "posting_entries_read=" << stats.posting_entries_read
                  << '\n'
                  << "positive_certificate_bytes="
                  << stats.positive_certificate_bytes << '\n'
                  << "timestamp_workspace_bytes="
                  << stats.timestamp_workspace_bytes << '\n'
                  << "core_vertices=" << stats.core_vertices << '\n'
                  << "clusters=" << stats.clusters << '\n'
                  << "output=" << output_directory.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fli_query_index: " << error.what() << '\n';
        return 1;
    }
}
