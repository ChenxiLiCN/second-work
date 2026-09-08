#ifndef HINSCAN_SIMILARITY_CERTIFICATE_INDEX_H
#define HINSCAN_SIMILARITY_CERTIFICATE_INDEX_H

#include "index/FactorIndex.h"
#include "scan/PscanOnFli.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace hinscan {

struct SimilarityCertificate {
    VertexId left = 0;
    VertexId right = 0;
    std::uint32_t common_neighbors = 0;
};

struct SimilarityCertificateIndexStats {
    std::uint64_t vertices = 0;
    std::uint64_t projected_edges = 0;
    std::uint64_t closed_neighborhood_entries = 0;
    std::uint64_t build_milliseconds = 0;
};

struct SimilarityCertificateQueryResult {
    PscanOnFliResult clustering;
    std::uint64_t similar_edges = 0;
    std::uint64_t certificate_edges_skipped = 0;
};

class SimilarityCertificateIndex {
public:
    static SimilarityCertificateIndex build(const FactorIndex& factor);
    static SimilarityCertificateIndex load(const std::filesystem::path& file);
    void save(const std::filesystem::path& file) const;

    std::uint64_t vertex_count() const noexcept;
    std::uint32_t degree(VertexId vertex) const;
    const std::vector<SimilarityCertificate>& certificates() const noexcept;
    const SimilarityCertificateIndexStats& stats() const noexcept;
    std::uint64_t serialized_bytes() const;

    std::size_t similar_prefix_size(
        const SimilarityThreshold& threshold) const;

private:
    bool score_at_least(const SimilarityCertificate& certificate,
                        const SimilarityThreshold& threshold) const;

    std::vector<std::uint32_t> degrees_;
    std::vector<SimilarityCertificate> certificates_;
    SimilarityCertificateIndexStats stats_;
};

SimilarityCertificateQueryResult run_pscan_on_similarity_certificates(
    const SimilarityCertificateIndex& index,
    const SimilarityThreshold& threshold,
    std::uint64_t mu);

}  // namespace hinscan

#endif
