#ifndef HINSCAN_REGION_COMPLETION_H
#define HINSCAN_REGION_COMPLETION_H
#include "scan/PscanOnFli.h"
namespace hinscan {
struct RegionCompletionStats {
    std::uint64_t vertices=0, incidences=0, components=0, largest_component=0;
    std::uint64_t certified_postings=0, certified_core_vertices=0;
    std::uint64_t completed_core_components=0, completed_noncore_components=0;
    std::uint64_t completed_core_vertices=0, completed_noncore_vertices=0;
    std::uint64_t residual_vertices=0, residual_incidences=0;
    // Raw degree-expansion entries avoided, NOT distinct projected edges.
    std::uint64_t avoided_degree_merge_entries=0, residual_degree_merge_entries=0;
    double proof_ms=0, residual_prepare_ms=0, residual_scan_ms=0, merge_ms=0;
};
struct RegionCompletionResult {
    PscanOnFliResult clustering;
    RegionCompletionStats regions;
};
// Low-level mu counts OTHER neighbors, as in run_pscan_on_fli.
// CLI callers map the paper's closed-neighborhood mu exactly once.
RegionCompletionResult run_region_completion(FactorIndex index,
    const SimilarityThreshold& threshold, std::uint64_t mu,
    std::uint64_t cache_bytes=32ULL*1024*1024);
}
#endif
