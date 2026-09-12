#include "scan/RegionCompletion.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>

int main(int argc, char** argv) {
    if(argc!=6) {
        std::cerr << "Usage: bri_query_regions <base.bri> <meta-path> <epsilon> <mu> <output-directory>\n";
        return 2;
    }
    try {
        using Clock=std::chrono::steady_clock;
        auto ms=[](Clock::duration d) { return std::chrono::duration<double,std::milli>(d).count(); };
        const auto eps=hinscan::SimilarityThreshold::parse(argv[3]);
        std::size_t consumed=0;
        const auto mu=std::stoull(argv[4],&consumed);
        if(mu==0 || argv[4][0]=='-' || consumed!=std::string(argv[4]).size())
            throw std::invalid_argument("mu must be positive");
        const auto pscan_mu=hinscan::pscan_mu_from_hinscan(mu);
        const auto start=Clock::now();
        const auto graph=hinscan::HinGraph::load_binary(argv[1]);
        const auto loaded=Clock::now();
        const auto path=hinscan::parse_meta_path(graph,argv[2]);
        auto factor=hinscan::FactorIndex::build(graph,path,false);
        const auto built=Clock::now();
        const auto result=hinscan::run_region_completion(std::move(factor),eps,pscan_mu);
        const auto done=Clock::now();
        hinscan::write_pscan_on_fli_results(argv[5],argv[3],mu,result.clustering);
        const auto saved=Clock::now();
        const auto& r=result.regions;
        const auto& q=result.clustering.stats;
        std::cout << "algorithm=region_completion_v1\n"
            << "semantics_version=hinscan_nonindependent_v1\n"
            << "mu_counts_self=1\nmu=" << mu << '\n'
            << "pscan_other_mu=" << pscan_mu << '\n'
            << "role_ms=" << q.role_ms << '\n'
            << "role_workspace_bytes=" << q.role_workspace_bytes << '\n'
            << "fallback_block_mode=9\ncache_budget_mib=32\n"
            << "used_roundtrip_metadata=0\n"
            << "index_load_ms=" << ms(loaded-start) << '\n'
            << "relation_prepare_ms=" << ms(built-loaded) << '\n'
            << "region_proof_ms=" << r.proof_ms << '\n'
            << "residual_prepare_ms=" << r.residual_prepare_ms << '\n'
            << "residual_scan_ms=" << r.residual_scan_ms << '\n'
            << "result_merge_ms=" << r.merge_ms << '\n'
            << "online_compute_ms=" << ms(done-loaded) << '\n'
            << "output_write_ms=" << ms(saved-done) << '\n'
            << "vertices=" << r.vertices << '\n'
            << "incidences=" << r.incidences << '\n'
            << "query_components=" << r.components << '\n'
            << "largest_query_component=" << r.largest_component << '\n'
            << "certified_postings=" << r.certified_postings << '\n'
            << "certified_core_vertices=" << r.certified_core_vertices << '\n'
            << "completed_core_components=" << r.completed_core_components << '\n'
            << "completed_noncore_components=" << r.completed_noncore_components << '\n'
            << "completed_core_vertices=" << r.completed_core_vertices << '\n'
            << "completed_noncore_vertices=" << r.completed_noncore_vertices << '\n'
            << "residual_vertices=" << r.residual_vertices << '\n'
            << "residual_incidences=" << r.residual_incidences << '\n'
            << "avoided_degree_merge_entries=" << r.avoided_degree_merge_entries << '\n'
            << "residual_degree_merge_entries=" << r.residual_degree_merge_entries << '\n'
            << "residual_candidate_vertices=" << q.candidate_vertices_emitted << '\n'
            << "residual_candidate_posting_entries=" << q.degree_bucket_posting_entries_read << '\n'
            << "residual_exact_similarity_checks=" << q.exact_similarity_checks << '\n'
            << "residual_witness_counts_built=" << q.adaptive_cache.witness_counts_built << '\n'
            << "core_vertices=" << q.core_vertices << '\n'
            << "clusters=" << q.clusters << '\n';
        return 0;
    } catch(const std::exception& e) {
        std::cerr << "bri_query_regions: " << e.what() << '\n'; return 1;
    }
}
