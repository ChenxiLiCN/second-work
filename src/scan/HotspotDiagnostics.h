#ifndef HINSCAN_HOTSPOT_DIAGNOSTICS_H
#define HINSCAN_HOTSPOT_DIAGNOSTICS_H
#include "index/FactorIndex.h"
#include <vector>

namespace hinscan {
struct NeighborhoodWork {
    std::uint64_t stream_calls=0, stream_entries=0, partial_calls=0;
    std::uint64_t generation_calls=0, generation_entries=0, cached_checks=0;
    double stream_ms=0, generation_ms=0;
    std::uint64_t entries() const { return stream_entries+generation_entries; }
};
struct HotspotTrace {
    explicit HotspotTrace(std::size_t n): rows(n) {}
    std::vector<NeighborhoodWork> rows;
};
// Diagnostic postprocessing only; never influences candidate or cache decisions.
void write_hotspot_report(const FactorIndex&,const HotspotTrace&,
                          const std::filesystem::path& directory);
}
#endif
