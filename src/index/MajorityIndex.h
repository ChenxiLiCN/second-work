#ifndef HINSCAN_MAJORITY_INDEX_H
#define HINSCAN_MAJORITY_INDEX_H
#include "hin/HinGraph.h"
#include <filesystem>
#include <vector>
namespace hinscan {
struct MajorityDirection {
    std::vector<std::uint64_t> offsets{0};
    std::vector<VertexId> members;
};
// The immutable graph passed to build/load must outlive this index.
class MajorityIndex {
public:
    static MajorityIndex build(const HinGraph&);
    static MajorityIndex load(const std::filesystem::path&, const HinGraph&);
    void save(const std::filesystem::path&, const HinGraph&) const;
    const MajorityDirection& groups_for(const HinGraph&, const Transition&) const;
    std::uint64_t group_count() const noexcept;
    std::uint64_t member_count() const noexcept;
private:
    const HinGraph* graph_ = nullptr;
    std::uint64_t fingerprint_ = 0;
    std::vector<MajorityDirection> directions_;
};
}
#endif
