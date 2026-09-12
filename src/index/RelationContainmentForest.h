#ifndef HINSCAN_RELATION_CONTAINMENT_FOREST_H
#define HINSCAN_RELATION_CONTAINMENT_FOREST_H
#include "hin/HinGraph.h"
#include <limits>
#include <utility>

namespace hinscan {
// One original directed relation only. No meta-path, epsilon, or mu.
struct RelationContainmentForest {
    static constexpr VertexId missing = std::numeric_limits<VertexId>::max();
    struct Node {
        VertexId representative = 0, weight = 0, parent = missing;
        VertexId depth = 0, tin = 0, tout = 0;
    };
    std::vector<Node> nodes;
    // Sparse: omit empty source rows, preventing schema-times-isolates storage.
    std::vector<std::pair<VertexId, VertexId>> membership;
    std::uint64_t parent_candidates = 0, subset_comparisons = 0;
    std::uint64_t within_class_pairs = 0, comparable_cross_class_pairs = 0;
    std::uint64_t parent_edges = 0, depth_sum = 0, max_depth = 0;
    double grouping_ms = 0, parent_search_ms = 0, labeling_ms = 0;
    static RelationContainmentForest build(
        const std::vector<std::vector<VertexId>>& rows, std::size_t target_count);
    bool ancestor(VertexId ancestor_class, VertexId descendant_class) const;
};
}
#endif
