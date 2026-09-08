#include "index/SelectiveEquivalenceIndex.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <index.seci> <epsilon> <mu> <output-directory>\n";
        return 2;
    }
    try {
        std::size_t consumed = 0;
        const auto mu = std::stoull(argv[3], &consumed);
        if (consumed != std::string(argv[3]).size() || mu == 0) {
            throw std::invalid_argument("mu must be a positive integer");
        }
        const auto threshold = hinscan::SimilarityThreshold::parse(argv[2]);
        const auto load_begin = std::chrono::steady_clock::now();
        const auto index =
            hinscan::SelectiveEquivalenceIndex::load(argv[1], threshold);
        const auto load_end = std::chrono::steady_clock::now();
        auto query = hinscan::run_pscan_on_selective_equivalence_index(
            index, threshold, mu);
        hinscan::write_pscan_on_fli_results(argv[4], argv[2], mu,
                                            query.clustering);
        const auto total_end = std::chrono::steady_clock::now();
        const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            load_end - load_begin).count();
        const auto end_to_end_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                total_end - load_begin).count();
        const auto& stats = query.clustering.stats;
        std::cout << "index_load_ms=" << load_ms << '\n'
                  << "query_ms=" << stats.query_milliseconds << '\n'
                  << "end_to_end_ms=" << end_to_end_ms << '\n'
                  << "vertices=" << index.stats().vertices << '\n'
                  << "classes=" << index.stats().classes << '\n'
                  << "retained_certificates="
                  << index.stats().retained_certificates << '\n'
                  << "loaded_certificates=" << index.certificates().size()
                  << '\n'
                  << "similar_class_edges=" << query.similar_class_edges << '\n'
                  << "certificates_skipped=" << query.certificates_skipped
                  << '\n'
                  << "core_vertices=" << stats.core_vertices << '\n'
                  << "clusters=" << stats.clusters << '\n'
                  << "border_vertices=" << stats.border_vertices << '\n'
                  << "hub_vertices=" << stats.hub_vertices << '\n'
                  << "outlier_vertices=" << stats.outlier_vertices << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pscan_on_seci: " << error.what() << '\n';
        return 1;
    }
}
