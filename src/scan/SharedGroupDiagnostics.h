#ifndef HINSCAN_SHARED_GROUP_DIAGNOSTICS_H
#define HINSCAN_SHARED_GROUP_DIAGNOSTICS_H
#include "index/FactorIndex.h"
#include <vector>

namespace hinscan {
struct SharedGroupStats {
    std::uint64_t calls=0,batches=0,groups=0,shared_calls=0,max_group=0,max_prefix=0;
    std::uint64_t covered=0,positive=0,negative=0,uncovered=0,mismatches=0;
    std::uint64_t multi_prefix_groups=0,multi_prefix_covered=0;
    std::uint64_t eligible_groups=0,impossible_groups=0,proof_posting_entries=0;
    std::uint64_t missing_insertions=0,left_build_entries=0,baseline_posting_entries=0;
    std::uint64_t covered_posting_entries=0,baseline_intersection_units=0,covered_intersection_units=0;
    std::uint64_t metadata_bytes=0,metadata_peak_bytes=0,peak_batch_bytes=0,peak_workspace_bytes=0;
    double metadata_ms=0,capture_ms=0,batch_sort_ms=0,proof_ms=0,left_build_ms=0;
    double baseline_predicate_ms=0,covered_predicate_ms=0;
};
// Offline index is untouched. Analyse actual consecutive-left checks AFTER
// their original decisions. This is a hindsight opportunity diagnostic, not
// an executable scheduler or a prediction of end-to-end speedup.
class SharedGroupDiagnostics {
public:
    explicit SharedGroupDiagnostics(const FactorIndex&);
    void observe(VertexId left,VertexId right,std::uint64_t required,bool similar,
                 double predicate_ms,std::uint64_t posting_entries,std::uint64_t intersection_units);
    void finish();
    const SharedGroupStats& stats() const { return stats_; }
private:
    struct Pair {
        VertexId right=0;
        std::uint64_t required=0,entries=0,units=0;
        double ms=0;
        bool similar=false,covered=false,shared=false;
    };
    struct Task {
        std::size_t lo=0,hi=0,depth=0,scanned=0,checkpoint=0;
        std::uint64_t sum=0;
        bool exit=false;
    };
    void flush();
    void build_left();
    void certify(Pair&,bool,std::size_t depth);
    void memory();
    std::size_t length(VertexId v) const { return offsets_[v+1]-offsets_[v]; }
    VertexId witness(VertexId v,std::size_t i) const { return ordered_[offsets_[v]+i]; }
    const FactorIndex& index_;
    std::vector<std::uint64_t> offsets_,left_bits_,missing_bits_;
    std::vector<VertexId> ordered_,rank_,left_touched_,undo_;
    std::vector<Pair> pairs_;
    std::vector<Task> tasks_;
    VertexId left_=~VertexId{0};
    bool left_ready_=false;
    SharedGroupStats stats_;
};
void write_shared_group_report(const SharedGroupStats&,const std::filesystem::path&);
}
#endif
