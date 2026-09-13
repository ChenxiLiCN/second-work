#ifndef HINSCAN_LAYERED_COMPLETION_H
#define HINSCAN_LAYERED_COMPLETION_H

#include "index/MajorityIndex.h"
#include "scan/PscanOnFli.h"

namespace hinscan {

struct LayeredCompletionStats {
    std::uint64_t vertices = 0, components = 0;
    std::uint64_t completed_core_vertices = 0, completed_noncore_vertices = 0;
    std::uint64_t early_core_vertices = 0, scalar_core_vertices = 0;
    std::uint64_t residual_vertices = 0, residual_incidences = 0;
    std::uint64_t residual_half_expansion_entries = 0, accepted_groups = 0;
    std::uint64_t component_raw_reads = 0, profile_raw_reads = 0, lift_raw_reads = 0;
    std::uint64_t coverage_member_reads = 0, certificate_member_reads = 0, lift_member_reads = 0;
    bool fast_path_supported = false;
    double layer_proof_ms = 0, residual_prepare_ms = 0, residual_scan_ms = 0;
    double result_merge_ms = 0;
};

struct LayeredCompletionResult {
    PscanOnFliResult clustering;
    LayeredCompletionStats layers;
};

// Low-level mu counts OTHER neighbors. The CLI maps self-inclusive mu once.
// Completed sources never enter FactorIndex construction.
LayeredCompletionResult run_layered_completion(
    const HinGraph& graph, const MajorityIndex& index,
    const std::vector<std::uint32_t>& path,
    const SimilarityThreshold& threshold, std::uint64_t pscan_other_mu);

}  // namespace hinscan
#endif
