#include "hin/HinGraph.h"
#include "scan/PscanOnFli.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <sstream>
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
#ifdef HINSCAN_VERIFY_RESUMABLE
std::uint64_t saved_rows=0,partial_hits=0,promotions=0,left_completions=0,partial_evictions=0,partial_drops=0;
std::uint64_t verify_resume(const FactorIndex& f,const std::vector<std::vector<VertexId>>& rows,
                            std::mt19937& random) {
    if (rows.empty()) return 0;
    std::uint64_t checks=0;
    for (auto budget:{0ULL,128ULL,256ULL,4096ULL}) {
        ExactNeighborhoodCache cache(f,budget,false,false,false,false,true,true,true);
        // Changing left rows and nonmonotonic thresholds must not reuse counts.
        for (unsigned round=0;round<6000;++round) {
            const VertexId u=random()%rows.size(),v=random()%rows.size();
            const auto t=round<100?1:random()%(rows.size()+2);
            cache.activate(u);
            require(cache.check(v,t)==(common(rows[u],rows[v])>=t),"resumed predicate mismatch");
            if (round%7==0) {
                cache.activate(v);
                require(cache.check(u,t)==(common(rows[u],rows[v])>=t),"resumed reverse mismatch");
                ++checks;
            }
            ++checks;
        }
        const auto& s=cache.stats();
        require(s.peak_bytes+s.pair_bound_bytes<=budget,"resume cache budget exceeded");
        require(s.resume_checks==s.streaming_checks,"resume check accounting");
        require(s.resume_partial_saves+s.resume_partial_drops==s.single_pass_partial,"partial accounting");
        require(s.single_pass_complete+s.single_pass_partial==s.streaming_checks,"completion accounting");
        saved_rows+=s.resume_partial_saves; partial_hits+=s.resume_hits;
        promotions+=s.resume_promotions; left_completions+=s.resume_left_completions;
        partial_evictions+=s.resume_partial_evictions; partial_drops+=s.resume_partial_drops;
    }
    return checks;
}
#endif
#ifdef HINSCAN_HOTSPOTS
void verify_report(const FactorIndex& f,const std::filesystem::path& dir) {
    std::ifstream input(dir/"hotspots.tsv");
    require(bool(input),"missing hotspot details");
    std::string line; std::getline(input,line);
    while (std::getline(input,line)) {
        std::istringstream stream(line);std::vector<std::string> fields;std::string value;
        while (std::getline(stream,value,'\t')) fields.push_back(value);
        require(fields.size()==22,"hotspot TSV column mismatch");
        if (fields[13]!="1") continue;
        const auto u=static_cast<VertexId>(std::stoul(fields[1]));
        auto ws=f.witnesses(u);
        std::sort(ws.begin(),ws.end(),[&](auto a,auto b) {
            return f.posting(a).size()!=f.posting(b).size()?f.posting(a).size()>f.posting(b).size():a<b;
        });
        std::set<VertexId> members;
        for (std::size_t i=0;i<4;++i) {
            if (i<ws.size()) members.insert(f.posting(ws[i]).begin(),f.posting(ws[i]).end());
            if (i==0 || i==1 || i==3)
                require(members.size()==std::stoull(fields[i==3?16:14+i]),"exact coverage mismatch");
        }
    }
}
#endif
}
int main(int argc,char** argv) {
    try {
        if (argc!=2) throw std::invalid_argument("Usage: verifier <new-fixture-directory>");
        const std::filesystem::path root(argv[1]);
        std::filesystem::create_directories(root);
        require(std::filesystem::is_empty(root),"fixture directory must be empty");
        std::mt19937 random(20260910);
        std::uint64_t predicates=0,clusters=0,rejects=0,resumed=0,replacements=0,cached=0;
        std::uint64_t integrated_rejects=0;
#ifdef HINSCAN_VERIFY_RESUMABLE
        const unsigned fixtures=15;
#else
        const unsigned fixtures=14;
#endif
        for (unsigned fixture=0;fixture<fixtures;++fixture) {
            const unsigned n=fixture==0?0:fixture==1?1:fixture==2?12:fixture==14?257:31;
            const unsigned m=fixture==2?3:9;
            std::vector<std::pair<unsigned,unsigned>> edges;
            for (unsigned u=0;u<n;++u) for (unsigned w=0;w<m;++w) {
                bool present=false;
                if (fixture==2) present=(w==0 && u<5)||(w==1 && u>=5 && u<10)||
                    (w==2 && (u==4||u==5)); // Small bridge, two dominant anchors, isolates.
                if (fixture==3) present=true;
                if (fixture==4) present=u%m==w;
                if (fixture>=5) present=random()%100<(fixture-4)*7;
                if (fixture==14) present=(w==0 && u<30)||(w==1 && u>=20 && u<40)||
                    (w==2 && u>=128 && u<155)||(w==3 && (u<5||u>=250));
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
#ifdef HINSCAN_VERIFY_RESUMABLE
                predicates+=verify_resume(f,rows,random);
#else
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
#endif
                for (const auto* eps:{"0.1","0.5","0.6","0.9","1"}) for (auto mu:{1ULL,2ULL,5ULL,40ULL}) {
                    const auto threshold=SimilarityThreshold::parse(eps);
                    const auto truth=oracle(rows,threshold,mu);
                    for (auto bytes:{0ULL,4096ULL}) {
                        const auto control=run_pscan_on_fli(f,threshold,mu,bytes,nullptr,nullptr,true,BlockExecutionMode::CoreSinglePassLean);
#ifdef HINSCAN_VERIFY_RESUMABLE
                        const auto trial=run_pscan_on_fli(f,threshold,mu,bytes,nullptr,nullptr,true,BlockExecutionMode::CoreResumable);
#else
                        const auto trial=run_pscan_on_fli(f,threshold,mu,bytes,nullptr,nullptr,true,BlockExecutionMode::CoreAnchor);
#endif
                        compare(control,truth); compare(trial,truth);
#ifdef HINSCAN_HOTSPOTS
                        for (const auto* result:{&control,&trial}) {
                            const auto& stats=result->stats.adaptive_cache;
                            require(bool(stats.hotspots),"missing hotspot trace");
                            std::uint64_t se=0,ge=0,sc=0,pc=0;
                            for (const auto& row:stats.hotspots->rows) {
                                se+=row.stream_entries; ge+=row.generation_entries;
                                sc+=row.stream_calls; pc+=row.partial_calls;
                            }
                            require(se==stats.streaming_posting_entries && ge==stats.posting_entries,
                                    "hotspot scan accounting mismatch");
                            require(sc==stats.streaming_checks && pc==stats.single_pass_partial,
                                    "hotspot call accounting mismatch");
                        }
                        if (std::string(eps)=="0.9" && mu==5 && bytes==4096) {
                            write_hotspot_report(f,*control.stats.adaptive_cache.hotspots,dir/(std::string(text)+"-report"));
                            verify_report(f,dir/(std::string(text)+"-report"));
                        }
#endif
#ifndef HINSCAN_VERIFY_RESUMABLE
                        require(trial.stats.anchor.calls==trial.stats.exact_similarity_checks+trial.stats.anchor.rejects,
                                "full-check accounting");
#endif
                        integrated_rejects+=trial.stats.anchor.rejects; ++clusters;
                    }
                }
            }
        }
#ifdef HINSCAN_VERIFY_RESUMABLE
        require(saved_rows && partial_hits && promotions && left_completions && partial_evictions && partial_drops,
                "unexercised resumable branch");
        std::cout<<"partial_saves="<<saved_rows<<"\npartial_hits="<<partial_hits
                 <<"\npromotions="<<promotions<<"\nleft_completions="<<left_completions
                 <<"\npartial_evictions="<<partial_evictions<<"\npartial_drops="<<partial_drops<<'\n';
#else
        require(rejects && resumed && replacements && cached && integrated_rejects,"unexercised anchor branch");
#endif
        std::cout<<"predicate_cases="<<predicates<<"\ncluster_cases="<<clusters
                 <<"\nanchor_rejects="<<rejects<<"\nresumed="<<resumed
                 <<"\nreplacements="<<replacements<<"\ncached_decisions="<<cached
                 <<"\nintegrated_rejects="<<integrated_rejects<<"\nall_passed=1\n";
        return 0;
    } catch (const std::exception& e) { std::cerr<<"verify_anchor_filter: "<<e.what()<<'\n'; return 1; }
}
