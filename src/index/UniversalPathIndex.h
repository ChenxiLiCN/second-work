#ifndef HINSCAN_UNIVERSAL_PATH_INDEX_H
#define HINSCAN_UNIVERSAL_PATH_INDEX_H

#include "hin/HinGraph.h"
#include "index/FactorIndex.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace hinscan {

struct UniversalPathEntry {
    std::vector<std::uint32_t> half_path;
    std::filesystem::path relative_file;
    std::uint64_t file_bytes = 0;
    std::uint64_t half_path_incidences = 0;
    std::uint64_t projected_edges = 0;
    std::uint64_t build_milliseconds = 0;
};

struct UniversalPathIndexStats {
    std::uint32_t max_half_length = 0;
    std::uint64_t path_entries = 0;
    std::uint64_t total_file_bytes = 0;
    std::uint64_t total_half_path_incidences = 0;
    std::uint64_t equivalence_entries = 0;
    std::uint64_t equivalence_file_bytes = 0;
    std::uint64_t certificate_entries = 0;
    std::uint64_t certificate_file_bytes = 0;
    std::uint32_t certificate_budget_percent = 0;
    std::uint64_t selective_entries = 0;
    std::uint64_t selective_file_bytes = 0;
    std::uint64_t selective_coverage_numerator = 9;
    std::uint64_t selective_coverage_denominator = 10;
    std::uint64_t base_relation_file_bytes = 0;
    std::uint64_t precomputed_path_limit = 64;
    std::uint64_t build_milliseconds = 0;
};

class UniversalPathIndex {
public:
    static UniversalPathIndex build(const HinGraph& graph,
                                    const std::filesystem::path& index_directory,
                                    std::uint32_t max_half_length,
                                    std::uint32_t certificate_budget_percent = 100,
                                    std::vector<SimilarityThreshold>
                                        selective_coverage_floors = {{9, 10}},
                                    std::uint64_t max_precomputed_paths = 64);
    static UniversalPathIndex load(
        const std::filesystem::path& index_directory);

    const UniversalPathEntry& resolve(const std::string& meta_path) const;
    const UniversalPathEntry* find(const std::string& meta_path) const;
    std::vector<std::uint32_t> parse_symmetric_meta_path(
        const std::string& meta_path) const;
    std::filesystem::path entry_file(const UniversalPathEntry& entry) const;
    std::filesystem::path selective_file(
        const UniversalPathEntry& entry,
        const SimilarityThreshold& coverage_floor) const;
    bool has_base_relation_index() const noexcept;
    std::filesystem::path base_relation_file() const;
    std::filesystem::path cached_factor_file(
        const std::string& meta_path) const;
    std::filesystem::path cached_selective_file(
        const std::string& meta_path,
        const SimilarityThreshold& coverage_floor) const;

    const std::vector<VertexType>& vertex_types() const noexcept;
    const std::vector<UniversalPathEntry>& entries() const noexcept;
    const UniversalPathIndexStats& stats() const noexcept;
    const std::vector<SimilarityThreshold>& selective_coverage_floors() const
        noexcept;
    const std::filesystem::path& directory() const noexcept;

private:
    void save_manifest() const;
    std::uint32_t type_id(const std::string& token) const;

    std::filesystem::path directory_;
    std::vector<VertexType> vertex_types_;
    std::vector<UniversalPathEntry> entries_;
    std::vector<SimilarityThreshold> selective_coverage_floors_;
    std::filesystem::path base_relation_relative_file_;
    UniversalPathIndexStats stats_;
};

}  // namespace hinscan

#endif
