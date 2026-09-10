#include "scan/AnchorFilter.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace hinscan {
namespace {
using Clock=std::chrono::steady_clock;
struct Timer {
    double& total;
    Clock::time_point start=Clock::now();
    ~Timer() { total+=std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
};
}

AnchorFilter::AnchorFilter(const FactorIndex& index, std::uint64_t state_budget)
    : index_(index) {
    Timer timer{stats_.prepare_ms};
    anchors_.assign(index.vertex_count(),missing);
    for (VertexId u=0; u<anchors_.size(); ++u) {
        std::uint64_t largest=0;
        for (auto w:index.witnesses(u)) {
            ++stats_.selection_entries;
            const auto size=index.posting(w).size();
            if (size>largest || (size==largest && w<anchors_[u])) {
                largest=size; anchors_[u]=w;
            }
        }
        if (anchors_[u]==missing) continue; // Isolated: closed neighborhood {u}.
        if (largest>index.degree(u)) throw std::logic_error("anchor exceeds closed neighborhood");
        ++stats_.anchored_vertices;
        stats_.coverage_55_vertices+=100*largest>55*index.degree(u);
        stats_.coverage_75_vertices+=100*largest>75*index.degree(u);
    }
    // Internal bounded direct-mapped memo. Eviction discards work, never facts
    // used for a different pair. Zero budget is supported for correctness tests.
    std::size_t slots=0;
    if (state_budget>=sizeof(Progress)) {
        slots=1;
        while (slots<65536 && 2*slots*sizeof(Progress)<=state_budget) slots*=2;
    }
    states_.resize(slots);
    stats_.mapping_bytes=anchors_.capacity()*sizeof(VertexId);
    stats_.state_bytes=states_.capacity()*sizeof(Progress);
}

bool AnchorFilter::reject(VertexId left, VertexId right, std::uint64_t required) {
    Timer timer{stats_.filter_ms};
    ++stats_.calls;
    auto a=anchors_.at(left), b=anchors_.at(right);
    if (a==missing || b==missing) { ++stats_.no_anchor; return false; }
    const auto ra=index_.degree(left)-index_.posting(a).size();
    const auto rb=index_.degree(right)-index_.posting(b).size();
    // Guard unsigned subtraction. t<=0 cannot reject even with zero overlap.
    if (ra>=required || rb>=required-ra) { ++stats_.ineligible; return false; }
    ++stats_.eligible;
    const auto threshold=required-ra-rb;
    if (a==b) {
        ++stats_.same_anchor;
        if (index_.posting(a).size()<threshold) { ++stats_.rejects; return true; }
        ++stats_.fallbacks; return false;
    }
    if (a>b) std::swap(a,b); // Canonical orientation also fixes cursor meanings.
    const auto key=(std::uint64_t{a}<<32U)|b;
    Progress local;
    Progress* state=&local;
    bool hit=false;
    if (!states_.empty()) {
        auto hash=key;
        hash^=hash>>30U; hash*=0xbf58476d1ce4e5b9ULL;
        hash^=hash>>27U; hash*=0x94d049bb133111ebULL; hash^=hash>>31U;
        state=&states_[hash&(states_.size()-1)];
        hit=state->key==key;
        if (hit) ++stats_.cache_hits;
        else {
            ++stats_.cache_misses;
            if (state->key!=~std::uint64_t{0}) ++stats_.replacements;
            *state=Progress{}; state->key=key;
        }
    } else ++stats_.cache_misses;
    const auto& first=index_.posting(a);
    const auto& second=index_.posting(b);
    bool advanced=false;
    for (;;) {
        if (state->common>=threshold) {
            ++stats_.fallbacks;
            if (hit && !advanced) ++stats_.cached_fallbacks;
            return false;
        }
        const auto upper=state->common+std::min(first.size()-state->left_pos,
                                               second.size()-state->right_pos);
        if (upper<threshold) {
            ++stats_.rejects;
            if (hit && !advanced) ++stats_.cached_rejects;
            return true;
        }
        // The bounds above guarantee both cursors are within their arrays.
        if (hit && !advanced) ++stats_.resumed;
        advanced=true;
        ++stats_.comparisons;
        const auto x=first[state->left_pos], y=second[state->right_pos];
        if (x<=y) { ++state->left_pos; ++stats_.entries_advanced; }
        if (y<=x) { ++state->right_pos; ++stats_.entries_advanced; }
        if (x==y) ++state->common;
    }
}
}
