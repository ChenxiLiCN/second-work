#include "index/BudgetedEquivalenceIndex.h"
#include "index/FactorIndex.h"
#include "scan/PscanOnFli.h"

#include <chrono>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "Usage: " << argv[0]
                  << " <index.beqi> <factor.fli> <epsilon> <mu> <output>\n";
        return 2;
    }
    try {
        const auto load_begin = std::chrono::steady_clock::now();
        const auto index = hinscan::BudgetedEquivalenceIndex::load(argv[1]);
        const auto factor = hinscan::FactorIndex::load(argv[2]);
        const auto load_end = std::chrono::steady_clock::now();
        const auto threshold = hinscan::SimilarityThreshold::parse(argv[3]);
        const auto mu = std::stoull(argv[4]);
        const auto query = hinscan::run_pscan_on_budgeted_equivalence(
            index, factor, threshold, mu);
        hinscan::write_pscan_on_fli_results(argv[5], argv[3], mu,
                                            query.clustering);
        const auto load_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                load_end - load_begin).count();
        std::cout << "index_load_ms=" << load_ms << '\n'
                  << "query_ms=" << query.clustering.stats.query_milliseconds
                  << '\n'
                  << "online_total_ms="
                  << load_ms + query.clustering.stats.query_milliseconds << '\n'
                  << "class_edges_considered="
                  << query.class_edges_considered << '\n'
                  << "exact_class_checks=" << query.exact_class_checks << '\n'
                  << "indexed_count_hits=" << query.indexed_count_hits << '\n'
                  << "fli_fallback_checks=" << query.fli_fallback_checks << '\n'
                  << "core_vertices=" << query.clustering.stats.core_vertices
                  << '\n'
                  << "clusters=" << query.clustering.stats.clusters << '\n'
                  << "border_vertices="
                  << query.clustering.stats.border_vertices << '\n'
                  << "hub_vertices=" << query.clustering.stats.hub_vertices
                  << '\n'
                  << "outlier_vertices="
                  << query.clustering.stats.outlier_vertices << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pscan_on_beqi: " << error.what() << '\n';
        return 1;
    }
}
