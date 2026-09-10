#include "hin/HinGraph.h"
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
void require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
std::uint64_t common(const std::vector<VertexId>& a,const std::vector<VertexId>& b) {
    std::uint64_t c=0;
    for (auto x:a) c+=std::binary_search(b.begin(),b.end(),x);
    return c;
}
// Independent full-path traversal; never uses factor witnesses or cache rows.
std::vector<std::vector<VertexId>> closed_rows(const HinGraph& g,const std::vector<std::uint32_t>& path) {
    std::vector<std::vector<VertexId>> rows(g.vertex_types()[path.front()].count);
    for (VertexId u=0;u<rows.size();++u) {
        std::set<VertexId> f{u};
        for (std::size_t i=1;i<path.size();++i) {
            std::set<VertexId> next;
            const auto tr=g.transition(path[i-1],path[i]);
            for (auto x:f) { const auto& ns=tr.neighbors(x); next.insert(ns.begin(),ns.end()); }
            f.swap(next);
        }
        f.insert(u); rows[u].assign(f.begin(),f.end());
    }
    return rows;
}
PscanOnFliResult oracle(const std::vector<std::vector<VertexId>>& rows,
                       const SimilarityThreshold& e,std::uint64_t mu) {
    const auto n=rows.size();
    std::vector<std::vector<VertexId>> similar(n);
    for (VertexId u=0;u<n;++u) for (auto v:rows[u]) if (u!=v) {
        const auto c=common(rows[u],rows[v]);
        // Tiny fixtures: integer products cannot overflow.
        if (c*c*e.denominator*e.denominator>=e.numerator*e.numerator*rows[u].size()*rows[v].size())
            similar[u].push_back(v);
    }
    PscanOnFliResult r;
    const auto missing=~VertexId{0};
    r.is_core.resize(n); r.core_cluster.assign(n,missing);
    r.noncore_clusters.resize(n); r.roles.assign(n,PscanVertexRole::Outlier);
    for (VertexId u=0;u<n;++u) r.is_core[u]=similar[u].size()>=mu;
    for (VertexId u=0;u<n;++u) {
        if (!r.is_core[u] || r.core_cluster[u]!=missing) continue;
        std::vector<VertexId> queue{u}; r.core_cluster[u]=u;
        for (std::size_t i=0;i<queue.size();++i) for (auto v:similar[queue[i]])
            if (r.is_core[v] && r.core_cluster[v]==missing) { r.core_cluster[v]=u; queue.push_back(v); }
    }
    for (VertexId u=0;u<n;++u) {
        if (r.is_core[u]) { r.roles[u]=PscanVertexRole::Core; continue; }
        std::set<VertexId> ids;
        for (auto v:similar[u]) if (r.is_core[v]) ids.insert(r.core_cluster[v]);
        r.noncore_clusters[u].assign(ids.begin(),ids.end());
        if (ids.size()==1) r.roles[u]=PscanVertexRole::Border;
        if (ids.size()>1) r.roles[u]=PscanVertexRole::Hub;
    }
    return r;
}
void compare(const PscanOnFliResult& a,const PscanOnFliResult& b) {
    require(a.is_core==b.is_core,"core mismatch");
    require(a.core_cluster==b.core_cluster,"cluster mismatch");
    require(a.noncore_clusters==b.noncore_clusters,"noncore memberships mismatch");
    require(a.roles==b.roles,"roles mismatch");
}
}
int main(int argc,char** argv) {
    try {
        if (argc!=2) throw std::invalid_argument("Usage: verify_anchor_filter <new-fixture-directory>");
        const std::filesystem::path root(argv[1]);
        std::filesystem::create_directories(root);
        require(std::filesystem::is_empty(root),"fixture directory must be empty");
        std::mt19937 random(20260910);
        std::uint64_t predicates=0,clusters=0,rejects=0,resumed=0,replacements=0,cached=0;
        std::uint64_t integrated_rejects=0;
        for (unsigned fixture=0;fixture<14;++fixture) {
            const unsigned n=fixture==0?0:fixture==1?1:fixture==2?12:31;
            const unsigned m=fixture==2?3:9;
            std::vector<std::pair<unsigned,unsigned>> edges;
            for (unsigned u=0;u<n;++u) for (unsigned w=0;w<m;++w) {
                bool present=false;
                if (fixture==2) present=(w==0 && u<5)||(w==1 && u>=5 && u<10)||
                    (w==2 && (u==4||u==5)); // Small bridge, two dominant anchors, isolates.
                if (fixture==3) present=true;
                if (fixture==4) present=u%m==w;
                if (fixture>=5) present=random()%100<(fixture-4)*7;
                if (present) { edges.emplace_back(u,w); if (u%7==0) edges.emplace_back(u,w); }
            }
            const auto dir=root/std::to_string(fixture);
            std::filesystem::create_directories(dir/"edge");
            {
                std::ofstream base(dir/"base.txt"), rel(dir/"edge/0.txt");
                base<<"2\nA "<<n<<"\nB "<<m<<"\n1\n0 1 "<<edges.size()<<'\n';
                rel<<"0 1 "<<edges.size()<<'\n';
                for (const auto& e:edges) rel<<e.first<<' '<<e.second<<'\n';
            }
            const auto graph=HinGraph::load(dir);
            for (const auto* text:{"A-B-A","B-A-B","A-B-A-B-A"}) {
                const auto path=parse_meta_path(graph,text);
                const auto f=FactorIndex::build(graph,path);
                const auto rows=closed_rows(graph,path);
                for (auto budget:{0ULL,32ULL,4096ULL}) {
                    AnchorFilter filter(f,budget);
                    std::vector<std::uint64_t> thresholds;
                    for (std::uint64_t t=0;t<=rows.size()+1;++t) thresholds.push_back(t);
                    std::shuffle(thresholds.begin(),thresholds.end(),random);
                    // Repeated, nonmonotonic thresholds exercise cursor reuse and resets.
                    for (VertexId u=0;u<rows.size();++u) for (VertexId v=0;v<rows.size();++v)
                        for (auto t:thresholds) {
                            const bool rejected=filter.reject(u,v,t);
                            require(!rejected || common(rows[u],rows[v])<t,"unsafe anchor rejection");
                            const bool reverse=filter.reject(v,u,t);
                            require(reverse==rejected,"orientation or threshold state mismatch");
                            predicates+=2;
                        }
                    const auto& s=filter.stats();
                    require(s.calls==s.no_anchor+s.ineligible+s.eligible,"call accounting");
                    require(s.eligible==s.rejects+s.fallbacks,"decision accounting");
                    require(s.state_bytes<=budget,"state budget exceeded");
                    rejects+=s.rejects; resumed+=s.resumed; replacements+=s.replacements;
                    cached+=s.cached_rejects+s.cached_fallbacks;
                }
                for (const auto* eps:{"0.1","0.5","0.6","0.9","1"}) for (auto mu:{1ULL,2ULL,5ULL,40ULL}) {
                    const auto threshold=SimilarityThreshold::parse(eps);
                    const auto truth=oracle(rows,threshold,mu);
                    for (auto bytes:{0ULL,4096ULL}) {
                        const auto control=run_pscan_on_fli(f,threshold,mu,bytes,nullptr,nullptr,true,BlockExecutionMode::CoreSinglePassLean);
                        const auto trial=run_pscan_on_fli(f,threshold,mu,bytes,nullptr,nullptr,true,BlockExecutionMode::CoreAnchor);
                        compare(control,truth); compare(trial,truth);
                        require(trial.stats.anchor.calls==trial.stats.exact_similarity_checks+trial.stats.anchor.rejects,
                                "full-check accounting");
                        integrated_rejects+=trial.stats.anchor.rejects; ++clusters;
                    }
                }
            }
        }
        require(rejects && resumed && replacements && cached && integrated_rejects,"unexercised anchor branch");
        std::cout<<"predicate_cases="<<predicates<<"\ncluster_cases="<<clusters
                 <<"\nanchor_rejects="<<rejects<<"\nresumed="<<resumed
                 <<"\nreplacements="<<replacements<<"\ncached_decisions="<<cached
                 <<"\nintegrated_rejects="<<integrated_rejects<<"\nall_passed=1\n";
        return 0;
    } catch (const std::exception& e) { std::cerr<<"verify_anchor_filter: "<<e.what()<<'\n'; return 1; }
}
