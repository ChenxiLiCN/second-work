#ifndef HINSCAN_EQUIVALENCE_INDEX_H
#define HINSCAN_EQUIVALENCE_INDEX_H

#include "index/FactorIndex.h"
#include "scan/PscanOnFli.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace hinscan {

struct EquivalenceEdge {
    VertexId neighbor_class = 0;
    std::uint64_t common_neighbors = 0;
};

struct EquivalenceIndexStats {
    std::uint64_t vertices = 0;
    std::uint64_t classes = 0;
    std::uint64_t projected_edges = 0;
    std::uint64_t quotient_edges = 0;
    std::uint64_t build_milliseconds = 0;
};

struct EquivalenceQueryResult {
    PscanOnFliResult clustering;
    std::uint64_t quotient_edges_checked = 0;
    std::uint64_t similar_quotient_edges = 0;
};

class EquivalenceIndex {
public:
    static EquivalenceIndex build(const FactorIndex& factor);
    static EquivalenceIndex load(const std::filesystem::path& file);
    void save(const std::filesystem::path& file) const;

    const std::vector<VertexId>& members(VertexId class_id) const;
    std::uint64_t degree(VertexId class_id) const;
    const std::vector<EquivalenceEdge>& neighbors(VertexId class_id) const;
    const EquivalenceIndexStats& stats() const noexcept;

private:
    std::vector<std::vector<VertexId>> members_;
    std::vector<std::uint64_t> degrees_;
    std::vector<std::vector<EquivalenceEdge>> adjacency_;
    EquivalenceIndexStats stats_;
};

EquivalenceQueryResult run_pscan_on_equivalence_index(
    const EquivalenceIndex& index,
    const SimilarityThreshold& threshold,
    std::uint64_t mu);

}  // namespace hinscan

#endif
