#ifndef HINSCAN_BUDGETED_SIMILARITY_INDEX_H
#define HINSCAN_BUDGETED_SIMILARITY_INDEX_H

#include "index/FactorIndex.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace hinscan {

struct BudgetedSimilarityEntry {
    std::uint64_t edge_key = 0;
    std::uint32_t common_neighbors = 0;
};

struct BudgetedSimilarityBuildStats {
    std::uint64_t candidate_edges = 0;
    std::uint64_t retained_edges = 0;
    std::uint64_t temporary_neighborhood_bytes = 0;
    std::uint64_t build_milliseconds = 0;
};

// Stores exact common-neighbor counts for only a structurally selected,
// budget-limited subset of projected edges.  Selection uses estimated FLI scan
// cost and is independent of epsilon and mu, so one file serves every query.
class BudgetedSimilarityIndex {
public:
    static BudgetedSimilarityIndex build(const FactorIndex& factor,
                                          std::uint64_t maximum_entries);
    static BudgetedSimilarityIndex load(
        const std::filesystem::path& index_file);

    void save(const std::filesystem::path& index_file) const;
    bool find(VertexId left, VertexId right,
              std::uint32_t* common_neighbors) const;

    std::uint64_t vertex_count() const noexcept;
    std::uint64_t entry_count() const noexcept;
    std::uint64_t estimated_bytes() const noexcept;
    const BudgetedSimilarityBuildStats& stats() const noexcept;

private:
    std::uint64_t vertex_count_ = 0;
    std::vector<BudgetedSimilarityEntry> entries_;
    BudgetedSimilarityBuildStats stats_;
};

}  // namespace hinscan

#endif
