#include "hin/HinGraph.h"
#include "index/FactorIndex.h"
#include "index/SelectiveEquivalenceIndex.h"
#include "index/UniversalPathIndex.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using Wide = unsigned __int128;

bool equal_threshold(const hinscan::SimilarityThreshold& left,
                     const hinscan::SimilarityThreshold& right) {
    return static_cast<Wide>(left.numerator) * right.denominator ==
           static_cast<Wide>(right.numerator) * left.denominator;
}

std::uint64_t directory_bytes(const std::filesystem::path& directory) {
    if (!std::filesystem::exists(directory)) { return 0; }
    std::uint64_t bytes = 0;
    for (const auto& item :
         std::filesystem::recursive_directory_iterator(directory)) {
        if (!item.is_regular_file()) { continue; }
        const auto size = item.file_size();
        if (size > std::numeric_limits<std::uint64_t>::max() - bytes) {
            throw std::overflow_error("adaptive cache size exceeds uint64");
        }
        bytes += size;
    }
    return bytes;
}

std::uint64_t parse_budget_mib(const std::string& value) {
    std::size_t consumed = 0;
    const auto mib = std::stoull(value, &consumed);
    if (consumed != value.size() || mib == 0 ||
        mib > std::numeric_limits<std::uint64_t>::max() / (1024ULL * 1024ULL)) {
        throw std::invalid_argument("cache budget MiB must be a positive integer");
    }
    return mib * 1024ULL * 1024ULL;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <upi-directory> <meta-path> <coverage-floor>"
                     " <cache-budget-mib>\n";
        return 2;
    }
    try {
        const auto begin = std::chrono::steady_clock::now();
        const auto universal = hinscan::UniversalPathIndex::load(argv[1]);
        if (!universal.has_base_relation_index()) {
            throw std::invalid_argument(
                "path promotion requires a UPI v6 base relation index");
        }
        if (universal.find(argv[2]) != nullptr) {
            throw std::invalid_argument("meta-path is already precomputed in UPI");
        }
        const auto floor = hinscan::SimilarityThreshold::parse(argv[3]);
        bool configured_floor = false;
        for (const auto& candidate : universal.selective_coverage_floors()) {
            configured_floor = configured_floor ||
                               equal_threshold(candidate, floor);
        }
        if (!configured_floor) {
            throw std::invalid_argument(
                "promotion floor is not listed in the UPI coverage candidates");
        }
        const auto budget_bytes = parse_budget_mib(argv[4]);
        const auto factor_file = universal.cached_factor_file(argv[2]);
        const auto selective_file =
            universal.cached_selective_file(argv[2], floor);
        const auto cache_directory = factor_file.parent_path();
        std::filesystem::create_directories(cache_directory);
        for (const auto& candidate : universal.selective_coverage_floors()) {
            if (equal_threshold(candidate, floor)) { continue; }
            if (std::filesystem::exists(
                    universal.cached_selective_file(argv[2], candidate))) {
                throw std::invalid_argument(
                    "this path already has a T-SECI at another floor;"
                    " duplicate threshold layers are disabled");
            }
        }
        if (std::filesystem::exists(factor_file) &&
            std::filesystem::exists(selective_file)) {
            const auto cache_bytes = directory_bytes(cache_directory);
            if (cache_bytes > budget_bytes) {
                throw std::runtime_error(
                    "existing adaptive cache exceeds the requested budget");
            }
            std::cout << "meta_path=" << argv[2] << '\n'
                      << "coverage_floor=" << floor.numerator << '/'
                      << floor.denominator << '\n'
                      << "promotion_status=already_present\n"
                      << "factor_file_bytes="
                      << std::filesystem::file_size(factor_file) << '\n'
                      << "tseci_file_bytes="
                      << std::filesystem::file_size(selective_file) << '\n'
                      << "adaptive_cache_bytes=" << cache_bytes << '\n'
                      << "cache_budget_bytes=" << budget_bytes << '\n';
            return 0;
        }
        const auto staging = cache_directory / ".staging-promotion";
        if (std::filesystem::exists(staging)) {
            std::filesystem::remove_all(staging);
        }
        std::filesystem::create_directories(staging);

        const auto path = universal.parse_symmetric_meta_path(argv[2]);
        const auto load_begin = std::chrono::steady_clock::now();
        hinscan::FactorIndex factor;
        std::uint64_t base_load_ms = 0;
        if (std::filesystem::exists(factor_file)) {
            factor = hinscan::FactorIndex::load(factor_file);
        } else {
            const auto graph =
                hinscan::HinGraph::load_binary(universal.base_relation_file());
            const auto base_loaded = std::chrono::steady_clock::now();
            base_load_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    base_loaded - load_begin).count());
            factor = hinscan::FactorIndex::build(graph, path);
        }
        const auto factor_ready = std::chrono::steady_clock::now();
        auto selective =
            hinscan::SelectiveEquivalenceIndex::build(factor, floor);
        const auto selective_ready = std::chrono::steady_clock::now();

        const auto staged_factor = staging / "path.fli";
        const auto staged_selective = staging / "path.seci";
        std::uint64_t added_bytes = 0;
        if (!std::filesystem::exists(factor_file)) {
            factor.save(staged_factor);
            added_bytes += std::filesystem::file_size(staged_factor);
        }
        if (!std::filesystem::exists(selective_file)) {
            selective.save(staged_selective);
            added_bytes += std::filesystem::file_size(staged_selective);
        }
        const auto existing_bytes = directory_bytes(cache_directory) -
            directory_bytes(staging);
        if (added_bytes > budget_bytes ||
            existing_bytes > budget_bytes - added_bytes) {
            std::filesystem::remove_all(staging);
            throw std::runtime_error(
                "promotion exceeds the adaptive cache budget");
        }
        if (std::filesystem::exists(staged_factor)) {
            std::filesystem::rename(staged_factor, factor_file);
        }
        if (std::filesystem::exists(staged_selective)) {
            std::filesystem::rename(staged_selective, selective_file);
        }
        std::filesystem::remove_all(staging);
        const auto end = std::chrono::steady_clock::now();

        std::cout << "meta_path=" << argv[2] << '\n'
                  << "coverage_floor=" << floor.numerator << '/'
                  << floor.denominator << '\n'
                  << "base_relation_load_ms=" << base_load_ms << '\n'
                  << "factor_build_or_load_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         factor_ready - load_begin).count() << '\n'
                  << "tseci_build_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         selective_ready - factor_ready).count() << '\n'
                  << "factor_file_bytes="
                  << std::filesystem::file_size(factor_file) << '\n'
                  << "tseci_file_bytes="
                  << std::filesystem::file_size(selective_file) << '\n'
                  << "adaptive_cache_bytes="
                  << directory_bytes(cache_directory) << '\n'
                  << "cache_budget_bytes=" << budget_bytes << '\n'
                  << "promotion_total_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         end - begin).count() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "upi_promote_path: " << error.what() << '\n';
        return 1;
    }
}
