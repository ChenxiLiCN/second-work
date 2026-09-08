#ifndef HINSCAN_SELECTIVE_EQUIVALENCE_INDEX_H
#define HINSCAN_SELECTIVE_EQUIVALENCE_INDEX_H

#include "index/FactorIndex.h"
#include "scan/PscanOnFli.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace hinscan {

struct SelectiveEquivalenceCertificate {
    VertexId left_class = 0;
    VertexId right_class = 0;
    std::uint32_t common_neighbors = 0;
};

struct SelectiveEquivalenceIndexStats {
    std::uint64_t vertices = 0;
    std::uint64_t classes = 0;
    std::uint64_t projected_edges = 0;
    std::uint64_t quotient_edges = 0;
    std::uint64_t degree_candidate_edges = 0;
    std::uint64_t retained_certificates = 0;
    std::uint64_t build_milliseconds = 0;
};

struct SelectiveEquivalenceQueryResult {
    PscanOnFliResult clustering;
    std::uint64_t similar_class_edges = 0;
    std::uint64_t certificates_skipped = 0;
};

class VertexMemberRange {
public:
    VertexMemberRange(const VertexId* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    const VertexId* begin() const noexcept { return data_; }
    const VertexId* end() const noexcept { return data_ + size_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    VertexId front() const { return data_[0]; }

private:
    const VertexId* data_ = nullptr;
    std::size_t size_ = 0;
};

class SelectiveEquivalenceIndex {
public:
    static SelectiveEquivalenceIndex build(
        const FactorIndex& factor,
        const SimilarityThreshold& coverage_floor);
    static SelectiveEquivalenceIndex build_from_materialized_graph(
        const std::filesystem::path& graph_directory,
        const SimilarityThreshold& coverage_floor);
    static SelectiveEquivalenceIndex load(const std::filesystem::path& file);
    static SelectiveEquivalenceIndex load(
        const std::filesystem::path& file,
        const SimilarityThreshold& query_threshold);
    void save(const std::filesystem::path& file) const;

    const SimilarityThreshold& coverage_floor() const noexcept;
    const SelectiveEquivalenceIndexStats& stats() const noexcept;
    VertexMemberRange members(VertexId class_id) const;
    std::uint32_t degree(VertexId class_id) const;
    const std::vector<SelectiveEquivalenceCertificate>& certificates() const
        noexcept;
    bool covers(const SimilarityThreshold& threshold) const noexcept;
    SelectiveEquivalenceIndex derive_coverage_layer(
        const SimilarityThreshold& higher_floor) const;
    std::uint64_t serialized_bytes() const;
    std::size_t similar_prefix_size(const SimilarityThreshold& threshold) const;

private:
    static SelectiveEquivalenceIndex load_impl(
        const std::filesystem::path& file,
        const SimilarityThreshold* query_threshold);
    bool score_at_least(const SelectiveEquivalenceCertificate& certificate,
                        const SimilarityThreshold& threshold) const;

    SimilarityThreshold coverage_floor_;
    SimilarityThreshold loaded_floor_;
    SelectiveEquivalenceIndexStats stats_;
    std::vector<VertexId> members_flat_;
    std::vector<std::uint64_t> member_offsets_;
    std::vector<std::uint32_t> degrees_;
    std::vector<SelectiveEquivalenceCertificate> certificates_;
};

SelectiveEquivalenceQueryResult run_pscan_on_selective_equivalence_index(
    const SelectiveEquivalenceIndex& index,
    const SimilarityThreshold& threshold,
    std::uint64_t mu);

}  // namespace hinscan

#endif
