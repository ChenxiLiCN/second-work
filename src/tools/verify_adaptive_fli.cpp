#include "hin/HinGraph.h"
#include "index/FactorIndex.h"
#include "scan/PscanOnFli.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>

using namespace hinscan;
namespace {
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
std::uint64_t common(const std::vector<VertexId>& a, const std::vector<VertexId>& b) {
    std::uint64_t result = 0;
    for (const auto v : a) result += std::binary_search(b.begin(), b.end(), v);
    return result;
}
// Independent oracle: traverse the WHOLE path using sets, not FLI witnesses,
// projected-degree stamps, lazy union, or adaptive cache implementations.
std::vector<std::vector<VertexId>> oracle_neighborhoods(
    const HinGraph& graph, const std::vector<std::uint32_t>& path) {
    std::vector<std::vector<VertexId>> result(graph.vertex_types()[path.front()].count);
    for (VertexId v=0; v<result.size(); ++v) {
        std::set<VertexId> frontier{v};
        for (std::size_t step=1; step<path.size(); ++step) {
            std::set<VertexId> next;
            const auto transition = graph.transition(path[step-1], path[step]);
            for (const auto x : frontier) {
                const auto& adjacent = transition.neighbors(x);
                next.insert(adjacent.begin(), adjacent.end());
            }
            frontier.swap(next);
        }
        frontier.insert(v); // Structural similarity uses closed neighborhoods.
        result[v].assign(frontier.begin(), frontier.end());
    }
    return result;
}
PscanOnFliResult oracle_clustering(const std::vector<std::vector<VertexId>>& closed,
                                  const SimilarityThreshold& eps, std::uint64_t mu) {
    const auto n = closed.size();
    std::vector<std::vector<VertexId>> similar(n);
    for (VertexId u=0; u<n; ++u) for (const auto v : closed[u]) {
        if (u==v) continue;
        const auto c = common(closed[u], closed[v]);
        // Small fixtures permit an exact integer comparison without overflow.
        if (c*c*eps.denominator*eps.denominator >=
            eps.numerator*eps.numerator*closed[u].size()*closed[v].size())
            similar[u].push_back(v);
    }
    PscanOnFliResult r;
    r.is_core.resize(n);
    r.core_cluster.assign(n, std::numeric_limits<VertexId>::max());
    r.noncore_clusters.resize(n);
    r.roles.assign(n, PscanVertexRole::Outlier);
    for (VertexId v=0; v<n; ++v) r.is_core[v] = similar[v].size() >= mu;
    for (VertexId root=0; root<n; ++root) {
        if (!r.is_core[root] || r.core_cluster[root] != std::numeric_limits<VertexId>::max()) continue;
        std::vector<VertexId> queue{root};
        r.core_cluster[root] = root;
        for (std::size_t i=0; i<queue.size(); ++i) for (const auto v : similar[queue[i]]) {
            if (r.is_core[v] && r.core_cluster[v] == std::numeric_limits<VertexId>::max()) {
                r.core_cluster[v] = root;
                queue.push_back(v);
            }
        }
    }
    for (VertexId v=0; v<n; ++v) {
        if (r.is_core[v]) { r.roles[v] = PscanVertexRole::Core; continue; }
        std::set<VertexId> memberships;
        for (const auto u : similar[v]) if (r.is_core[u]) memberships.insert(r.core_cluster[u]);
        r.noncore_clusters[v].assign(memberships.begin(), memberships.end());
        if (memberships.size()==1) r.roles[v] = PscanVertexRole::Border;
        if (memberships.size()>1) r.roles[v] = PscanVertexRole::Hub;
    }
    return r;
}
void compare(const PscanOnFliResult& a, const PscanOnFliResult& b) {
    require(a.is_core == b.is_core, "core mismatch");
    require(a.core_cluster == b.core_cluster, "component mismatch");
    require(a.noncore_clusters == b.noncore_clusters, "membership mismatch");
    require(a.roles == b.roles, "role mismatch");
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Usage: verify_adaptive_fli <fixture-output-directory>");
        std::uint64_t predicates=0, cluster_cases=0, evictions=0, bitmap_checks=0, list_checks=0;
        std::uint64_t streaming_checks=0, pair_bound_hits=0, equal_factor_checks=0;
        std::uint64_t witness_accepts=0, witness_rejects=0, witness_hits=0;
        std::uint64_t witness_bitmap_checks=0;
        std::mt19937 random(20260905);
        for (unsigned fixture=0; fixture<14; ++fixture) {
            const unsigned n = fixture==0 ? 0 : fixture==1 ? 1 : fixture==2 ? 7 : fixture==3 ? 65 : 257;
            const unsigned m = fixture<4 ? 5 : fixture==4 ? 257 : 23;
            std::vector<std::pair<unsigned,unsigned>> edges;
            for (unsigned u=0; u<n; ++u) for (unsigned v=0; v<m; ++v) {
                bool present = false;
                if (fixture==2) present = (u<5 && v<3);
                if (fixture==3) present = true; // Dense bitmap, non-word-aligned domain.
                if (fixture==4) present = u==v; // Lists, singleton neighborhoods.
                if (fixture==5) present = u/16==v; // Sparse bitmaps.
                if (fixture>=6) present = random()%1000 < (fixture-5)*17;
                if (fixture==10) present = (v==0 && u<=5) || (v==1 && (u==0 || (u>=6 && u<=10)));
                if (fixture==11) present = u<4 && v<4; // Duplicate witnesses cannot add density.
                if (fixture==12) present = v<3 && u>=5*v && u<=5*v+5; // Overlapping blocks.
                if (fixture==13) present = (v==0 && u<65) || (v==1 && u>0 && u<=65); // Sparse witness words.
                if (present) edges.emplace_back(u,v);
            }
            const auto dir = std::filesystem::path(argv[1])/std::to_string(fixture);
            std::filesystem::create_directories(dir/"edge");
            {
                std::ofstream schema(dir/"base.txt"), relation(dir/"edge/0.txt");
                schema << "2\nA " << n << "\nB " << m << "\n1\n0 1 " << edges.size() << '\n';
                relation << "0 1 " << edges.size() << '\n';
                for (const auto& e : edges) relation << e.first << ' ' << e.second << '\n';
            }
            const auto graph = HinGraph::load(dir);
            for (const auto* specification : {"A-B-A", "A-B-A-B-A"}) {
                const auto path = parse_meta_path(graph, specification);
                const auto factor = FactorIndex::build(graph, path);
                const auto expected = oracle_neighborhoods(graph,path);
                for (VertexId v=0; v<n; ++v) {
                    require(factor.degree(v)==expected[v].size(), "degree builder mismatch");
                    require(factor.collect_closed_neighborhood(v)==expected[v], "factor neighborhood mismatch");
                }
                for (const std::uint64_t budget : {0ULL, 1ULL, 96ULL, 1024ULL, 1048576ULL}) {
                    for (const unsigned bounds : {0U,1U,2U,3U,4U,5U}) {
                    ExactNeighborhoodCache cache(factor,budget,bounds!=0,bounds==2 || bounds==4,bounds==3 || bounds==4,bounds==5);
                    for (VertexId u=0; u<n; ++u) {
                        cache.activate(u);
                        for (VertexId v=0; v<n; ++v) {
                            const auto c = common(expected[u], expected[v]);
                            for (const std::uint64_t required : {std::uint64_t{0},c,c+1,std::uint64_t{n+1}}) {
                                require(cache.check(v,required)==(c>=required), "cache intersection mismatch");
                                ++predicates;
                            }
                        }
                    }
                    require(cache.stats().peak_bytes + cache.stats().pair_bound_bytes <= budget, "cache budget exceeded");
                    require(cache.stats().witness_bitmap_bytes <= budget, "witness bitmap budget exceeded");
                    evictions += cache.stats().evictions;
                    bitmap_checks += cache.stats().bitmap_checks;
                    list_checks += cache.stats().list_checks;
                    streaming_checks += cache.stats().streaming_checks;
                    pair_bound_hits += cache.stats().pair_bound_hits;
                    equal_factor_checks += cache.stats().equal_factor_checks;
                    witness_accepts += cache.stats().witness_bound_accepts;
                    witness_rejects += cache.stats().witness_bound_rejects;
                    witness_hits += cache.stats().witness_count_hits;
                    witness_bitmap_checks += cache.stats().witness_bitmap_checks;
                    }
                    for (const auto* text : {"0.2","0.5","0.7","0.9","1.0"}) for (const std::uint64_t mu : {1ULL,2ULL,5ULL,300ULL}) {
                        const auto eps = SimilarityThreshold::parse(text);
                        const auto reference = oracle_clustering(expected,eps,mu);
                        compare(run_pscan_on_fli(factor,eps,mu,budget),reference);
                        // Unchanged pSCAN processing with old neighborhood engine.
                        compare(run_pscan_on_fli(factor,eps,mu,budget,nullptr,nullptr,false),reference);
                        compare(run_pscan_on_fli(factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::SeedOnly),reference);
                        compare(run_pscan_on_fli(factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::SkipCertified),reference);
                        compare(run_pscan_on_fli(factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::WitnessBounds),reference);
                        compare(run_pscan_on_fli(factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::AdaptiveWitnessBounds),reference);
                        compare(run_pscan_on_fli(factor,eps,mu,budget,nullptr,nullptr,false,BlockExecutionMode::AdaptiveWitnessBounds),reference);
                        compare(run_pscan_on_fli(factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::WitnessBitmaps),reference);
                        compare(run_pscan_on_fli(factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::WitnessExclusion),reference);
                        compare(run_pscan_on_fli(factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::LazyNoWitness),reference);
                        compare(run_pscan_on_fli(factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::LazyAlwaysExclusion),reference);
                        cluster_cases += 11;
                    }
                }
                std::cout << "fixture=" << fixture << " path=" << specification << " passed\n";
            }
        }
        require(evictions>0 && bitmap_checks>0 && list_checks>0, "missing cache branch coverage");
        require(streaming_checks>0 && pair_bound_hits>0 && equal_factor_checks>0,
                "missing streaming or sharing branch coverage");
        require(witness_accepts>0 && witness_rejects>0 && witness_hits>0,
                "missing witness bound branch coverage");
        require(witness_bitmap_checks>0, "missing witness bitmap coverage");
        std::cout << "exact_predicates=" << predicates << "\ncluster_cases=" << cluster_cases
                  << "\nevictions=" << evictions << "\nbitmap_checks=" << bitmap_checks
                  << "\nlist_checks=" << list_checks << "\nstreaming_checks=" << streaming_checks
                  << "\npair_bound_hits=" << pair_bound_hits << "\nequal_factor_checks=" << equal_factor_checks
                  << "\nwitness_accepts=" << witness_accepts << "\nwitness_rejects=" << witness_rejects
                  << "\nwitness_hits=" << witness_hits
                  << "\nwitness_bitmap_checks=" << witness_bitmap_checks
                  << "\nall_passed=1\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "verify_adaptive_fli: " << e.what() << '\n'; return 1;
    }
}
