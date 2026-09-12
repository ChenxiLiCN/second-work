#include "index/RelationContainmentForest.h"
#include <algorithm>
#include <chrono>
#include <numeric>
#include <stdexcept>

namespace hinscan {
namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
}
RelationContainmentForest RelationContainmentForest::build(
    const std::vector<std::vector<VertexId>>& rows, std::size_t target_count) {
    if (rows.size() >= missing || target_count >= missing)
        throw std::invalid_argument("containment diagnostic requires 32-bit vertex IDs");
    RelationContainmentForest out;
    const auto start = Clock::now();
    std::vector<VertexId> order;
    for (std::size_t u = 0; u < rows.size(); ++u) {
        const auto& row = rows[u];
        if (!std::is_sorted(row.begin(), row.end()) ||
            std::adjacent_find(row.begin(), row.end()) != row.end() ||
            (!row.empty() && row.back() >= target_count))
            throw std::invalid_argument("relation rows must be sorted, unique and in range");
        if (!row.empty()) order.push_back(static_cast<VertexId>(u));
    }
    std::sort(order.begin(), order.end(), [&](VertexId a, VertexId b) {
        if (rows[a] != rows[b]) return rows[a] < rows[b];
        return a < b;
    });
    for (auto u : order) {
        if (out.nodes.empty() || rows[u] != rows[out.nodes.back().representative])
            out.nodes.push_back(Node{u, 0, missing, 0, 0, 0});
        ++out.nodes.back().weight;
        out.membership.emplace_back(u, static_cast<VertexId>(out.nodes.size()-1));
    }
    std::sort(out.membership.begin(), out.membership.end());
    const auto grouped = Clock::now();
    out.grouping_ms = ms(grouped-start);
    const auto q = out.nodes.size();
    std::vector<VertexId> size_order(q);
    std::iota(size_order.begin(), size_order.end(), 0);
    auto size = [&](VertexId c) { return rows[out.nodes[c].representative].size(); };
    std::sort(size_order.begin(), size_order.end(), [&](VertexId a, VertexId b) {
        return size(a) != size(b) ? size(a) < size(b) : a < b;
    });
    // Temporary inverted CLASS postings, not projected target-target edges.
    std::vector<std::vector<VertexId>> inverted(target_count);
    for (auto c : size_order)
        for (auto x : rows[out.nodes[c].representative]) inverted[x].push_back(c);
    for (auto c : size_order) {
        const auto& small = rows[out.nodes[c].representative];
        auto rare = small.front();
        for (auto x : small) if (inverted[x].size() < inverted[rare].size()) rare = x;
        const auto& candidates = inverted[rare];
        auto begin = std::upper_bound(candidates.begin(), candidates.end(), small.size(),
            [&](std::size_t n, VertexId candidate) { return n < size(candidate); });
        for (; begin != candidates.end(); ++begin) {
            ++out.parent_candidates;
            const auto& large = rows[out.nodes[*begin].representative];
            bool subset = true;
            auto pos = large.begin();
            for (auto x : small) {
                pos = std::lower_bound(pos, large.end(), x, [&](VertexId a, VertexId b) {
                    ++out.subset_comparisons;
                    return a < b;
                });
                if (pos == large.end() || *pos != x) { subset = false; break; }
                ++pos;
            }
            if (subset) { out.nodes[c].parent = *begin; ++out.parent_edges; break; }
        }
    }
    const auto searched = Clock::now();
    out.parent_search_ms = ms(searched-grouped);
    std::vector<std::uint64_t> ancestor_weight(q, 0);
    std::vector<VertexId> head(q, missing), next(q, missing);
    for (auto it = size_order.rbegin(); it != size_order.rend(); ++it) {
        const auto c = *it;
        auto& node = out.nodes[c];
        if (node.parent != missing) {
            const auto& parent = out.nodes[node.parent];
            node.depth = parent.depth + 1;
            ancestor_weight[c] = ancestor_weight[node.parent] + parent.weight;
            next[c] = head[node.parent]; head[node.parent] = c;
        }
        out.depth_sum += node.depth;
        out.max_depth = std::max<std::uint64_t>(out.max_depth, node.depth);
        out.within_class_pairs += std::uint64_t(node.weight) * (node.weight-1) / 2;
        out.comparable_cross_class_pairs += std::uint64_t(node.weight) * ancestor_weight[c];
    }
    // Iterative Euler traversal, safe for deep inclusion chains.
    VertexId timer = 0;
    std::vector<std::pair<VertexId, bool>> stack;
    for (VertexId root = 0; root < q; ++root) if (out.nodes[root].parent == missing) {
        stack.emplace_back(root, false);
        while (!stack.empty()) {
            auto entry = stack.back(); stack.pop_back();
            auto& node = out.nodes[entry.first];
            if (entry.second) { node.tout = timer; continue; }
            node.tin = timer++;
            stack.emplace_back(entry.first, true);
            for (auto child = head[entry.first]; child != missing; child = next[child])
                stack.emplace_back(child, false);
        }
    }
    out.labeling_ms = ms(Clock::now()-searched);
    return out;
}
bool RelationContainmentForest::ancestor(VertexId a, VertexId d) const {
    return nodes.at(a).tin <= nodes.at(d).tin && nodes.at(d).tin < nodes.at(a).tout;
}
}
