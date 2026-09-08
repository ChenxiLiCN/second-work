#include "index/FactorIndex.h"
#include "index/FingerprintNeighborhoodIndex.h"

#include <chrono>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <index.fli> <output.fni>\n";
        return 2;
    }
    try {
        const auto begin = std::chrono::steady_clock::now();
        const auto factor = hinscan::FactorIndex::load(argv[1]);
        const auto load_end = std::chrono::steady_clock::now();
        const auto index = hinscan::FingerprintNeighborhoodIndex::build(factor);
        const auto build_end = std::chrono::steady_clock::now();
        index.save(argv[2]);
        const auto save_end = std::chrono::steady_clock::now();
        auto milliseconds = [](auto duration) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(duration)
                .count();
        };
        std::cout << "factor_index=" << argv[1] << '\n'
                  << "fingerprint_index=" << argv[2] << '\n'
                  << "vertex_count=" << index.vertex_count() << '\n'
                  << "fingerprint_count=" << index.fingerprint_count() << '\n'
                  << "estimated_bytes=" << index.estimated_bytes() << '\n'
                  << "file_bytes=" << std::filesystem::file_size(argv[2]) << '\n'
                  << "factor_load_ms=" << milliseconds(load_end - begin) << '\n'
                  << "build_ms=" << milliseconds(build_end - load_end) << '\n'
                  << "save_ms=" << milliseconds(save_end - build_end) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fni_build_index: " << error.what() << '\n';
        return 1;
    }
}
