#include "scan/LayeredCompletion.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace hinscan {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto missing = std::numeric_limits<VertexId>::max();
constexpr auto absent = std::numeric_limits<std::size_t>::max();
constexpr auto max64 = std::numeric_limits<std::uint64_t>::max();

double ms(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

std::uint64_t capped_add(std::uint64_t a, std::uint64_t b, std::uint64_t cap) {
    return a + std::min(b, cap - a);
}
void count(std::uint64_t& value) { if (value != max64) ++value; }

std::size_t checked_size_add(std::size_t a, std::size_t b) {
    if (b > absent - a) throw std::overflow_error("layered workspace size overflow");
    return a + b;
}

// Exact a/b >= c/d, without cross products or compiler-specific wide integers.
// Equal integral parts reduce to reciprocal fractions and reverse the order.
bool ratio_at_least(std::uint64_t a, std::uint64_t b,
                    std::uint64_t c, std::uint64_t d) {
    bool reversed = false;
    for (;;) {
        const auto qa = a / b, qc = c / d;
        if (qa != qc) return reversed ? qa < qc : qa > qc;
        const auto ra = a % b, rc = c % d;
        if (ra == 0 || rc == 0) {
            if (ra == rc) return true;
            return reversed ? ra == 0 : rc == 0;
        }
        a = b; b = ra; c = d; d = rc;
        reversed = !reversed;
    }
}

struct Dsu {
    // A path may revisit a type many times: summed layer sizes are NOT VertexId.
    std::vector<std::size_t> parent, sizes;
    explicit Dsu(std::size_t n = 0) : parent(n), sizes(n, 1) {
        std::iota(parent.begin(), parent.end(), std::size_t{0});
    }
    std::size_t find(std::size_t u) {
        while (parent[u] != u) { parent[u] = parent[parent[u]]; u = parent[u]; }
        return u;
    }
    void unite(std::size_t u, std::size_t v) {
        u = find(u); v = find(v);
        if (u == v) return;
        if (sizes[u] < sizes[v]) std::swap(u, v);
        parent[v] = u; sizes[u] += sizes[v];
    }
};

struct LayerConnection {
    Dsu dsu;
    std::vector<std::vector<bool>> active;
    std::vector<std::size_t> offsets;
};

template<class F>
void visit_group(const MajorityDirection& groups, std::size_t g, F&& f) {
    for (auto j = groups.offsets[g]; j < groups.offsets[g + 1]; ++j)
        f(groups.members[static_cast<std::size_t>(j)]);
}

// A single batched layered traversal, with only group IDs (no lifted lists).
// Both trims are necessary: unreachable members must never bridge certificates.
LayerConnection connect_layers(const std::vector<Transition>& rows,
    const std::vector<std::size_t>& sizes, std::uint64_t& reads,
    const MajorityDirection* groups = nullptr,
    const std::vector<std::size_t>* accepted = nullptr,
    const std::vector<bool>* sources = nullptr, std::uint64_t* member_reads = nullptr) {
    LayerConnection result;
    result.offsets.push_back(0);
    std::vector<std::vector<bool>> forward, backward;
    for (auto n : sizes) {
        result.offsets.push_back(checked_size_add(result.offsets.back(), n));
        forward.emplace_back(n, false); backward.emplace_back(n, false);
    }
    forward[0] = sources ? *sources : std::vector<bool>(sizes[0], true);
    for (std::size_t l = 0; l < rows.size(); ++l)
        for (std::size_t u = 0; u < sizes[l]; ++u) if (forward[l][u])
            for (auto v : rows[l].neighbors(static_cast<VertexId>(u))) {
                count(reads); forward[l + 1][v] = true;
            }
    if (!groups) std::fill(backward.back().begin(), backward.back().end(), true);
    else for (auto g : *accepted) visit_group(*groups, g, [&](VertexId b) {
        count(*member_reads); backward.back()[b] = true;
    });
    for (std::size_t l = rows.size(); l-- > 0;)
        for (std::size_t u = 0; u < sizes[l]; ++u) if (forward[l][u])
            for (auto v : rows[l].neighbors(static_cast<VertexId>(u))) {
                count(reads);
                if (backward[l + 1][v]) backward[l][u] = true;
            }
    result.active = std::move(forward);
    for (std::size_t l = 0; l < sizes.size(); ++l)
        for (std::size_t u = 0; u < sizes[l]; ++u)
            result.active[l][u] = result.active[l][u] && backward[l][u];
    result.dsu = Dsu(checked_size_add(result.offsets.back(), accepted ? accepted->size() : 0));
    for (std::size_t l = 0; l < rows.size(); ++l)
        for (std::size_t u = 0; u < sizes[l]; ++u) if (result.active[l][u])
            for (auto v : rows[l].neighbors(static_cast<VertexId>(u))) {
                count(reads);
                if (result.active[l + 1][v])
                    result.dsu.unite(result.offsets[l] + u, result.offsets[l + 1] + v);
            }
    if (groups) for (std::size_t i = 0; i < accepted->size(); ++i)
        visit_group(*groups, (*accepted)[i], [&](VertexId b) {
            count(*member_reads);
            if (result.active.back()[b])
                result.dsu.unite(result.offsets[sizes.size() - 1] + b,
                                 result.offsets.back() + i);
        });
    return result;
}

std::vector<std::uint64_t> walk_upper(const std::vector<Transition>& rows,
    const std::vector<std::size_t>& sizes, std::uint64_t cap, std::uint64_t& reads) {
    std::vector<std::uint64_t> values(sizes.back(), std::min<std::uint64_t>(1, cap));
    for (std::size_t l = rows.size(); l-- > 0;) {
        std::vector<std::uint64_t> previous(sizes[l], 0);
        // Intentionally scan the full raw relation, including completed components.
        for (std::size_t u = 0; u < sizes[l]; ++u)
            for (auto v : rows[l].neighbors(static_cast<VertexId>(u))) {
                count(reads); previous[u] = capped_add(previous[u], values[v], cap);
            }
        values = std::move(previous);
    }
    return values;
}

void prove_components(const HinGraph& graph, const MajorityIndex& index,
    const std::vector<Transition>& full_rows, const std::vector<std::size_t>& full_sizes,
    const SimilarityThreshold& eps, std::uint64_t mu, LayeredCompletionStats& st,
    std::vector<bool>& keep, std::vector<VertexId>& resolved) {
    const auto half_length = full_rows.size() / 2;
    const std::vector<Transition> rows(full_rows.begin(), full_rows.begin() + half_length);
    const std::vector<std::size_t> sizes(full_sizes.begin(), full_sizes.begin() + half_length + 1);
    const auto n = sizes[0];
    auto connection = connect_layers(rows, sizes, st.component_raw_reads);
    std::vector<std::vector<VertexId>> components;
    std::vector<std::size_t> root_component(connection.dsu.parent.size(), absent);
    std::vector<std::size_t> source_component(n);
    for (std::size_t u = 0; u < n; ++u) {
        auto& c = root_component[connection.dsu.find(u)];
        if (c == absent) { c = components.size(); components.emplace_back(); }
        source_component[u] = c; components[c].push_back(static_cast<VertexId>(u));
    }
    st.components = components.size();
    std::vector<bool> remaining(components.size(), true);
    std::size_t remaining_count = components.size();
    auto finish = [&](std::size_t c, bool core, bool early) {
        remaining[c] = false; --remaining_count;
        for (auto u : components[c]) {
            keep[u] = false;
            if (core) resolved[u] = components[c][0];
        }
        if (core) {
            st.completed_core_vertices += components[c].size();
            (early ? st.early_core_vertices : st.scalar_core_vertices) += components[c].size();
        } else st.completed_noncore_vertices += components[c].size();
    };
    for (std::size_t c = 0; c < components.size(); ++c)
        if (components[c].size() < mu) finish(c, false, true);
    if (remaining_count == 0) return;

    const auto terminal_layer = sizes.size() - 2;
    std::vector<std::size_t> terminal_component(sizes[terminal_layer], absent);
    std::vector<std::uint64_t> terminal_counts(components.size(), 0);
    for (std::size_t b = 0; b < sizes[terminal_layer]; ++b)
        if (connection.active[terminal_layer][b]) {
            const auto c = root_component[connection.dsu.find(connection.offsets[terminal_layer] + b)];
            if (c == absent) throw std::logic_error("active half-path without a source");
            terminal_component[b] = c; ++terminal_counts[c];
        }
    const auto& groups = index.groups_for(graph, rows.back());
    std::vector<std::size_t> group_components(groups.offsets.size() - 1, absent);
    for (std::size_t g = 0; g < group_components.size(); ++g) {
        std::size_t c = absent;
        std::uint64_t covered = 0;
        visit_group(groups, g, [&](VertexId b) {
            count(st.coverage_member_reads);
            if (terminal_component[b] == absent) return;
            if (c != absent && c != terminal_component[b])
                throw std::logic_error("majority clique straddles projection components");
            c = terminal_component[b]; ++covered;
        });
        group_components[g] = c;
        if (c != absent && remaining[c] && covered == terminal_counts[c]) finish(c, true, true);
    }
    if (remaining_count == 0) return;

    const std::vector<Transition> prefix(rows.begin(), rows.end() - 1);
    const std::vector<std::size_t> prefix_sizes(sizes.begin(), sizes.end() - 1);
    std::vector<std::uint64_t> upper;
    const auto& inner = *rows.back().relation;
    if (prefix.empty() && inner.roundtrip_ready) {
        upper = rows.back().use_reverse ? inner.target_closed_degrees : inner.source_closed_degrees;
    } else {
        upper = walk_upper(full_rows, full_sizes, n, st.profile_raw_reads);
        for (std::size_t u = 0; u < n; ++u)
            upper[u] = std::min<std::uint64_t>(components[source_component[u]].size(),
                                             std::max<std::uint64_t>(1, upper[u]));
    }
    const auto q_upper = walk_upper(prefix, prefix_sizes, prefix_sizes.back(), st.profile_raw_reads);
    std::vector<std::uint64_t> delta(components.size(), 0);
    for (std::size_t u = 0; u < n; ++u)
        delta[source_component[u]] = std::max(delta[source_component[u]], q_upper[u]);
    std::vector<std::uint64_t> lower(n, 1), maximum = std::move(upper);
    for (std::size_t l = 0; l < prefix.size(); ++l) {
        std::vector<std::uint64_t> next_lower(prefix_sizes[l + 1], 0);
        std::vector<std::uint64_t> next_max(prefix_sizes[l + 1], 0);
        for (std::size_t u = 0; u < prefix_sizes[l]; ++u)
            for (auto b : prefix[l].neighbors(static_cast<VertexId>(u))) {
                count(st.profile_raw_reads);
                // Exact first-hop indegree; later preimage sets may overlap.
                next_lower[b] = l == 0 ? capped_add(next_lower[b], 1, n)
                                      : std::max(next_lower[b], lower[u]);
                next_max[b] = std::max(next_max[b], maximum[u]);
            }
        lower = std::move(next_lower); maximum = std::move(next_max);
    }
    std::vector<std::size_t> accepted;
    for (std::size_t g = 0; g < group_components.size(); ++g) {
        const auto c = group_components[g];
        if (c == absent || !remaining[c]) continue;
        std::uint64_t total = 0, best = 0, m = 0;
        visit_group(groups, g, [&](VertexId b) {
            count(st.certificate_member_reads);
            total = capped_add(total, lower[b], max64);
            best = std::max(best, lower[b]); m = std::max(m, maximum[b]);
        });
        const auto divisor = std::min(groups.offsets[g + 1] - groups.offsets[g], delta[c]);
        if (divisor == 0 || m == 0) continue;
        const auto bound = std::max(best, total / divisor + (total % divisor != 0));
        if (bound >= mu && ratio_at_least(bound, m, eps.numerator, eps.denominator))
            accepted.push_back(g);
    }
    st.accepted_groups = accepted.size();
    if (accepted.empty()) return;
    // Release the half-path DSU before constructing the batched prefix DSU.
    connection = LayerConnection{};
    root_component.clear(); root_component.shrink_to_fit();
    auto certified = connect_layers(prefix, prefix_sizes, st.lift_raw_reads,
                                    &groups, &accepted, &keep, &st.lift_member_reads);
    for (std::size_t c = 0; c < components.size(); ++c) if (remaining[c]) {
        const auto root = certified.dsu.find(components[c][0]);
        bool complete = true;
        for (auto u : components[c])
            if (!certified.active[0][u] || certified.dsu.find(u) != root) { complete = false; break; }
        if (complete) finish(c, true, false);
    }
}

}  // namespace

LayeredCompletionResult run_layered_completion(const HinGraph& graph,
    const MajorityIndex& index, const std::vector<std::uint32_t>& path,
    const SimilarityThreshold& threshold, std::uint64_t pscan_other_mu) {
    const auto begin = Clock::now();
    if (pscan_other_mu == 0 || pscan_other_mu == max64)
        throw std::invalid_argument("mu must count at least one other neighbor and fit with self");
    if (threshold.numerator == 0 || threshold.denominator == 0 ||
        threshold.numerator > threshold.denominator)
        throw std::invalid_argument("epsilon must be in (0,1]");
    if (path.size() < 3 || path.size() % 2 == 0)
        throw std::invalid_argument("factor index requires an odd-length symmetric type sequence");
    std::vector<std::size_t> sizes;
    std::vector<Transition> transitions;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] != path[path.size() - 1 - i])
            throw std::invalid_argument("factor index requires a symmetric meta-path");
        if (path[i] >= graph.vertex_types().size()) throw std::invalid_argument("invalid meta-path type");
        const auto n = graph.vertex_types()[path[i]].count;
        if (n > missing || n > std::numeric_limits<std::size_t>::max())
            throw std::overflow_error("layered completion uses 32-bit per-type vertex ids");
        sizes.push_back(static_cast<std::size_t>(n));
        if (i) transitions.push_back(graph.transition(path[i - 1], path[i]));
    }
    LayeredCompletionResult output;
    auto& st = output.layers;
    const auto n = sizes[0]; st.vertices = n;
    st.fast_path_supported = true;
    for (std::size_t i = 0; i < transitions.size() / 2; ++i) {
        const auto& a = transitions[i];
        const auto& b = transitions[transitions.size() - 1 - i];
        if (a.relation->source_type == a.relation->target_type ||
            a.relation != b.relation || a.use_reverse == b.use_reverse)
            st.fast_path_supported = false;
    }
    std::vector<bool> keep(n, true);
    std::vector<VertexId> resolved(n, missing);
    if (st.fast_path_supported)
        prove_components(graph, index, transitions, sizes, threshold, pscan_other_mu + 1,
                         st, keep, resolved);
    // All substantial proof workspaces have been released before any factor build.
    const auto proof_end = Clock::now(); st.layer_proof_ms = ms(proof_end - begin);
    std::vector<VertexId> original;
    for (std::size_t u = 0; u < n; ++u) if (keep[u]) original.push_back(static_cast<VertexId>(u));
    st.residual_vertices = original.size();
    PscanOnFliResult residual;
    if (!original.empty()) {
        const auto factor = st.fast_path_supported
            ? FactorIndex::build_selected(graph, path, original)
            : FactorIndex::build(graph, path);
        st.residual_incidences = factor.stats().half_path_incidences;
        st.residual_half_expansion_entries = factor.stats().half_expansion_entries;
        output.residual_factor = factor.stats();
        const auto prepared = Clock::now(); st.residual_prepare_ms = ms(prepared - proof_end);
        residual = run_pscan_on_fli(factor, threshold, pscan_other_mu,
            32ULL * 1024 * 1024, nullptr, nullptr, true, BlockExecutionMode::CoreConnectivity);
        st.residual_scan_ms = ms(Clock::now() - prepared);
    } else st.residual_prepare_ms = ms(Clock::now() - proof_end);
    const auto merge_begin = Clock::now();
    auto& r = output.clustering;
    r.stats = residual.stats;
    r.is_core.assign(n, false); r.core_cluster.assign(n, missing);
    r.noncore_clusters.resize(n); r.roles.assign(n, PscanVertexRole::Outlier);
    for (std::size_t u = 0; u < n; ++u) if (!keep[u] && resolved[u] != missing) {
        r.is_core[u] = true; r.core_cluster[u] = resolved[u]; r.roles[u] = PscanVertexRole::Core;
    }
    for (std::size_t u = 0; u < residual.is_core.size(); ++u) {
        const auto dest = original[u];
        r.is_core[dest] = residual.is_core[u]; r.roles[dest] = residual.roles[u];
        if (residual.is_core[u]) r.core_cluster[dest] = original[residual.core_cluster[u]];
        for (auto id : residual.noncore_clusters[u]) r.noncore_clusters[dest].push_back(original[id]);
    }
    r.stats.core_vertices = r.stats.clusters = r.stats.border_vertices = 0;
    r.stats.hub_vertices = r.stats.outlier_vertices = 0;
    for (std::size_t u = 0; u < n; ++u) {
        if (r.is_core[u]) { ++r.stats.core_vertices; if (r.core_cluster[u] == u) ++r.stats.clusters; }
        else if (r.roles[u] == PscanVertexRole::Border) ++r.stats.border_vertices;
        else if (r.roles[u] == PscanVertexRole::Hub) ++r.stats.hub_vertices;
        else ++r.stats.outlier_vertices;
    }
    st.result_merge_ms = ms(Clock::now() - merge_begin);
    return output;
}
}  // namespace hinscan
