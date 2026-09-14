#include "scan/LayeredCompletion.h"
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
// Guard the existing decimal parser against uint64 wrapping before calling it.
void validate_decimal(const std::string& text) {
    std::uint64_t n=0,d=1;bool dot=false,digit=false;
    for(char c:text) {
        if(c=='.' && !dot){dot=true;continue;}
        if(c<'0'||c>'9') throw std::invalid_argument("epsilon must be decimal");
        digit=true;const auto v=static_cast<unsigned>(c-'0');
        if(n>(std::numeric_limits<std::uint64_t>::max()-v)/10 ||
           (dot && d>std::numeric_limits<std::uint64_t>::max()/10))
            throw std::invalid_argument("epsilon decimal exceeds uint64 representation");
        n=n*10+v;if(dot)d*=10;
    }
    if(!digit||n==0||n>d)throw std::invalid_argument("epsilon must be in (0,1]");
}
}
int main(int argc,char** argv) {
    if(argc!=6){std::cerr<<"Usage: mgi_query_index <index-directory> <meta-path> <epsilon> <mu> <output-directory>\n";return 2;}
    try {
        using Clock=std::chrono::steady_clock;
        auto ms=[](Clock::duration d){return std::chrono::duration<double,std::milli>(d).count();};
        const auto start=Clock::now();
        const std::filesystem::path dir=argv[1];
        const auto graph=hinscan::HinGraph::load_binary(dir/"base.bri");
        const auto index=hinscan::MajorityIndex::load(dir/"groups.mgi",graph);
        const auto loaded=Clock::now();
        validate_decimal(argv[3]);
        const auto eps=hinscan::SimilarityThreshold::parse(argv[3]);
        const auto path=hinscan::parse_meta_path(graph,argv[2]);
        const std::string mu_text=argv[4];
        if(mu_text.empty() || mu_text.find_first_not_of("0123456789")!=std::string::npos)
            throw std::invalid_argument("mu must be a positive integer");
        const auto mu=std::stoull(mu_text);
        const auto other_mu=hinscan::pscan_mu_from_hinscan(mu);
        const auto result=hinscan::run_layered_completion(graph,index,path,eps,other_mu);
        const auto done=Clock::now();
        hinscan::write_pscan_on_fli_results(argv[5],argv[3],mu,result.clustering);
        const auto saved=Clock::now();
        const auto& s=result.layers;
        std::cout<<std::setprecision(12)
            <<"algorithm=majority_layered_v1\nsemantics_version=hinscan_nonindependent_v1\nmu_counts_self=1\n"
            <<"mu="<<mu<<"\npscan_other_mu="<<other_mu<<"\nfallback_block_mode=9\n"
            <<"online_compute_ms="<<ms(done-loaded)<<"\nindex_load_ms="<<ms(loaded-start)
            <<"\noutput_write_ms="<<ms(saved-done)<<'\n'
            <<"layer_proof_ms="<<s.layer_proof_ms<<"\nresidual_prepare_ms="<<s.residual_prepare_ms
            <<"\nresidual_scan_ms="<<s.residual_scan_ms<<"\nresult_merge_ms="<<s.result_merge_ms<<'\n'
            <<"fast_path_supported="<<s.fast_path_supported<<"\nquery_components="<<s.components
            <<"\ncompleted_core_vertices="<<s.completed_core_vertices<<"\ncompleted_noncore_vertices="<<s.completed_noncore_vertices
            <<"\nearly_core_vertices="<<s.early_core_vertices<<"\nscalar_core_vertices="<<s.scalar_core_vertices
            <<"\nresidual_vertices="<<s.residual_vertices<<"\nresidual_incidences="<<s.residual_incidences
            <<"\nresidual_half_expansion_entries="<<s.residual_half_expansion_entries<<"\naccepted_groups="<<s.accepted_groups
            <<"\ncomponent_raw_reads="<<s.component_raw_reads<<"\nprofile_raw_reads="<<s.profile_raw_reads
            <<"\nlift_raw_reads="<<s.lift_raw_reads<<"\ncoverage_member_reads="<<s.coverage_member_reads
            <<"\ncertificate_member_reads="<<s.certificate_member_reads<<"\nlift_member_reads="<<s.lift_member_reads
            <<"\ncore_vertices="<<result.clustering.stats.core_vertices<<"\nclusters="<<result.clustering.stats.clusters<<'\n';
        // Scan counters survive the merge; core/cluster totals above include completed components.
        const auto& q=result.clustering.stats;
        const auto& f=result.residual_factor;
        const auto& a=q.adaptive_cache;
        std::cout<<"residual_diagnostics_version=1\n"
            <<"residual_used_roundtrip_metadata="<<f.used_roundtrip_metadata<<'\n'
            <<"residual_half_expansion_ms="<<f.half_expansion_ms<<'\n'
            <<"residual_degree_compute_ms="<<f.degree_compute_ms<<'\n'
            <<"residual_posting_order_ms="<<f.posting_order_ms<<'\n'
            <<"residual_degree_merge_entries_read="<<f.degree_merge_entries_read<<'\n'
            <<"residual_projected_edges="<<f.exact_projected_edges<<'\n'
            <<"residual_prune_ms="<<q.prune_ms<<'\n'
            <<"residual_core_ms="<<q.core_ms<<'\n'
            <<"residual_noncore_ms="<<q.noncore_ms<<'\n'
            <<"residual_role_ms="<<q.role_ms<<'\n'
            <<"residual_block_discovery_ms="<<q.block_discovery_ms<<'\n'
            <<"residual_certified_blocks="<<q.certified_blocks<<'\n'
            <<"residual_block_core_vertices="<<q.block_core_vertices<<'\n'
            <<"residual_candidate_vertices_emitted="<<q.candidate_vertices_emitted<<'\n'
            <<"residual_candidate_posting_entries_read="<<q.degree_bucket_posting_entries_read<<'\n'
            <<"residual_exact_similarity_checks="<<q.exact_similarity_checks<<'\n'
            <<"residual_certificate_entries="<<q.sparse_certificate_entries<<'\n'
            <<"residual_adaptive_posting_entries="<<a.posting_entries<<'\n'
            <<"residual_adaptive_streaming_entries="<<a.streaming_posting_entries<<'\n'
            <<"residual_adaptive_streaming_checks="<<a.streaming_checks<<'\n'
            <<"residual_adaptive_intersection_units="<<a.intersection_units<<'\n'
            <<"residual_adaptive_generation_ms="<<a.generation_ms<<'\n'
            <<"residual_adaptive_streaming_ms="<<a.streaming_ms<<'\n'
            <<"residual_adaptive_hits="<<a.hits<<'\n'
            <<"residual_adaptive_misses="<<a.misses<<'\n'
            <<"residual_adaptive_evictions="<<a.evictions<<'\n'
            <<"residual_adaptive_distinct_factor_rows="<<a.distinct_factor_rows<<'\n'
            <<"residual_witness_bound_checks="<<a.witness_bound_checks<<'\n'
            <<"residual_witness_bound_accepts="<<a.witness_bound_accepts<<'\n'
            <<"residual_witness_bound_rejects="<<a.witness_bound_rejects<<'\n'
            <<"residual_witness_entries_read="<<a.witness_entries_read<<'\n'
            <<"residual_witness_counts_built="<<a.witness_counts_built<<'\n'
            <<"residual_activation_calls="<<a.activation_calls<<'\n'
            <<"residual_activation_full_words_written="<<a.activation_full_words_written<<'\n';
        return 0;
    } catch(const std::exception& e){std::cerr<<"mgi_query_index: "<<e.what()<<'\n';return 1;}
}
