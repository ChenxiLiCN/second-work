#include "index/EquivalenceIndex.h"
#include "index/FactorIndex.h"
#include "scan/PscanOnFli.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <index.eqi> <epsilon> <mu> <output-directory>\n";
        return 2;
    }
    try {
        std::size_t consumed = 0;
        const auto mu = std::stoull(argv[3], &consumed);
        if (consumed != std::string(argv[3]).size() || mu == 0) {
            throw std::invalid_argument("mu must be a positive integer");
        }
        const auto load_begin = std::chrono::steady_clock::now();
        const auto index = hinscan::EquivalenceIndex::load(argv[1]);
        const auto load_end = std::chrono::steady_clock::now();
        const auto threshold = hinscan::SimilarityThreshold::parse(argv[2]);
        auto query = hinscan::run_pscan_on_equivalence_index(index, threshold, mu);
        hinscan::write_pscan_on_fli_results(argv[4], argv[2], mu,
                                            query.clustering);
        const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 load_end - load_begin)
                                 .count();
        const auto& stats = query.clustering.stats;
        std::cout << "index_load_ms=" << load_ms << '\n'
                  << "query_ms=" << stats.query_milliseconds << '\n'
                  << "online_total_ms=" << load_ms + stats.query_milliseconds
                  << '\n'
                  << "vertices=" << index.stats().vertices << '\n'
                  << "classes=" << index.stats().classes << '\n'
                  << "projected_edges=" << index.stats().projected_edges << '\n'
                  << "quotient_edges=" << index.stats().quotient_edges << '\n'
                  << "quotient_edges_checked=" << query.quotient_edges_checked
                  << '\n'
                  << "similar_quotient_edges=" << query.similar_quotient_edges
                  << '\n'
                  << "core_vertices=" << stats.core_vertices << '\n'
                  << "clusters=" << stats.clusters << '\n'
                  << "border_vertices=" << stats.border_vertices << '\n'
                  << "hub_vertices=" << stats.hub_vertices << '\n'
                  << "outlier_vertices=" << stats.outlier_vertices << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pscan_on_eqi: " << error.what() << '\n';
        return 1;
    }
}
