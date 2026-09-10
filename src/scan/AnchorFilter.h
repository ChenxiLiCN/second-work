#ifndef HINSCAN_ANCHOR_FILTER_H
#define HINSCAN_ANCHOR_FILTER_H

#include "index/FactorIndex.h"
#include <vector>

namespace hinscan {

struct AnchorFilterStats {
    std::uint64_t calls=0, ineligible=0, no_anchor=0, eligible=0;
    std::uint64_t rejects=0, fallbacks=0, same_anchor=0;
    std::uint64_t cache_hits=0, cache_misses=0, replacements=0;
    std::uint64_t resumed=0, cached_rejects=0, cached_fallbacks=0;
    std::uint64_t comparisons=0, entries_advanced=0;
    std::uint64_t selection_entries=0, anchored_vertices=0;
    std::uint64_t coverage_55_vertices=0, coverage_75_vertices=0;
    std::uint64_t mapping_bytes=0, state_bytes=0;
    double prepare_ms=0, filter_ms=0;
};

// Query-local rejection only. No projected edges, neighborhood unions or
// similarity matrix are stored. A false result means UNKNOWN, never similar.
class AnchorFilter {
public:
    explicit AnchorFilter(const FactorIndex& index, std::uint64_t state_budget);
    bool reject(VertexId left, VertexId right, std::uint64_t required_common);
    const AnchorFilterStats& stats() const { return stats_; }
private:
    static constexpr VertexId missing=~VertexId{0};
    struct Progress {
        std::uint64_t key=~std::uint64_t{0};
        std::uint64_t left_pos=0, right_pos=0, common=0;
    };
    const FactorIndex& index_;
    std::vector<VertexId> anchors_;
    std::vector<Progress> states_;
    AnchorFilterStats stats_;
};
}
#endif
