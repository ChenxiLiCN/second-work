#include "hin/HinGraph.h"
#include "index/FactorIndex.h"
#include "scan/PscanOnFli.h"
#include "scan/RegionCompletion.h"
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
        std::uint64_t single_complete=0, single_accept=0, single_reject=0;
        std::uint64_t lean_sparse_clears=0, lean_full_writes=0, lean_cases=0;
        std::mt19937 random(20260905);
        const auto huge=std::numeric_limits<std::uint64_t>::max();
        const SimilarityThreshold unit{huge,huge};
        require(unit.certifies_common(huge,huge,huge), "wide certificate equality");
        require(!unit.certifies_common(huge-1,huge,huge), "wide certificate rejection");
        std::uint64_t new_core_cases=0, region_cases=0, resolved_cases=0, fallback_cases=0;
        for (unsigned fixture=0; fixture<18; ++fixture) {
            const unsigned n = fixture==0 ? 0 : fixture==1 ? 1 : fixture==2 ? 7 : fixture==3 ? 65 : 257;
            const unsigned m = fixture<4 ? 5 : fixture==4 ? 257 : fixture==15 ? 80 : 23;
            std::vector<std::pair<unsigned,unsigned>> edges;
            for (unsigned u=0; u<n; ++u) for (unsigned v=0; v<m; ++v) {
                bool present = false;
                if (fixture==2) present = (u<5 && v<3);
                if (fixture==3) present = true; // Dense bitmap, non-word-aligned domain.
                if (fixture==4) present = u==v; // Lists, singleton neighborhoods.
                if (fixture==5) present = u/16==v; // Sparse bitmaps.
                if (fixture>=6 && fixture<14) present = random()%1000 < (fixture-5)*17;
                if (fixture==10) present = (v==0 && u<=5) || (v==1 && (u==0 || (u>=6 && u<=10)));
                if (fixture==11) present = u<4 && v<4; // Duplicate witnesses cannot add density.
                if (fixture==12) present = v<3 && u>=5*v && u<=5*v+5; // Overlapping blocks.
                if (fixture==13) present = (v==0 && u<65) || (v==1 && u>0 && u<=65); // Sparse witness words.
                // All vertices core, but two proven cliques joined by a dissimilar edge.
                if (fixture==16) present=(v==0 && u<6) || (v==1 && u>=6 && u<12)
                    || (v==2 && (u==5 || u==6));
                // Noncontiguous completed clique IDs interspersed with a residual component.
                if (fixture==17) present=(v==0 && u<18 && u%3==0)
                    || (v==1 && u<18 && u%3==1) || (v==2 && u<18 && u%3==2)
                    || (v==3 && (u==1 || u==2));
                if (fixture==14 || fixture==15) {
                    present = v==0 && u<6;
                    unsigned next_vertex=6, next_witness=1;
                    const unsigned extra[6]={0,0,1,4,5,6};
                    for (unsigned a=0; a<6; ++a) {
                        const auto count=fixture==14 ? extra[a] : (a==0 ? 0U : 14U);
                        for (unsigned j=0; j<count; ++j,++next_vertex,++next_witness)
                            if (v==next_witness && (u==a || u==next_vertex)) present=true;
                    }
                }
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
            graph.save_binary(dir/"legacy.bri");
            auto enhanced=HinGraph::load_binary(dir/"legacy.bri");
            enhanced.prepare_roundtrip_metadata();
            enhanced.save_binary(dir/"enhanced.bri");
            enhanced=HinGraph::load_binary(dir/"enhanced.bri");
            for (const auto* p : {"A-B-A", "B-A-B", "A-B-A-B-A"}) {
                const auto parsed=parse_meta_path(graph,p);
                const auto original=FactorIndex::build(graph,parsed);
                const auto prepared=FactorIndex::build(enhanced,parsed);
                require(prepared.stats().used_roundtrip_metadata == (parsed.size()==3),
                        "roundtrip fast-path routing");
                for (VertexId v=0; v<original.vertex_count(); ++v)
                    require(original.degree(v)==prepared.degree(v) &&
                            original.collect_closed_neighborhood(v)==prepared.collect_closed_neighborhood(v),
                            "roundtrip neighborhood mismatch");
                for (VertexId w=0; w<original.center_count(); ++w)
                    require(original.degree_ordered_posting(w)==prepared.degree_ordered_posting(w),
                            "roundtrip order mismatch");
            }
            for (const auto* specification : {"A-B-A", "A-B-A-B-A"}) {
                const auto path = parse_meta_path(graph, specification);
                const auto factor = FactorIndex::build(graph, path);
                const auto prepared_factor = FactorIndex::build(enhanced,path);
                const auto relations = FactorIndex::build(graph,path,false);
                require(relations.stats().degree_merge_entries_read==0, "relation view expanded degrees");
                const auto expected = oracle_neighborhoods(graph,path);
                for (VertexId v=0; v<n; ++v) {
                    require(factor.degree(v)==expected[v].size(), "degree builder mismatch");
                    require(factor.collect_closed_neighborhood(v)==expected[v], "factor neighborhood mismatch");
                }
                for (const std::uint64_t budget : {0ULL, 1ULL, 96ULL, 1024ULL, 1048576ULL}) {
                    for (const unsigned bounds : {0U,1U,2U,3U,4U,5U,6U,7U}) {
                    ExactNeighborhoodCache cache(factor,budget,bounds!=0,bounds==2 || bounds==4,bounds==3 || bounds==4,bounds==5,bounds>=6,bounds==7);
                    for (VertexId u=0; u<n; ++u) {
                        cache.activate(u);
                        for (VertexId v=0; v<n; ++v) {
                            const auto c = common(expected[u], expected[v]);
                            std::uint64_t scan_limit=0;
                            if (bounds>=6) for (auto w:factor.witnesses(v)) scan_limit+=factor.posting(w).size();
                            for (const std::uint64_t required : {std::uint64_t{0},c,c+1,std::uint64_t{n+1}}) {
                                const auto before=cache.stats().streaming_posting_entries;
                                require(cache.check(v,required)==(c>=required), "cache intersection mismatch");
                                if (bounds>=6) require(cache.stats().streaming_posting_entries-before<=scan_limit,
                                                       "single pass rescanned a posting");
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
                    if (bounds>=6) {
                        require(cache.stats().witness_counts_built==0 && cache.stats().witness_bound_checks==0,
                                "single pass performed witness pre-scan");
                        single_complete+=cache.stats().single_pass_complete;
                        single_accept+=cache.stats().single_pass_early_accepts;
                        single_reject+=cache.stats().single_pass_early_rejects;
                    }
                    if (bounds==7) {
                        lean_sparse_clears+=cache.stats().activation_sparse_words_cleared;
                        lean_full_writes+=cache.stats().activation_full_words_written;
                    }
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
                        const auto fresh=run_pscan_on_fli(factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::CoreConnectivity);
                        compare(fresh,reference);
                        const auto single=run_pscan_on_fli(factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::CoreSinglePass);
                        compare(single,reference);
                        const auto lean=run_pscan_on_fli(factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::CoreSinglePassLean);
                        compare(lean,reference);
                        compare(run_pscan_on_fli(prepared_factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::CoreSinglePassLean),reference);
                        lean_cases+=2;
                        const auto& x=single.stats.adaptive_cache;
                        const auto& y=lean.stats.adaptive_cache;
                        require(x.hits==y.hits && x.misses==y.misses && x.evictions==y.evictions &&
                                x.posting_entries==y.posting_entries && x.streaming_posting_entries==y.streaming_posting_entries &&
                                x.single_pass_complete==y.single_pass_complete && x.single_pass_partial==y.single_pass_partial &&
                                x.activation_calls==y.activation_calls, "lean changed cache/scanning policy");
                        require(y.activation_full_words_written+y.activation_sparse_words_cleared<=x.activation_full_words_written &&
                                y.streaming_bound_evaluations<=x.streaming_bound_evaluations, "lean increased maintenance work");
                        compare(run_pscan_on_fli(prepared_factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::CoreSinglePass),reference);
                        require(single.stats.block_core_vertices==fresh.stats.block_core_vertices &&
                                single.stats.certified_blocks==fresh.stats.certified_blocks,
                                "single pass changed core block certificates");
                        compare(run_pscan_on_fli(prepared_factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::CoreConnectivity),reference);
                        const auto regional=run_region_completion(relations,eps,mu,budget);
                        compare(regional.clustering,reference);
                        ++region_cases;
                        resolved_cases+=regional.regions.completed_core_vertices>0;
                        fallback_cases+=regional.regions.residual_vertices>0;
                        require(regional.regions.completed_core_vertices
                            +regional.regions.completed_noncore_vertices
                            +regional.regions.residual_vertices==factor.vertex_count(), "region partition");
                        require(regional.regions.avoided_degree_merge_entries
                            +regional.regions.residual_degree_merge_entries
                            ==factor.stats().degree_merge_entries_read, "degree work accounting");
                        if ((fixture==16 || fixture==17) && std::string(specification)=="A-B-A"
                            && eps.numerator==1 && eps.denominator==2 && mu==5)
                            require(regional.regions.residual_vertices==12, "merged unproven bridge");
                        const auto old=run_pscan_on_fli(factor,eps,mu,budget,nullptr,nullptr,true,BlockExecutionMode::WitnessExclusion);
                        require(fresh.stats.block_core_vertices>=old.stats.block_core_vertices,
                                "connectivity certificate lost old cores");
                        if (fresh.stats.block_core_vertices>old.stats.block_core_vertices) ++new_core_cases;
                        cluster_cases += 18;
                    }
                }
                std::cout << "fixture=" << fixture << " path=" << specification << " passed\n";
            }
        }
        require(new_core_cases>0, "stronger certificate never exercised");
        require(lean_sparse_clears>0 && lean_full_writes>0, "lean activation branch coverage");
        std::cout << "lean_cases=" << lean_cases << "\nlean_sparse_clears=" << lean_sparse_clears
                  << "\nlean_full_writes=" << lean_full_writes << '\n';
        require(single_complete>0 && single_accept>0 && single_reject>0, "single pass branch coverage");
        std::cout << "single_pass_complete=" << single_complete << "\nsingle_pass_early_accepts=" << single_accept
                  << "\nsingle_pass_early_rejects=" << single_reject << '\n';
        require(resolved_cases>0 && fallback_cases>0, "missing region branch coverage");
        std::cout << "region_cases=" << region_cases << "\nregion_resolved_cases=" << resolved_cases
                  << "\nregion_fallback_cases=" << fallback_cases << '\n';
        std::cout << "stronger_core_cases=" << new_core_cases << '\n';
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
