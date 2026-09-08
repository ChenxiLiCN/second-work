#include "index/SelectiveEquivalenceIndex.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    const bool factor_mode = argc == 5 && std::string(argv[1]) == "--factor";
    if (argc != 4 && !factor_mode) {
        std::cerr << "Usage: " << argv[0]
                  << " <materialized-graph-directory> <coverage-floor>"
                     " <output.seci>\n       " << argv[0]
                  << " --factor <factor-index.fli> <coverage-floor>"
                     " <output.seci>\n";
        return 2;
    }
    try {
        const auto input_argument = factor_mode ? 2 : 1;
        const auto floor_argument = factor_mode ? 3 : 2;
        const auto output_argument = factor_mode ? 4 : 3;
        const auto floor =
            hinscan::SimilarityThreshold::parse(argv[floor_argument]);
        const auto index = factor_mode
            ? hinscan::SelectiveEquivalenceIndex::build(
                  hinscan::FactorIndex::load(argv[input_argument]), floor)
            : hinscan::SelectiveEquivalenceIndex::build_from_materialized_graph(
                  argv[input_argument], floor);
        index.save(argv[output_argument]);
        const auto& stats = index.stats();
        std::cout << "vertices=" << stats.vertices << '\n'
                  << "classes=" << stats.classes << '\n'
                  << "projected_edges=" << stats.projected_edges << '\n'
                  << "quotient_edges=" << stats.quotient_edges << '\n'
                  << "degree_candidate_edges=" << stats.degree_candidate_edges
                  << '\n'
                  << "retained_certificates=" << stats.retained_certificates
                  << '\n'
                  << "build_ms=" << stats.build_milliseconds << '\n'
                  << "file_bytes="
                  << std::filesystem::file_size(argv[output_argument])
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "seci_build_index: " << error.what() << '\n';
        return 1;
    }
}
