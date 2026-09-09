#ifndef HINSCAN_FACTOR_INDEX_H
#define HINSCAN_FACTOR_INDEX_H

#include "hin/HinGraph.h"

#include <cstdint>
#include <filesystem>
#include <queue>
#include <string>
#include <vector>

namespace hinscan {

class LazyUnionCursor {
public:
    LazyUnionCursor(VertexId self,
                    const std::vector<VertexId>& witnesses,
                    const std::vector<std::vector<VertexId>>& postings);

    bool next(VertexId* value);
    std::uint64_t posting_entries_read() const noexcept;

private:
    struct HeapEntry {
        VertexId value = 0;
        VertexId witness = 0;
        std::size_t offset = 0;
    };

    struct Greater {
        bool operator()(const HeapEntry& left, const HeapEntry& right) const;
    };

    VertexId self_ = 0;
    bool self_pending_ = true;
    const std::vector<std::vector<VertexId>>* postings_ = nullptr;
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, Greater> heap_;
    std::uint64_t posting_entries_read_ = 0;
};

struct FactorIndexStats {
    std::uint64_t target_vertices = 0;
    std::uint64_t center_vertices = 0;
    std::uint64_t half_path_incidences = 0;
    std::uint64_t exact_projected_edges = 0;
    std::uint64_t degree_merge_entries_read = 0;
    std::uint64_t estimated_index_bytes = 0;
    std::uint64_t build_milliseconds = 0;
    // Fresh-build instrumentation only; the stable FLI file format is unchanged.
    double half_expansion_ms = 0, degree_compute_ms = 0, posting_order_ms = 0;
    std::uint64_t half_expansion_entries = 0;
    bool used_roundtrip_metadata = false;
};

struct SimilarityThreshold {
    std::uint64_t numerator = 0;
    std::uint64_t denominator = 1;

    static SimilarityThreshold parse(const std::string& value);
    std::uint64_t required_common_neighbors(std::uint64_t left_degree,
                                            std::uint64_t right_degree) const;
    bool fails_degree_ratio(std::uint64_t left_degree,
                             std::uint64_t right_degree) const;
    bool certifies_common(std::uint64_t common, std::uint64_t left_degree,
                          std::uint64_t right_degree) const;
};

struct SimilarityCheck {
    bool similar = false;
    bool degree_pruned = false;
    std::uint64_t common_neighbors = 0;
    std::uint64_t required_common_neighbors = 0;
    std::uint64_t posting_entries_read = 0;
};

class FactorIndex {
public:
    static FactorIndex build(const HinGraph& graph,
                             const std::vector<std::uint32_t>& meta_path);
    static FactorIndex load(const std::filesystem::path& index_file);

    void save(const std::filesystem::path& index_file) const;

    std::uint64_t vertex_count() const noexcept;
    std::uint64_t center_count() const noexcept;
    std::uint64_t degree(VertexId vertex) const;
    const std::vector<VertexId>& witnesses(VertexId vertex) const;
    const std::vector<VertexId>& posting(VertexId center) const;
    const std::vector<VertexId>& degree_ordered_posting(VertexId center) const;
    LazyUnionCursor closed_neighborhood(VertexId vertex) const;
    std::vector<VertexId> collect_closed_neighborhood(
        VertexId vertex,
        std::uint64_t* posting_entries_read = nullptr) const;
    SimilarityCheck check_similarity(VertexId left,
                                     VertexId right,
                                     const SimilarityThreshold& threshold) const;
    SimilarityCheck check_similarity_with_left_neighborhood(
        VertexId left,
        const std::vector<VertexId>& left_neighborhood,
        VertexId right,
        const SimilarityThreshold& threshold) const;
    const FactorIndexStats& stats() const noexcept;

private:
    std::vector<std::vector<VertexId>> witnesses_;
    std::vector<std::vector<VertexId>> postings_;
    std::vector<std::vector<VertexId>> degree_ordered_postings_;
    std::vector<std::uint64_t> degrees_;
    FactorIndexStats stats_;
};

}  // namespace hinscan

#endif
