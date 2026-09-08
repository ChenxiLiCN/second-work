#include "hin/HinGraph.h"
#include "index/UniversalPathIndex.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void print_usage(const char* executable) {
    std::cerr << "Usage: " << executable
              << " <hin-directory> <upi-directory> [max-half-length]"
                 " [certificate-budget-percent] [seci-coverage-floors]"
                 " [max-precomputed-paths]\n"
                 "  floors example: 0.3,0.5,0.7,0.9\n";
}

std::uint32_t parse_budget_percent(const std::string& value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoul(value, &consumed);
    if (consumed != value.size() || parsed > 1000) {
        throw std::invalid_argument(
            "certificate-budget-percent must be an integer in [0, 1000]");
    }
    return static_cast<std::uint32_t>(parsed);
}

std::uint32_t parse_max_half_length(const std::string& value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoul(value, &consumed);
    if (consumed != value.size() || parsed == 0 || parsed > 32) {
        throw std::invalid_argument(
            "max-half-length must be an integer in [1, 32]");
    }
    return static_cast<std::uint32_t>(parsed);
}

std::uint64_t parse_max_precomputed_paths(const std::string& value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed);
    if (consumed != value.size()) {
        throw std::invalid_argument(
            "max-precomputed-paths must be a nonnegative integer");
    }
    return parsed;
}

std::vector<hinscan::SimilarityThreshold> parse_coverage_floors(
    const std::string& value) {
    std::vector<hinscan::SimilarityThreshold> floors;
    std::istringstream input(value);
    std::string token;
    while (std::getline(input, token, ',')) {
        if (token.empty()) {
            throw std::invalid_argument("SECI coverage list contains an empty value");
        }
        floors.push_back(hinscan::SimilarityThreshold::parse(token));
    }
    if (floors.empty()) {
        throw std::invalid_argument("SECI coverage list is empty");
    }
    return floors;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 7) {
        print_usage(argv[0]);
        return 2;
    }
    try {
        const std::filesystem::path hin_directory = argv[1];
        const std::filesystem::path upi_directory = argv[2];
        const auto max_half_length =
            argc >= 4 ? parse_max_half_length(argv[3]) : 3U;
        const auto certificate_budget_percent =
            argc >= 5 ? parse_budget_percent(argv[4]) : 100U;
        const auto selective_coverage_floors =
            argc >= 6 ? parse_coverage_floors(argv[5])
                      : std::vector<hinscan::SimilarityThreshold>{{9, 10}};
        const auto max_precomputed_paths =
            argc == 7 ? parse_max_precomputed_paths(argv[6])
                      : 64;
        const auto graph = hinscan::HinGraph::load(hin_directory);
        const auto index = hinscan::UniversalPathIndex::build(
            graph, upi_directory, max_half_length,
            certificate_budget_percent, selective_coverage_floors,
            max_precomputed_paths);
        const auto& stats = index.stats();
        std::cout << "hin_directory=" << hin_directory.string() << '\n'
                  << "upi_directory=" << index.directory().string() << '\n'
                  << "max_half_length=" << stats.max_half_length << '\n'
                  << "max_full_length=" << 2 * stats.max_half_length << '\n'
                  << "path_entries=" << stats.path_entries << '\n'
                  << "total_half_path_incidences="
                  << stats.total_half_path_incidences << '\n'
                  << "equivalence_entries=" << stats.equivalence_entries << '\n'
                  << "equivalence_file_bytes="
                  << stats.equivalence_file_bytes << '\n'
                  << "certificate_entries=" << stats.certificate_entries << '\n'
                  << "certificate_file_bytes="
                  << stats.certificate_file_bytes << '\n'
                  << "certificate_budget_percent="
                  << stats.certificate_budget_percent << '\n'
                  << "selective_entries=" << stats.selective_entries << '\n'
                  << "selective_file_bytes="
                  << stats.selective_file_bytes << '\n'
                  << "base_relation_file_bytes="
                  << stats.base_relation_file_bytes << '\n'
                  << "precomputed_path_limit="
                  << stats.precomputed_path_limit << '\n'
                  << "selective_coverage_layers=";
        for (std::size_t i = 0;
             i < index.selective_coverage_floors().size(); ++i) {
            if (i != 0) { std::cout << ','; }
            const auto& floor = index.selective_coverage_floors()[i];
            std::cout << floor.numerator << '/' << floor.denominator;
        }
        std::cout << '\n'
                  << "total_index_bytes=" << stats.total_file_bytes << '\n'
                  << "build_ms=" << stats.build_milliseconds << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "upi_build_index: " << error.what() << '\n';
        return 1;
    }
}
