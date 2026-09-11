#include "scan/SharedGroupDiagnostics.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <numeric>
#include <stdexcept>

namespace hinscan {
namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
}
std::uint64_t needed(std::uint64_t degree,std::uint64_t required) {
    return required>degree?0:degree-required+1;
}
}
SharedGroupDiagnostics::SharedGroupDiagnostics(const FactorIndex& index):index_(index) {
    const auto start=Clock::now();
    const auto n=static_cast<std::size_t>(index.vertex_count());
    offsets_.reserve(n+1); offsets_.push_back(0);
    for (VertexId v=0;v<n;++v) offsets_.push_back(offsets_.back()+index.witnesses(v).size());
    ordered_.reserve(static_cast<std::size_t>(offsets_.back()));
    // Global order favours large postings; ties use witness ID, never parameters.
    for (VertexId v=0;v<n;++v) {
        ordered_.insert(ordered_.end(),index.witnesses(v).begin(),index.witnesses(v).end());
        std::sort(ordered_.begin()+offsets_[v],ordered_.end(),[&](auto a,auto b) {
            const auto x=index.posting(a).size(),y=index.posting(b).size();
            return x!=y?x>y:a<b;
        });
    }
    std::vector<VertexId> order(n); std::iota(order.begin(),order.end(),VertexId{0});
    auto less_witness=[&](auto a,auto b) {
        const auto x=index.posting(a).size(),y=index.posting(b).size();
        return x!=y?x>y:a<b;
    };
    std::sort(order.begin(),order.end(),[&](auto a,auto b) {
        const auto ab=ordered_.begin()+offsets_[a],ae=ordered_.begin()+offsets_[a+1];
        const auto bb=ordered_.begin()+offsets_[b],be=ordered_.begin()+offsets_[b+1];
        if (std::lexicographical_compare(ab,ae,bb,be,less_witness)) return true;
        if (std::lexicographical_compare(bb,be,ab,ae,less_witness)) return false;
        return a<b;
    });
    rank_.resize(n);
    for (std::size_t i=0;i<n;++i) rank_[order[i]]=static_cast<VertexId>(i);
    left_bits_.resize((n+63)/64); missing_bits_.resize(left_bits_.size());
    stats_.metadata_bytes=offsets_.capacity()*sizeof(std::uint64_t)+
        (ordered_.capacity()+rank_.capacity())*sizeof(VertexId);
    stats_.metadata_peak_bytes=stats_.metadata_bytes+order.capacity()*sizeof(VertexId);
    stats_.metadata_ms=elapsed(start); memory();
}
void SharedGroupDiagnostics::memory() {
    const auto batch=pairs_.capacity()*sizeof(Pair)+tasks_.capacity()*sizeof(Task);
    stats_.peak_batch_bytes=std::max<std::uint64_t>(stats_.peak_batch_bytes,batch);
    const auto work=batch+(left_bits_.capacity()+missing_bits_.capacity())*sizeof(std::uint64_t)+
        (left_touched_.capacity()+undo_.capacity())*sizeof(VertexId);
    stats_.peak_workspace_bytes=std::max<std::uint64_t>(stats_.peak_workspace_bytes,work);
}
void SharedGroupDiagnostics::observe(VertexId left,VertexId right,std::uint64_t required,
    bool similar,double ms,std::uint64_t entries,std::uint64_t units) {
    if (left>=index_.vertex_count() || right>=index_.vertex_count() || left==right)
        throw std::logic_error("invalid shared diagnostic edge");
    if (left_!=left) { flush(); left_=left; left_ready_=false; }
    const auto start=Clock::now();
    pairs_.push_back(Pair{right,required,entries,units,ms,similar,false,false});
    ++stats_.calls; stats_.baseline_predicate_ms+=ms;
    stats_.baseline_posting_entries+=entries; stats_.baseline_intersection_units+=units;
    stats_.capture_ms+=elapsed(start);
}
void SharedGroupDiagnostics::certify(Pair& p,bool similar,std::size_t depth) {
    if (p.covered) return;
    if (p.similar!=similar) {
        ++stats_.mismatches;
        throw std::logic_error("shared certificate disagrees with original exact predicate");
    }
    p.covered=true; ++stats_.covered;
    stats_.multi_prefix_covered+=depth>1;
    if (similar) ++stats_.positive; else ++stats_.negative;
    stats_.covered_predicate_ms+=p.ms;
    stats_.covered_posting_entries+=p.entries; stats_.covered_intersection_units+=p.units;
}
void SharedGroupDiagnostics::build_left() {
    if (left_ready_) return;
    const auto start=Clock::now();
    for (auto word:left_touched_) left_bits_[word]=0;
    left_touched_.clear();
    auto visit=[&](VertexId v) {
        const auto word=v>>6U,bit=v&63U;
        if (!left_bits_[word]) left_touched_.push_back(word);
        left_bits_[word]|=std::uint64_t{1}<<bit;
    };
    visit(left_);
    for (auto w:index_.witnesses(left_)) {
        const auto& posting=index_.posting(w);
        stats_.left_build_entries+=posting.size();
        for (auto v:posting) visit(v);
    }
    left_ready_=true; stats_.left_build_ms+=elapsed(start);
}
void SharedGroupDiagnostics::flush() {
    if (pairs_.empty()) return;
    ++stats_.batches;
    auto start=Clock::now();
    std::sort(pairs_.begin(),pairs_.end(),[&](const auto& a,const auto& b) {
        return rank_[a.right]<rank_[b.right];
    });
    for (std::size_t i=1;i<pairs_.size();++i)
        if (pairs_[i-1].right==pairs_[i].right) throw std::logic_error("duplicate actual check in batch");
    stats_.batch_sort_ms+=elapsed(start);
    start=Clock::now(); const auto left_before=stats_.left_build_ms;
    tasks_.push_back(Task{0,pairs_.size(),0,0,0,0,false});
    while (!tasks_.empty()) {
        auto task=tasks_.back(); tasks_.pop_back();
        if (task.exit) {
            while (undo_.size()>task.checkpoint) {
                const auto v=undo_.back(); undo_.pop_back();
                missing_bits_[v>>6U]&=~(std::uint64_t{1}<<(v&63U));
            }
            continue;
        }
        const auto count=task.hi-task.lo;
        if (count<2) continue; // No multi-comparison sharing to diagnose.
        bool unresolved=false;
        for (auto i=task.lo;i<task.hi;++i) unresolved|=!pairs_[i].covered;
        if (!unresolved) continue;
        const auto first=pairs_[task.lo].right,last=pairs_[task.hi-1].right;
        auto depth=task.depth;
        while (depth<std::min(length(first),length(last)) &&
               witness(first,depth)==witness(last,depth)) ++depth;
        const auto checkpoint=undo_.size();
        tasks_.push_back(Task{0,0,0,0,checkpoint,0,true});
        auto sum=task.sum;
        for (auto i=task.depth;i<depth;++i)
            sum=std::min(index_.vertex_count(),sum+index_.posting(witness(first,i)).size());
        auto scanned=task.scanned;
        if (depth>0) {
            ++stats_.groups;
            stats_.multi_prefix_groups+=depth>1;
            stats_.max_group=std::max<std::uint64_t>(stats_.max_group,count);
            stats_.max_prefix=std::max<std::uint64_t>(stats_.max_prefix,depth);
            auto h_upper=sum;
            for (auto i=task.lo;i<task.hi;++i) {
                auto& p=pairs_[i];
                if (!p.shared) { p.shared=true; ++stats_.shared_calls; }
                h_upper=std::min(h_upper,index_.degree(p.right));
                if (!p.covered && count>=p.required) certify(p,true,depth);
            }
            if (h_upper<count) throw std::logic_error("shared support exceeds union upper bound");
            std::uint64_t min_need=~std::uint64_t{0},max_need=0;
            unresolved=false;
            for (auto i=task.lo;i<task.hi;++i) if (!pairs_[i].covered) {
                unresolved=true;
                const auto need=needed(index_.degree(pairs_[i].right),pairs_[i].required);
                min_need=std::min(min_need,need); max_need=std::max(max_need,need);
            }
            if (!unresolved) continue;
            if (h_upper-count<min_need) {
                ++stats_.impossible_groups; // Children may still have stronger prefixes.
            } else {
                ++stats_.eligible_groups; build_left();
                for (auto i=scanned;i<depth && undo_.size()<max_need;++i) {
                    for (auto v:index_.posting(witness(first,i))) {
                        ++stats_.proof_posting_entries;
                        const auto word=v>>6U;
                        const auto bit=std::uint64_t{1}<<(v&63U);
                        if (!(left_bits_[word]&bit) && !(missing_bits_[word]&bit)) {
                            missing_bits_[word]|=bit; undo_.push_back(v);
                            ++stats_.missing_insertions;
                            if (undo_.size()>=max_need) break;
                        }
                    }
                }
                // If this stopped mid-prefix, every remaining pair is rejected
                // and no child will reuse an incorrectly advanced cursor.
                const auto missing=undo_.size();
                unresolved=false;
                for (auto i=task.lo;i<task.hi;++i) if (!pairs_[i].covered) {
                    if (missing>=needed(index_.degree(pairs_[i].right),pairs_[i].required))
                        certify(pairs_[i],false,depth);
                    else unresolved=true;
                }
                if (!unresolved) continue;
                scanned=depth;
            }
        }
        // Compressed local prefix tree over ACTUAL checked right endpoints.
        // Only child ranges are stored, never their posting unions.
        auto lo=task.lo;
        while (lo<task.hi) {
            const auto v=pairs_[lo].right;
            if (length(v)==depth) { ++lo; continue; } // Terminal row cannot extend.
            const auto symbol=witness(v,depth);
            auto hi=lo+1;
            while (hi<task.hi && length(pairs_[hi].right)>depth &&
                   witness(pairs_[hi].right,depth)==symbol) ++hi;
            if (hi-lo>=2) tasks_.push_back(Task{lo,hi,depth,scanned,0,sum,false});
            lo=hi;
        }
    }
    if (!undo_.empty()) throw std::logic_error("shared undo stack not restored");
    for (const auto& p:pairs_) stats_.uncovered+=!p.covered;
    stats_.proof_ms+=elapsed(start)-(stats_.left_build_ms-left_before);
    memory(); pairs_.clear();
}
void SharedGroupDiagnostics::finish() { flush(); memory(); }
void write_shared_group_report(const SharedGroupStats& s,const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    std::ofstream out(directory/"shared-groups.log");
    if (!out) throw std::runtime_error("cannot write shared group diagnostic");
    out.precision(17);
    out<<"diagnostic_only=1\nhindsight_actual_checks=1\n"
       <<"scope=consecutive-left exact checks; prefix support uses future checks in the batch\n"
       <<"timing=instrumented predicate time only, excludes left activation; not a speedup prediction\n"
       <<"proof=group cardinality lower bound and distinct missing-union upper bound\n";
    out<<"calls="<<s.calls<<'\n';
    out<<"batches="<<s.batches<<'\n';
    out<<"groups="<<s.groups<<'\n';
    out<<"shared_calls="<<s.shared_calls<<'\n';
    out<<"max_group="<<s.max_group<<'\n';
    out<<"max_prefix="<<s.max_prefix<<'\n';
    out<<"covered="<<s.covered<<'\n';
    out<<"positive="<<s.positive<<'\n';
    out<<"negative="<<s.negative<<'\n';
    out<<"uncovered="<<s.uncovered<<'\n';
    out<<"mismatches="<<s.mismatches<<'\n';
    out<<"multi_prefix_groups="<<s.multi_prefix_groups<<'\n'
       <<"multi_prefix_covered="<<s.multi_prefix_covered<<'\n';
    out<<"eligible_groups="<<s.eligible_groups<<'\n';
    out<<"impossible_groups="<<s.impossible_groups<<'\n';
    out<<"proof_posting_entries="<<s.proof_posting_entries<<'\n';
    out<<"missing_insertions="<<s.missing_insertions<<'\n';
    out<<"left_build_entries="<<s.left_build_entries<<'\n';
    out<<"baseline_posting_entries="<<s.baseline_posting_entries<<'\n';
    out<<"covered_posting_entries="<<s.covered_posting_entries<<'\n';
    out<<"baseline_intersection_units="<<s.baseline_intersection_units<<'\n';
    out<<"covered_intersection_units="<<s.covered_intersection_units<<'\n';
    out<<"metadata_bytes="<<s.metadata_bytes<<'\n';
    out<<"metadata_peak_bytes="<<s.metadata_peak_bytes<<'\n';
    out<<"peak_batch_bytes="<<s.peak_batch_bytes<<'\n';
    out<<"peak_workspace_bytes="<<s.peak_workspace_bytes<<'\n';
    out<<"metadata_ms="<<s.metadata_ms<<'\n';
    out<<"capture_ms="<<s.capture_ms<<'\n';
    out<<"batch_sort_ms="<<s.batch_sort_ms<<'\n';
    out<<"proof_ms="<<s.proof_ms<<'\n';
    out<<"left_build_ms="<<s.left_build_ms<<'\n';
    out<<"baseline_predicate_ms="<<s.baseline_predicate_ms<<'\n';
    out<<"covered_predicate_ms="<<s.covered_predicate_ms<<'\n';
    const auto cost=s.metadata_ms+s.capture_ms+s.batch_sort_ms+s.proof_ms+s.left_build_ms;
    out<<"diagnostic_cost_ms="<<cost<<'\n'
       <<"potential_predicate_balance_ms="<<s.covered_predicate_ms-cost<<'\n';
    if (!out) throw std::runtime_error("shared group report write failed");
}
} // namespace hinscan
