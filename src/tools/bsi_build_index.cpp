#include "index/BudgetedSimilarityIndex.h"
#include "index/FactorIndex.h"

#include <chrono>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <index.fli> <output.bsi> <maximum-entries>\n";
        return 2;
    }
    try {
        const auto begin = std::chrono::steady_clock::now();
        const auto factor = hinscan::FactorIndex::load(argv[1]);
        const auto load_end = std::chrono::steady_clock::now();
        const auto index = hinscan::BudgetedSimilarityIndex::build(
            factor, std::stoull(argv[3]));
        const auto build_end = std::chrono::steady_clock::now();
        index.save(argv[2]);
        const auto save_end = std::chrono::steady_clock::now();
        auto milliseconds = [](auto duration) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(duration)
                .count();
        };
        std::cout << "factor_index=" << argv[1] << '\n'
                  << "budgeted_index=" << argv[2] << '\n'
                  << "candidate_edges=" << index.stats().candidate_edges << '\n'
                  << "retained_edges=" << index.entry_count() << '\n'
                  << "estimated_bytes=" << index.estimated_bytes() << '\n'
                  << "file_bytes=" << std::filesystem::file_size(argv[2]) << '\n'
                  << "temporary_neighborhood_bytes="
                  << index.stats().temporary_neighborhood_bytes << '\n'
                  << "factor_load_ms=" << milliseconds(load_end - begin) << '\n'
                  << "build_ms=" << milliseconds(build_end - load_end) << '\n'
                  << "save_ms=" << milliseconds(save_end - build_end) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bsi_build_index: " << error.what() << '\n';
        return 1;
    }
}
