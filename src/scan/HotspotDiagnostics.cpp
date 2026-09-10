#include "scan/HotspotDiagnostics.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <stdexcept>

namespace hinscan {
void write_hotspot_report(const FactorIndex& index,const HotspotTrace& trace,
                          const std::filesystem::path& directory) {
    if (trace.rows.size()!=index.vertex_count()) throw std::logic_error("hotspot trace size mismatch");
    std::filesystem::create_directories(directory);
    std::ofstream summary(directory/"hotspot-summary.log"), detail(directory/"hotspots.tsv");
    if (!summary || !detail) throw std::runtime_error("cannot write hotspot report");
    summary<<std::setprecision(12); detail<<std::setprecision(12);
    std::vector<VertexId> ranked;
    std::uint64_t entries=0, stream_entries=0, generation_entries=0, stream_calls=0;
    std::uint64_t repeated_entries=0, repeated_rows=0, partial_calls=0, full_once=0;
    double row_ms=0;
    for (VertexId u=0;u<trace.rows.size();++u) {
        const auto& r=trace.rows[u];
        entries+=r.entries(); stream_entries+=r.stream_entries; generation_entries+=r.generation_entries;
        stream_calls+=r.stream_calls; partial_calls+=r.partial_calls;
        row_ms+=r.stream_ms+r.generation_ms;
        if (!r.entries()) continue;
        ranked.push_back(u);
        if (r.stream_calls+r.generation_calls>1) { ++repeated_rows; repeated_entries+=r.entries(); }
        for (auto w:index.witnesses(u)) full_once+=index.posting(w).size();
    }
    std::sort(ranked.begin(),ranked.end(),[&](auto a,auto b) {
        return trace.rows[a].entries()!=trace.rows[b].entries() ?
            trace.rows[a].entries()>trace.rows[b].entries() : a<b;
    });
    auto share=[&](std::uint64_t value) { return entries?double(value)/double(entries):0.0; };
    summary<<"diagnostic_only=1\ncoverage_selection=largest_postings_not_optimal_subset\n"
           <<"target_vertices="<<index.vertex_count()<<"\nworking_canonical_rows="<<ranked.size()
           <<"\ntrace_bytes="<<trace.rows.capacity()*sizeof(NeighborhoodWork)
           <<"\nscan_entries="<<entries<<"\nstream_entries="<<stream_entries
           <<"\ngeneration_entries="<<generation_entries<<"\nstream_calls="<<stream_calls
           <<"\npartial_calls="<<partial_calls<<"\nrow_operation_ms="<<row_ms
           <<"\nrepeated_rows="<<repeated_rows<<"\nrepeated_row_work_share="<<share(repeated_entries)
           <<"\none_full_expansion_per_working_row_entries="<<full_once
           <<"\nentry_amplification_vs_full_once="<<(full_once?double(entries)/double(full_once):0.0)<<'\n';
    for (auto limit:{1U,10U,100U,1000U}) {
        std::uint64_t sum=0;
        for (std::size_t i=0;i<std::min<std::size_t>(limit,ranked.size());++i) sum+=trace.rows[ranked[i]].entries();
        summary<<"top_"<<limit<<"_rows_work_share="<<share(sum)<<'\n';
    }
    detail<<"rank\tcanonical_vertex\tclosed_degree\twitness_count\tstream_calls\tstream_entries\tpartial_calls"
          <<"\tgeneration_calls\tgeneration_entries\tcached_checks\trow_operation_ms\tone_full_expansion_entries"
          <<"\tlist_or_dense_payload_bytes\tcoverage_complete\tlargest1_union\tlargest2_union\tlargest4_union"
          <<"\tcoverage1\tcoverage2\tcoverage4\tany4_union_upper\tany4_coverage_upper\n";
    // Reporting limits only, not algorithm parameters. Never scan all projected
    // neighborhoods to produce a diagnostic. Record incomplete coverage explicitly.
    constexpr std::size_t report_rows=512;
    constexpr std::uint64_t coverage_scan_budget=50000000;
    std::uint64_t coverage_scanned=0, covered_work=0, reported_work=0, complete_rows=0;
    long double weighted1=0,weighted2=0,weighted4=0;
    std::vector<std::uint32_t> stamps(index.vertex_count(),0);
    std::uint32_t epoch=0;
    for (std::size_t rank=0;rank<std::min(report_rows,ranked.size());++rank) {
        const auto u=ranked[rank]; const auto& r=trace.rows[u];
        const auto degree=index.degree(u);
        auto witnesses=index.witnesses(u);
        std::uint64_t once=0;
        for (auto w:witnesses) once+=index.posting(w).size();
        const auto used=std::min<std::size_t>(4,witnesses.size());
        std::partial_sort(witnesses.begin(),witnesses.begin()+used,witnesses.end(),[&](auto a,auto b) {
            return index.posting(a).size()!=index.posting(b).size() ?
                index.posting(a).size()>index.posting(b).size() : a<b;
        });
        std::uint64_t requested=0;
        for (std::size_t i=0;i<used;++i) requested+=index.posting(witnesses[i]).size();
        const auto upper=std::min<std::uint64_t>(degree,requested);
        const bool complete=requested<=coverage_scan_budget-coverage_scanned;
        std::uint64_t union_count=0, counts[4]={0,0,0,0};
        if (complete) {
            ++epoch;
            for (std::size_t i=0;i<4;++i) {
                if (i<used) for (auto v:index.posting(witnesses[i])) {
                    ++coverage_scanned;
                    if (stamps[v]!=epoch) { stamps[v]=epoch; ++union_count; }
                }
                counts[i]=union_count;
            }
            if (union_count>degree || counts[0]>counts[1] || counts[1]>counts[3])
                throw std::logic_error("invalid exact witness coverage");
            ++complete_rows; covered_work+=r.entries();
            weighted1+=static_cast<long double>(r.entries())*counts[0]/degree;
            weighted2+=static_cast<long double>(r.entries())*counts[1]/degree;
            weighted4+=static_cast<long double>(r.entries())*counts[3]/degree;
        }
        reported_work+=r.entries();
        detail<<rank+1<<'\t'<<u<<'\t'<<degree<<'\t'<<witnesses.size()<<'\t'
              <<r.stream_calls<<'\t'<<r.stream_entries<<'\t'<<r.partial_calls<<'\t'
              <<r.generation_calls<<'\t'<<r.generation_entries<<'\t'<<r.cached_checks<<'\t'
              <<r.stream_ms+r.generation_ms<<'\t'<<once<<'\t'
              <<std::min<std::uint64_t>(degree*4,((index.vertex_count()+63)/64)*8)<<'\t'<<complete;
        if (complete) detail<<'\t'<<counts[0]<<'\t'<<counts[1]<<'\t'<<counts[3]<<'\t'
                            <<double(counts[0])/degree<<'\t'<<double(counts[1])/degree<<'\t'<<double(counts[3])/degree;
        else detail<<"\t\t\t\t\t\t";
        detail<<'\t'<<upper<<'\t'<<double(upper)/degree<<'\n';
    }
    summary<<"reported_rows="<<std::min(report_rows,ranked.size())
           <<"\nreported_work_share="<<share(reported_work)
           <<"\ncoverage_complete_rows="<<complete_rows
           <<"\ncoverage_scanned_entries="<<coverage_scanned
           <<"\ncoverage_work_share="<<share(covered_work)
           <<"\ncoverage1_work_weighted="<<(covered_work?weighted1/covered_work:0)
           <<"\ncoverage2_work_weighted="<<(covered_work?weighted2/covered_work:0)
           <<"\ncoverage4_work_weighted="<<(covered_work?weighted4/covered_work:0)<<'\n';
    summary.flush(); detail.flush();
    if (!summary || !detail) throw std::runtime_error("hotspot report write failed");
}
}
