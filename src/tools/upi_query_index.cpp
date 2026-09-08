#include "index/EquivalenceIndex.h"
#include "index/FactorIndex.h"
#include "index/SimilarityCertificateIndex.h"
#include "index/SelectiveEquivalenceIndex.h"
#include "index/UniversalPathIndex.h"
#include "scan/PscanOnFli.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using Wide = unsigned __int128;

void print_usage(const char* executable) {
    std::cerr << "Usage: " << executable
              << " <upi-directory> <meta-path> <epsilon> <mu>"
                 " <output-directory> [neighborhood-cache-mib]\n";
}

std::uint64_t parse_mu(const std::string& value) {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed);
    if (consumed != value.size() || parsed == 0) {
        throw std::invalid_argument("mu must be a positive integer");
    }
    return parsed;
}

bool threshold_at_least(const hinscan::SimilarityThreshold& threshold,
                        std::uint64_t numerator,
                        std::uint64_t denominator) {
    return static_cast<Wide>(threshold.numerator) * denominator >=
           static_cast<Wide>(numerator) * threshold.denominator;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6 && argc != 7) {
        print_usage(argv[0]);
        return 2;
    }
    try {
        const std::filesystem::path upi_directory = argv[1];
        const std::string meta_path = argv[2];
        const std::string epsilon_text = argv[3];
        const auto mu = parse_mu(argv[4]);
        const std::filesystem::path output_directory = argv[5];
        const auto cache_mib = argc == 7 ? std::stoull(argv[6]) : 32;
        if (cache_mib > std::numeric_limits<std::uint64_t>::max() /
                            (1024ULL * 1024ULL)) {
            throw std::overflow_error("neighborhood cache size is too large");
        }
        const auto cache_bytes = cache_mib * 1024ULL * 1024ULL;

        const auto manifest_begin = std::chrono::steady_clock::now();
        const auto universal =
            hinscan::UniversalPathIndex::load(upi_directory);
        const auto meta_path_types =
            universal.parse_symmetric_meta_path(meta_path);
        const auto* entry = universal.find(meta_path);
        const auto manifest_end = std::chrono::steady_clock::now();
        const auto threshold = hinscan::SimilarityThreshold::parse(epsilon_text);
        std::filesystem::path factor_file;
        std::filesystem::path equivalence_file;
        std::filesystem::path certificate_file;
        std::filesystem::path legacy_selective_file;
        if (entry != nullptr) {
            factor_file = universal.entry_file(*entry);
            equivalence_file = factor_file;
            equivalence_file.replace_extension(".eqi");
            certificate_file = factor_file;
            certificate_file.replace_extension(".sci");
            legacy_selective_file = factor_file;
            legacy_selective_file.replace_extension(".seci");
        } else {
            factor_file = universal.cached_factor_file(meta_path);
            certificate_file = factor_file;
            certificate_file.replace_extension(".sci");
        }
        bool has_selective = false;
        std::optional<std::filesystem::path> selected_selective_file;
        std::optional<hinscan::SimilarityThreshold> selected_selective_floor;
        for (const auto& floor : universal.selective_coverage_floors()) {
            const auto candidate = entry != nullptr
                ? universal.selective_file(*entry, floor)
                : universal.cached_selective_file(meta_path, floor);
            if (!std::filesystem::exists(candidate)) { continue; }
            has_selective = true;
            if (threshold_at_least(threshold, floor.numerator,
                                   floor.denominator)) {
                selected_selective_file = candidate;
                selected_selective_floor = floor;
            }
        }
        if (entry != nullptr &&
            std::filesystem::exists(legacy_selective_file)) {
            has_selective = true;
            const hinscan::SimilarityThreshold legacy_floor{
                universal.stats().selective_coverage_numerator,
                universal.stats().selective_coverage_denominator};
            if (threshold_at_least(threshold, legacy_floor.numerator,
                                   legacy_floor.denominator) &&
                (!selected_selective_floor ||
                 threshold_at_least(legacy_floor,
                                    selected_selective_floor->numerator,
                                    selected_selective_floor->denominator))) {
                selected_selective_file = legacy_selective_file;
                selected_selective_floor = legacy_floor;
            }
        }
        const auto index_begin = manifest_end;
        auto index_end = index_begin;
        std::string index_mode;
        std::filesystem::path resolved_file;
        hinscan::PscanOnFliResult result;
        std::uint64_t equivalence_classes = 0;
        std::uint64_t quotient_edges = 0;
        std::uint64_t quotient_edges_checked = 0;
        std::uint64_t similar_certificate_edges = 0;
        std::uint64_t certificate_edges_skipped = 0;
        std::uint64_t selective_classes = 0;
        std::uint64_t selective_certificates = 0;
        std::uint64_t selective_certificates_loaded = 0;
        std::uint64_t base_relation_load_ms = 0;
        std::uint64_t on_demand_fli_build_ms = 0;
        if (entry != nullptr && std::filesystem::exists(equivalence_file)) {
            index_mode = "equivalence_quotient";
            resolved_file = equivalence_file;
            const auto equivalence =
                hinscan::EquivalenceIndex::load(equivalence_file);
            index_end = std::chrono::steady_clock::now();
            auto quotient = hinscan::run_pscan_on_equivalence_index(
                equivalence, threshold, mu);
            equivalence_classes = equivalence.stats().classes;
            quotient_edges = equivalence.stats().quotient_edges;
            quotient_edges_checked = quotient.quotient_edges_checked;
            result = std::move(quotient.clustering);
        } else if (selected_selective_file) {
            const auto selective =
                hinscan::SelectiveEquivalenceIndex::load(
                    *selected_selective_file, threshold);
            if (!selective.covers(threshold)) {
                throw std::runtime_error(
                    "UPI manifest and SECI coverage floor disagree");
            }
            index_mode = "selective_equivalence_certificates";
            resolved_file = *selected_selective_file;
            index_end = std::chrono::steady_clock::now();
            auto selective_query =
                hinscan::run_pscan_on_selective_equivalence_index(
                    selective, threshold, mu);
            selective_classes = selective.stats().classes;
            selective_certificates =
                selective.stats().retained_certificates;
            selective_certificates_loaded = selective.certificates().size();
            similar_certificate_edges =
                selective_query.similar_class_edges;
            certificate_edges_skipped =
                selective_query.certificates_skipped;
            result = std::move(selective_query.clustering);
        } else if (entry != nullptr &&
                   std::filesystem::exists(certificate_file)) {
            index_mode = "similarity_certificates";
            resolved_file = certificate_file;
            const auto certificates =
                hinscan::SimilarityCertificateIndex::load(certificate_file);
            index_end = std::chrono::steady_clock::now();
            auto certificate_query =
                hinscan::run_pscan_on_similarity_certificates(
                    certificates, threshold, mu);
            similar_certificate_edges = certificate_query.similar_edges;
            certificate_edges_skipped =
                certificate_query.certificate_edges_skipped;
            result = std::move(certificate_query.clustering);
        } else if (std::filesystem::exists(factor_file)) {
            if (entry != nullptr) {
                index_mode = has_selective ? "factor_index_below_seci_floor"
                                           : "factor_index";
            } else {
                index_mode = has_selective
                    ? "cached_factor_index_below_seci_floor"
                    : "cached_factor_index";
            }
            resolved_file = factor_file;
            const auto factor = hinscan::FactorIndex::load(factor_file);
            index_end = std::chrono::steady_clock::now();
            result = hinscan::run_pscan_on_fli(
                factor, threshold, mu, cache_bytes);
        } else {
            if (!universal.has_base_relation_index()) {
                throw std::invalid_argument(
                    "meta-path is not precomputed and this legacy UPI has no BRI fallback");
            }
            index_mode = "base_relation_on_demand_fli";
            resolved_file = universal.base_relation_file();
            const auto graph = hinscan::HinGraph::load_binary(resolved_file);
            const auto base_loaded = std::chrono::steady_clock::now();
            const auto factor =
                hinscan::FactorIndex::build(graph, meta_path_types);
            index_end = std::chrono::steady_clock::now();
            base_relation_load_ms = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    base_loaded - index_begin).count());
            on_demand_fli_build_ms = factor.stats().build_milliseconds;
            result = hinscan::run_pscan_on_fli(
                factor, threshold, mu, cache_bytes);
        }
        const auto output_begin = std::chrono::steady_clock::now();
        hinscan::write_pscan_on_fli_results(
            output_directory, epsilon_text, mu, result);
        const auto output_end = std::chrono::steady_clock::now();

        const auto manifest_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     manifest_end - manifest_begin)
                                     .count();
        const auto index_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  index_end - index_begin)
                                  .count();
        const auto output_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                output_end - output_begin)
                .count();
        const auto online_with_output_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                output_end - manifest_begin)
                .count();
        const auto& stats = result.stats;
        std::cout << "upi_directory=" << universal.directory().string() << '\n'
                  << "meta_path=" << meta_path << '\n'
                  << "precomputed_path="
                  << (entry != nullptr ? "yes" : "no") << '\n'
                  << "index_mode=" << index_mode << '\n'
                  << "resolved_index_file=" << resolved_file.string() << '\n'
                  << "epsilon=" << epsilon_text << '\n'
                  << "mu=" << mu << '\n'
                  << "manifest_lookup_ms=" << manifest_ms << '\n'
                  << "index_load_ms=" << index_ms << '\n'
                  << "factor_load_ms=" << index_ms << '\n'
                  << "base_relation_load_ms=" << base_relation_load_ms << '\n'
                  << "on_demand_fli_build_ms=" << on_demand_fli_build_ms
                  << '\n'
                  << "online_total_ms="
                  << manifest_ms + index_ms + stats.query_milliseconds << '\n'
                  << "output_write_ms=" << output_ms << '\n'
                  << "online_with_output_ms=" << online_with_output_ms << '\n'
                  << "query_ms=" << stats.query_milliseconds << '\n'
                  << "equivalence_classes=" << equivalence_classes << '\n'
                  << "quotient_edges=" << quotient_edges << '\n'
                  << "quotient_edges_checked=" << quotient_edges_checked << '\n'
                  << "similar_certificate_edges="
                  << similar_certificate_edges << '\n'
                  << "certificate_edges_skipped="
                  << certificate_edges_skipped << '\n'
                  << "selective_classes=" << selective_classes << '\n'
                  << "selective_certificates="
                  << selective_certificates << '\n'
                  << "selective_certificates_loaded="
                  << selective_certificates_loaded << '\n'
                  << "selected_selective_floor=";
        if (selected_selective_floor) {
            std::cout << selected_selective_floor->numerator << '/'
                      << selected_selective_floor->denominator;
        } else {
            std::cout << "none";
        }
        std::cout << '\n'
                  << "projected_edges="
                  << stats.projected_edges_seen_in_prune << '\n'
                  << "degree_pruned_edges=" << stats.degree_pruned_edges << '\n'
                  << "degree_compatible_edges="
                  << stats.degree_compatible_edges_enumerated << '\n'
                  << "exact_similarity_checks="
                  << stats.exact_similarity_checks << '\n'
                  << "full_neighborhood_generations="
                  << stats.neighborhood_generations << '\n'
                  << "degree_bucket_posting_entries_read="
                  << stats.degree_bucket_posting_entries_read << '\n'
                  << "degree_bucket_posting_entries_skipped="
                  << stats.degree_bucket_posting_entries_skipped << '\n'
                  << "core_vertices=" << stats.core_vertices << '\n'
                  << "clusters=" << stats.clusters << '\n'
                  << "border_vertices=" << stats.border_vertices << '\n'
                  << "hub_vertices=" << stats.hub_vertices << '\n'
                  << "outlier_vertices=" << stats.outlier_vertices << '\n'
                  << "output=" << output_directory.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "upi_query_index: " << error.what() << '\n';
        return 1;
    }
}
