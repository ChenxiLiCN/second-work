#include "scan/RegionCompletion.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace hinscan {
namespace {
using Clock=std::chrono::steady_clock;
constexpr auto missing=std::numeric_limits<VertexId>::max();
struct Components {
    std::vector<VertexId> parent, size, minimum;
    explicit Components(std::size_t n):parent(n),size(n,1),minimum(n) {
        std::iota(parent.begin(),parent.end(),0);
        std::iota(minimum.begin(),minimum.end(),0);
    }
    VertexId find(VertexId v) {
        while(parent[v]!=v) { parent[v]=parent[parent[v]]; v=parent[v]; }
        return v;
    }
    void unite(VertexId a, VertexId b) {
        a=find(a); b=find(b); if(a==b) return;
        if(size[a]<size[b]) std::swap(a,b);
        parent[b]=a; size[a]+=size[b]; minimum[a]=std::min(minimum[a],minimum[b]);
    }
};
double ms(Clock::duration value) { return std::chrono::duration<double,std::milli>(value).count(); }
}

RegionCompletionResult run_region_completion(FactorIndex index,
    const SimilarityThreshold& threshold, std::uint64_t mu, std::uint64_t cache_bytes) {
    if(mu==0) throw std::invalid_argument("mu must be positive");
    RegionCompletionResult output;
    auto& st=output.regions;
    const auto begin=Clock::now();
    const auto n=static_cast<std::size_t>(index.vertex_count());
    st.vertices=n; st.incidences=index.stats().half_path_incidences;
    std::vector<bool> keep(n,true);
    std::vector<VertexId> resolved_cluster(n,missing);
    {
        // Graph connectivity must never be confused with similarity connectivity.
        Components graph(n), similar(n);
        for(VertexId w=0; w<index.center_count(); ++w) {
            const auto& row=index.posting(w);
            for(std::size_t j=1; j<row.size(); ++j) graph.unite(row[0],row[j]);
        }
        std::vector<std::uint64_t> upper(n,1);
        for(VertexId v=0; v<n; ++v) {
            const auto root=graph.find(v);
            const std::uint64_t cap=graph.size[root];
            if(root==v) { ++st.components; st.largest_component=std::max(st.largest_component,cap); }
            for(auto w:index.witnesses(v)) {
                const auto extra=index.posting(w).size()-1;
                upper[v]+=std::min<std::uint64_t>(cap-upper[v],extra);
                if(upper[v]==cap) break;
            }
        }
        std::vector<bool> core(n,false);
        for(VertexId w=0; w<index.center_count(); ++w) {
            const auto& row=index.posting(w);
            if(row.size()<=mu) continue;
            std::uint64_t max_degree=1;
            for(auto v:row) max_degree=std::max(max_degree,upper[v]);
            if(!threshold.certifies_common(row.size(),max_degree,max_degree)) continue;
            ++st.certified_postings;
            for(auto v:row) {
                if(!core[v]) { core[v]=true; ++st.certified_core_vertices; }
                similar.unite(row[0],v);
            }
        }
        std::vector<VertexId> first(n,missing);
        std::vector<bool> complete(n,true);
        for(VertexId v=0; v<n; ++v) {
            const auto root=graph.find(v);
            if(!core[v]) { complete[root]=false; continue; }
            const auto c=similar.find(v);
            if(first[root]==missing) first[root]=c;
            else if(first[root]!=c) complete[root]=false;
        }
        for(VertexId v=0; v<n; ++v) {
            const auto root=graph.find(v);
            // Whole component too small for ANY core: every vertex is an outlier.
            if(static_cast<std::uint64_t>(graph.size[root])-1<mu) {
                keep[v]=false; ++st.completed_noncore_vertices;
                if(v==root) ++st.completed_noncore_components;
            } else if(complete[root]) {
                keep[v]=false; resolved_cluster[v]=graph.minimum[root];
                ++st.completed_core_vertices;
                if(v==root) ++st.completed_core_components;
            } else ++st.residual_vertices;
        }
        for(VertexId w=0; w<index.center_count(); ++w) {
            const auto& row=index.posting(w);
            if(row.empty()) continue;
            if(keep[row[0]]) st.residual_incidences+=row.size();
            else st.avoided_degree_merge_entries+=std::uint64_t(row.size())*row.size();
        }
    } // Release proof workspaces before fallback.
    const auto proof_end=Clock::now();
    st.proof_ms=ms(proof_end-begin);
    PscanOnFliResult residual;
    std::vector<VertexId> original;
    if(st.residual_vertices) {
        if(st.residual_vertices!=n) {
            original.reserve(st.residual_vertices);
            for(VertexId v=0; v<n; ++v) if(keep[v]) original.push_back(v);
            index=index.restrict_to_components(keep);
        }
        index.prepare_exact();
        st.residual_degree_merge_entries=index.stats().degree_merge_entries_read;
        const auto prepared=Clock::now();
        st.residual_prepare_ms=ms(prepared-proof_end);
        residual=run_pscan_on_fli(index,threshold,mu,cache_bytes,nullptr,nullptr,true,
                                 BlockExecutionMode::CoreConnectivity);
        st.residual_scan_ms=ms(Clock::now()-prepared);
    }
    const auto merge_begin=Clock::now();
    auto& r=output.clustering;
    r.stats=residual.stats; // Algorithm work counters describe residual processing.
    r.is_core.assign(n,false);
    r.core_cluster.assign(n,missing);
    r.noncore_clusters.resize(n);
    r.roles.assign(n,PscanVertexRole::Outlier);
    for(VertexId v=0; v<n; ++v) if(!keep[v] && resolved_cluster[v]!=missing) {
        r.is_core[v]=true; r.core_cluster[v]=resolved_cluster[v]; r.roles[v]=PscanVertexRole::Core;
    }
    for(VertexId v=0; v<residual.is_core.size(); ++v) {
        const auto dest=original.empty()?v:original[v];
        r.is_core[dest]=residual.is_core[v]; r.roles[dest]=residual.roles[v];
        if(residual.is_core[v]) {
            const auto id=residual.core_cluster[v];
            r.core_cluster[dest]=original.empty()?id:original[id];
        }
        for(auto id:residual.noncore_clusters[v])
            r.noncore_clusters[dest].push_back(original.empty()?id:original[id]);
    }
    r.stats.core_vertices=r.stats.clusters=r.stats.border_vertices=0;
    r.stats.hub_vertices=r.stats.outlier_vertices=0;
    for(VertexId v=0; v<n; ++v) {
        if(r.is_core[v]) { ++r.stats.core_vertices; if(r.core_cluster[v]==v) ++r.stats.clusters; }
        else if(r.roles[v]==PscanVertexRole::Border) ++r.stats.border_vertices;
        else if(r.roles[v]==PscanVertexRole::Hub) ++r.stats.hub_vertices;
        else ++r.stats.outlier_vertices;
    }
    st.merge_ms=ms(Clock::now()-merge_begin);
    return output;
}
}
