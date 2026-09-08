#ifndef HINSCAN_FINGERPRINT_NEIGHBORHOOD_INDEX_H
#define HINSCAN_FINGERPRINT_NEIGHBORHOOD_INDEX_H

#include "index/FactorIndex.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace hinscan {

struct FingerprintBoundResult {
    bool may_reach_required = true;
    std::uint64_t fingerprint_matches = 0;
    std::uint64_t entries_read = 0;
};

// A threshold-independent upper-bound index.  Each closed neighborhood is
// represented by the sorted multiset of 16-bit fingerprints of its vertices.
// For two neighborhoods, the multiset intersection of fingerprints is always
// at least their true intersection: equal vertices always have equal
// fingerprints, while collisions can only add matches.  Therefore an upper
// bound below the query's required common-neighbor count proves dissimilarity
// without storing an edge similarity or depending on epsilon.
class FingerprintNeighborhoodIndex {
public:
    static FingerprintNeighborhoodIndex build(const FactorIndex& factor);
    static FingerprintNeighborhoodIndex load(
        const std::filesystem::path& index_file);

    void save(const std::filesystem::path& index_file) const;

    std::uint64_t vertex_count() const noexcept;
    std::uint64_t fingerprint_count() const noexcept;
    std::uint64_t neighborhood_size(VertexId vertex) const;
    std::uint64_t estimated_bytes() const noexcept;

    FingerprintBoundResult check_upper_bound(
        VertexId left,
        VertexId right,
        std::uint64_t required_common_neighbors) const;

private:
    static constexpr std::size_t kWordCount = 32;
    std::vector<std::uint32_t> degrees_;
    std::vector<std::uint32_t> maximum_bucket_loads_;
    std::vector<std::uint64_t> occupancy_words_;
    std::vector<std::uint64_t> count_offsets_;
    std::vector<std::uint8_t> bucket_counts_;
    std::vector<std::uint8_t> count_overflow_;
    std::uint64_t fingerprint_count_ = 0;
};

}  // namespace hinscan

#endif
