#ifndef HINSCAN_EXACT_NEIGHBORHOOD_CACHE_H
#define HINSCAN_EXACT_NEIGHBORHOOD_CACHE_H

#include "index/FactorIndex.h"
#include "scan/HotspotDiagnostics.h"
#include <memory>
#include <vector>

namespace hinscan {

struct ExactNeighborhoodCacheStats {
    // Always present for ABI consistency; allocated only in diagnostic builds.
    std::shared_ptr<HotspotTrace> hotspots;
    std::uint64_t hits = 0, misses = 0, evictions = 0;
    std::uint64_t posting_entries = 0, peak_bytes = 0;
    std::uint64_t list_checks = 0, bitmap_checks = 0, intersection_units = 0;
    std::uint64_t distinct_factor_rows = 0;
    std::uint64_t equal_factor_checks = 0;
    std::uint64_t pair_bound_hits = 0, pair_bound_bytes = 0;
    std::uint64_t streaming_checks = 0, streaming_posting_entries = 0;
    double streaming_ms = 0;
    double generation_ms = 0;
    double factor_sharing_ms = 0;
    std::uint64_t witness_bound_checks = 0, witness_bound_accepts = 0, witness_bound_rejects = 0;
    std::uint64_t witness_count_hits = 0, witness_counts_built = 0, witness_entries_read = 0;
    std::uint64_t witness_bitmap_bytes = 0, witness_bitmap_build_entries = 0;
    std::uint64_t witness_bitmap_checks = 0, witness_bitmap_words = 0;
    double witness_bitmap_build_ms = 0;
    std::uint64_t single_pass_complete = 0, single_pass_partial = 0;
    std::uint64_t single_pass_early_accepts = 0, single_pass_early_rejects = 0;
    std::uint64_t witness_exclusion_rejects = 0;
    std::uint64_t activation_calls = 0, activation_full_words_written = 0;
    std::uint64_t activation_sparse_words_cleared = 0;
    std::uint64_t streaming_bound_evaluations = 0, streaming_duplicate_visits = 0;
    std::uint64_t resume_checks=0, resume_hits=0, resume_new_rows=0;
    std::uint64_t resume_replay_units=0, resume_new_entries=0, resume_skipped_prefix_entries=0;
    std::uint64_t resume_partial_saves=0, resume_partial_drops=0, resume_partial_evictions=0;
    std::uint64_t resume_promotions=0, resume_left_completions=0;
    double resume_replay_ms=0, resume_save_ms=0;
};

// Query-local exact neighborhoods and pair bounds; nothing is persisted in
// the offline index. Optional partial rows share the SAME LRU byte budget.
class ExactNeighborhoodCache {
public:
    ExactNeighborhoodCache(const FactorIndex& index, std::uint64_t byte_budget,
                           bool witness_bounds = false, bool pressure_only = false,
                           bool witness_bitmaps = false, bool witness_exclusion = false,
                           bool single_pass = false, bool lean_workspaces = false,
                           bool resumable = false);
    void activate(VertexId vertex);
    bool check(VertexId right, std::uint64_t required);
    const ExactNeighborhoodCacheStats& stats() const { return stats_; }
    std::uint64_t workspace_bytes() const;

private:
    enum class Kind { List, SparseBitmap, DenseBitmap };
    struct Row {
        Kind kind = Kind::List;
        std::vector<VertexId> ids;
        std::vector<std::uint64_t> words;
        // Known distinct count. A value below index.degree(v) marks a partial
        // row, whose words end with {witness, offset, raw-prefix-count}.
        // Complete rows have no cursor suffix; get() completes partials first.
        std::uint64_t degree = 0;
        std::uint64_t bytes() const {
            return sizeof(Row) + ids.capacity() * sizeof(VertexId) +
                   words.capacity() * sizeof(std::uint64_t);
        }
    };
    static constexpr VertexId missing = ~VertexId{0};
    // Position of the next UNREAD raw posting entry, not a distinct-neighbor offset.
    struct ResumeCursor { std::uint64_t witness=0, offset=0, scanned=0; };
    struct ResumeResult { bool similar=false; std::uint64_t lower=0, upper=0; };
    ResumeResult resume(VertexId vertex,std::uint64_t required,bool complete_required);
    bool is_partial(VertexId v) const { return rows_[v] && rows_[v]->degree<index_.degree(v); }
    const Row& get(VertexId vertex);
    void unlink(VertexId vertex);
    void touch(VertexId vertex);
    std::unique_ptr<Row> generate(VertexId vertex);
    std::unique_ptr<Row> encode_scratch(VertexId vertex,
        std::uint64_t known=~std::uint64_t{0},const ResumeCursor* cursor=nullptr);
    const Row& admit(VertexId vertex, std::unique_ptr<Row> row);
    struct PairBound {
        std::uint64_t key = ~std::uint64_t{0};
        std::uint64_t lower = 0, upper = ~std::uint64_t{0};
    };
    std::vector<PairBound> pair_bounds_;
    bool witness_bounds_ = false;
    bool pressure_only_ = false;
    bool witness_exclusion_ = false;
    bool single_pass_ = false;
    bool resumable_ = false;
    bool lean_workspaces_ = false, active_dense_ = false;
    std::vector<std::uint64_t> witness_counts_;
    std::vector<std::uint32_t> witness_epochs_;
    std::uint32_t witness_epoch_ = 0;
    std::vector<std::unique_ptr<Row>> witness_rows_;
    std::vector<bool> witness_prepared_;
    std::uint64_t witness_bitmap_budget_ = 0;
    std::uint64_t count_witness(VertexId witness);

    const FactorIndex& index_;
    std::uint64_t budget_, bytes_ = 0;
    std::vector<std::unique_ptr<Row>> rows_;
    std::vector<VertexId> previous_, next_;
    std::vector<VertexId> canonical_;
    VertexId head_ = missing, tail_ = missing;
    VertexId active_vertex_ = missing;
    std::unique_ptr<Row> transient_;
    std::vector<std::uint64_t> scratch_, active_;
    std::vector<VertexId> touched_;
    std::vector<VertexId> active_touched_;
    ExactNeighborhoodCacheStats stats_;
};

} // namespace hinscan
#endif
