#ifndef HINSCAN_BUDGETED_EQUIVALENCE_INDEX_H
#define HINSCAN_BUDGETED_EQUIVALENCE_INDEX_H

#include "index/BudgetedSimilarityIndex.h"
#include "index/FactorIndex.h"
#include "scan/PscanOnFli.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace hinscan {

struct BudgetedEquivalenceStats {
    std::uint64_t vertices = 0;
    std::uint64_t classes = 0;
    std::uint64_t projected_edges = 0;
    std::uint64_t quotient_edges = 0;
    std::uint64_t retained_counts = 0;
    std::uint64_t build_milliseconds = 0;
};

struct VertexRange {
    const VertexId* first = nullptr;
    const VertexId* last = nullptr;
    const VertexId* begin() const noexcept { return first; }
    const VertexId* end() const noexcept { return last; }
    std::size_t size() const noexcept {
        return static_cast<std::size_t>(last - first);
    }
};

class BudgetedEquivalenceIndex {
public:
    static BudgetedEquivalenceIndex build(const FactorIndex& factor,
                                           std::uint64_t maximum_count_entries);
    static BudgetedEquivalenceIndex load(
        const std::filesystem::path& index_file);
    void save(const std::filesystem::path& index_file) const;

    VertexRange members(VertexId class_id) const;
    VertexRange neighbors(VertexId class_id) const;
    VertexId representative(VertexId class_id) const;
    std::uint32_t degree(VertexId class_id) const;
    bool find_count(VertexId left_class, VertexId right_class,
                    std::uint32_t* common_neighbors) const;
    std::uint64_t estimated_bytes() const noexcept;
    const BudgetedEquivalenceStats& stats() const noexcept;

private:
    std::vector<std::uint32_t> degrees_;
    std::vector<VertexId> representatives_;
    std::vector<std::uint64_t> member_offsets_;
    std::vector<VertexId> members_flat_;
    std::vector<std::uint64_t> adjacency_offsets_;
    std::vector<VertexId> neighbors_flat_;
    std::vector<BudgetedSimilarityEntry> counts_;
    BudgetedEquivalenceStats stats_;
};

struct BudgetedEquivalenceQueryResult {
    PscanOnFliResult clustering;
    std::uint64_t class_edges_considered = 0;
    std::uint64_t exact_class_checks = 0;
    std::uint64_t indexed_count_hits = 0;
    std::uint64_t fli_fallback_checks = 0;
};

BudgetedEquivalenceQueryResult run_pscan_on_budgeted_equivalence(
    const BudgetedEquivalenceIndex& index,
    const FactorIndex& factor,
    const SimilarityThreshold& threshold,
    std::uint64_t mu);

}  // namespace hinscan

#endif
