#include "index/BudgetedEquivalenceIndex.h"
#include "index/FactorIndex.h"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <factor-index.fli> <output.beqi> <count-budget>\n";
        return 2;
    }
    try {
        const auto factor = hinscan::FactorIndex::load(argv[1]);
        const auto index = hinscan::BudgetedEquivalenceIndex::build(
            factor, std::stoull(argv[3]));
        index.save(argv[2]);
        const auto& stats = index.stats();
        std::cout << "vertices=" << stats.vertices << '\n'
                  << "classes=" << stats.classes << '\n'
                  << "projected_edges=" << stats.projected_edges << '\n'
                  << "quotient_edges=" << stats.quotient_edges << '\n'
                  << "retained_counts=" << stats.retained_counts << '\n'
                  << "build_ms=" << stats.build_milliseconds << '\n'
                  << "estimated_bytes=" << index.estimated_bytes() << '\n'
                  << "file_bytes=" << std::filesystem::file_size(argv[2])
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "beqi_build_index: " << error.what() << '\n';
        return 1;
    }
}
