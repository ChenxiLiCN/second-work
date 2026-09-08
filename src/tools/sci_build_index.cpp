#include "index/FactorIndex.h"
#include "index/SimilarityCertificateIndex.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <factor-index.fli> <output.sci>\n";
        return 2;
    }
    try {
        const auto factor = hinscan::FactorIndex::load(argv[1]);
        const auto index = hinscan::SimilarityCertificateIndex::build(factor);
        index.save(argv[2]);
        const auto& stats = index.stats();
        std::cout << "factor_index=" << argv[1] << '\n'
                  << "similarity_certificate_index=" << argv[2] << '\n'
                  << "vertices=" << stats.vertices << '\n'
                  << "projected_edges=" << stats.projected_edges << '\n'
                  << "closed_neighborhood_entries="
                  << stats.closed_neighborhood_entries << '\n'
                  << "build_ms=" << stats.build_milliseconds << '\n'
                  << "file_bytes=" << std::filesystem::file_size(argv[2]) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sci_build_index: " << error.what() << '\n';
        return 1;
    }
}
