#include "index/EquivalenceIndex.h"
#include "index/FactorIndex.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <factor-index.fli> <output.eqi>\n";
        return 2;
    }
    try {
        const auto factor = hinscan::FactorIndex::load(argv[1]);
        const auto index = hinscan::EquivalenceIndex::build(factor);
        index.save(argv[2]);
        const auto& stats = index.stats();
        std::cout << "factor_index=" << argv[1] << '\n'
                  << "equivalence_index=" << argv[2] << '\n'
                  << "vertices=" << stats.vertices << '\n'
                  << "classes=" << stats.classes << '\n'
                  << "projected_edges=" << stats.projected_edges << '\n'
                  << "quotient_edges=" << stats.quotient_edges << '\n'
                  << "build_ms=" << stats.build_milliseconds << '\n'
                  << "file_bytes=" << std::filesystem::file_size(argv[2]) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "eqi_build_index: " << error.what() << '\n';
        return 1;
    }
}
