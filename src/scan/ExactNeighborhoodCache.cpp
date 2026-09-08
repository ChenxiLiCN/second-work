#include "scan/ExactNeighborhoodCache.h"
#include "scan/QueryProfile.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <numeric>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace hinscan {
namespace {
unsigned popcount(std::uint64_t x) {
    // Portable SWAR; compilers targeting POPCNT recognize this expression.
    x -= (x >> 1) & 0x5555555555555555ULL;
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    return static_cast<unsigned>((((x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL) *
                                  0x0101010101010101ULL) >> 56);
}
unsigned lowest_bit(std::uint64_t x) {
#if defined(_MSC_VER)
    unsigned long bit;
    _BitScanForward64(&bit, x);
    return bit;
#else
    return static_cast<unsigned>(__builtin_ctzll(x));
#endif
}

using BitmapCounter = std::uint64_t (*)(const std::uint64_t*, const std::uint64_t*,
                                      const VertexId*, std::size_t);
std::uint64_t portable_count(const std::uint64_t* active, const std::uint64_t* row,
                             const VertexId* ids, std::size_t count) {
    std::uint64_t common = 0;
    if (ids) {
        for (std::size_t i = 0; i < count; ++i) common += popcount(active[ids[i]] & row[i]);
    } else {
        for (std::size_t i = 0; i < count; ++i) common += popcount(active[i] & row[i]);
    }
    return common;
}
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
__attribute__((target("popcnt")))
std::uint64_t hardware_count(const std::uint64_t* active, const std::uint64_t* row,
                             const VertexId* ids, std::size_t count) {
    std::uint64_t common = 0;
    if (ids) {
        for (std::size_t i = 0; i < count; ++i)
            common += __builtin_popcountll(active[ids[i]] & row[i]);
    } else {
        for (std::size_t i = 0; i < count; ++i)
            common += __builtin_popcountll(active[i] & row[i]);
    }
    return common;
}
#endif
BitmapCounter bitmap_counter() {
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    return __builtin_cpu_supports("popcnt") ? hardware_count : portable_count;
#else
    return portable_count;
#endif
}
}

ExactNeighborhoodCache::ExactNeighborhoodCache(const FactorIndex& index,
                                               std::uint64_t byte_budget,
                                               bool witness_bounds, bool pressure_only,
                                               bool witness_bitmaps, bool witness_exclusion)
    : index_(index), budget_(byte_budget), rows_(index.vertex_count()),
      previous_(index.vertex_count(), missing), next_(index.vertex_count(), missing),
      scratch_((index.vertex_count() + 63) / 64, 0), active_(scratch_.size(), 0) {
    const auto start = std::chrono::steady_clock::now();
    witness_bounds_ = witness_bounds;
    pressure_only_ = pressure_only;
    witness_exclusion_ = witness_exclusion;
    if (witness_bitmaps) {
        witness_rows_.resize(index.center_count());
        witness_prepared_.resize(index.center_count(), false);
        witness_bitmap_budget_ = byte_budget;
    }
    if (witness_bounds_) {
        witness_counts_.resize(index.center_count());
        witness_epochs_.resize(index.center_count(), 0);
    }
    // A direct-mapped table uses at most 1/16 of the SAME cache budget, capped
    // at 65536 records. Collisions evict a record, never approximate equality.
    std::size_t bound_slots = 0;
    if (sizeof(PairBound) <= byte_budget / 16) {
        bound_slots = 1;
        while (bound_slots < 65536 && 2 * bound_slots * sizeof(PairBound) <= byte_budget / 16)
            bound_slots *= 2;
    }
    pair_bounds_.resize(bound_slots);
    stats_.pair_bound_bytes = pair_bounds_.capacity() * sizeof(PairBound);
    budget_ -= stats_.pair_bound_bytes;
    canonical_.resize(index.vertex_count());
    std::iota(canonical_.begin(), canonical_.end(), VertexId{0});
    std::vector<std::pair<std::uint64_t, VertexId>> signatures;
    signatures.reserve(index.vertex_count());
    for (VertexId v = 0; v < index.vertex_count(); ++v) {
        const auto& witnesses = index.witnesses(v);
        if (witnesses.empty()) {
            ++stats_.distinct_factor_rows; // Each isolated vertex has its own {v}.
            continue;
        }
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto w : witnesses) { hash ^= w; hash *= 1099511628211ULL; }
        signatures.emplace_back(hash, v);
    }
    std::sort(signatures.begin(), signatures.end(), [&](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return index.witnesses(a.second) < index.witnesses(b.second);
    });
    VertexId representative = missing;
    std::uint64_t previous_hash = 0;
    for (const auto& entry : signatures) {
        const auto v = entry.second;
        // Hashes only group candidates. Equality is checked on the full sorted
        // witness lists; collisions cannot merge different neighborhoods.
        if (representative == missing || previous_hash != entry.first ||
            index.witnesses(representative) != index.witnesses(v)) {
            representative = v;
            previous_hash = entry.first;
            ++stats_.distinct_factor_rows;
        }
        canonical_[v] = representative;
        if (index.degree(v) != index.degree(representative))
            throw std::logic_error("equal factors have unequal closed degrees");
    }
    // A nonempty witness posting includes every vertex carrying that witness,
    // so equal witness lists imply equal CLOSED neighborhoods, including self.
    stats_.factor_sharing_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

void ExactNeighborhoodCache::unlink(VertexId v) {
    if (previous_[v] != missing) next_[previous_[v]] = next_[v];
    else head_ = next_[v];
    if (next_[v] != missing) previous_[next_[v]] = previous_[v];
    else tail_ = previous_[v];
    previous_[v] = next_[v] = missing;
}

void ExactNeighborhoodCache::touch(VertexId v) {
    if (head_ == v) return;
    if (previous_[v] != missing || next_[v] != missing || tail_ == v) unlink(v);
    next_[v] = head_;
    if (head_ != missing) previous_[head_] = v;
    else tail_ = v;
    head_ = v;
}

std::unique_ptr<ExactNeighborhoodCache::Row>
ExactNeighborhoodCache::generate(VertexId vertex) {
    const auto start = std::chrono::steady_clock::now();
    for (const auto word : touched_) scratch_[word] = 0;
    touched_.clear();
    auto visit = [&](VertexId candidate) {
        const auto word = candidate >> 6U;
        if (scratch_[word] == 0) touched_.push_back(word);
        scratch_[word] |= std::uint64_t{1} << (candidate & 63U);
    };
    visit(vertex);
    for (const auto witness : index_.witnesses(vertex)) {
        const auto& posting = index_.posting(witness);
        stats_.posting_entries += posting.size();
        for (const auto candidate : posting) visit(candidate);
    }
    auto row = std::make_unique<Row>();
    row->degree = index_.degree(vertex);
    const auto list_bytes = row->degree * sizeof(VertexId);
    const auto sparse_bytes = touched_.size() * (sizeof(VertexId) + sizeof(std::uint64_t));
    const auto dense_bytes = scratch_.size() * sizeof(std::uint64_t);
    if (dense_bytes < list_bytes && dense_bytes <= sparse_bytes) {
        row->kind = Kind::DenseBitmap;
        row->words = scratch_;
    } else if (sparse_bytes < list_bytes) {
        row->kind = Kind::SparseBitmap;
        row->ids.assign(touched_.begin(), touched_.end());
        row->words.reserve(touched_.size());
        for (const auto word : touched_) row->words.push_back(scratch_[word]);
    } else {
        row->ids.reserve(static_cast<std::size_t>(row->degree));
        for (const auto word : touched_) {
            auto bits = scratch_[word];
            while (bits != 0) {
                row->ids.push_back((word << 6U) + lowest_bit(bits));
                bits &= bits - 1;
            }
        }
        if (row->ids.size() != row->degree)
            throw std::logic_error("cached neighborhood disagrees with FLI degree");
    }
    stats_.generation_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    return row;
}

const ExactNeighborhoodCache::Row& ExactNeighborhoodCache::get(VertexId v) {
    v = canonical_[v];
    if (rows_[v]) {
        ++stats_.hits;
        touch(v);
        return *rows_[v];
    }
    ++stats_.misses;
    auto row = generate(v);
    const auto size = row->bytes();
    if (size > budget_) {
        transient_ = std::move(row);
        return *transient_;
    }
    while (tail_ != missing && bytes_ + size > budget_) {
        const auto victim = tail_;
        unlink(victim);
        bytes_ -= rows_[victim]->bytes();
        rows_[victim].reset();
        ++stats_.evictions;
    }
    rows_[v] = std::move(row);
    bytes_ += size;
    stats_.peak_bytes = std::max(stats_.peak_bytes, bytes_);
    touch(v);
    return *rows_[v];
}

void ExactNeighborhoodCache::activate(VertexId v) {
    if (witness_bounds_ && active_vertex_ != canonical_[v]) {
        if (++witness_epoch_ == 0) {
            std::fill(witness_epochs_.begin(), witness_epochs_.end(), 0);
            witness_epoch_ = 1;
        }
    }
    active_vertex_ = canonical_[v];
    const auto& row = get(v);
    if (row.kind == Kind::DenseBitmap) {
        active_ = row.words;
    } else {
        std::fill(active_.begin(), active_.end(), 0);
        if (row.kind == Kind::List) {
            for (const auto id : row.ids)
                active_[id >> 6U] |= std::uint64_t{1} << (id & 63U);
        } else {
            for (std::size_t i = 0; i < row.ids.size(); ++i)
                active_[row.ids[i]] = row.words[i];
        }
    }
}

std::uint64_t ExactNeighborhoodCache::count_witness(VertexId w) {
    const auto& posting = index_.posting(w);
    if (!witness_rows_.empty() && !witness_prepared_[w]) {
        witness_prepared_[w] = true;
        // The FLI posting is sorted and unique. Compress consecutive vertex IDs
        // into nonempty 64-bit words; keep original lists when compression loses.
        // Each witness is considered once, even when the fixed budget is full.
        if (posting.size() >= 64 && stats_.witness_bitmap_bytes < witness_bitmap_budget_) {
            const auto start = std::chrono::steady_clock::now();
            auto row = std::make_unique<Row>();
            VertexId previous = missing;
            for (const auto v : posting) {
                const auto word = v >> 6U;
                if (word != previous) {
                    row->ids.push_back(word);
                    row->words.push_back(0);
                    previous = word;
                }
                row->words.back() |= std::uint64_t{1} << (v & 63U);
            }
            stats_.witness_bitmap_build_entries += posting.size();
            row->ids.shrink_to_fit();
            row->words.shrink_to_fit();
            row->kind = Kind::SparseBitmap;
            const auto dense_bytes = active_.size() * sizeof(std::uint64_t);
            if (dense_bytes < row->ids.size() * sizeof(VertexId) + row->words.size() * sizeof(std::uint64_t)) {
                std::vector<std::uint64_t> dense(active_.size(), 0);
                for (std::size_t i=0; i<row->ids.size(); ++i) dense[row->ids[i]] = row->words[i];
                row->words.swap(dense);
                std::vector<VertexId>().swap(row->ids);
                row->kind = Kind::DenseBitmap;
            }
            if (row->bytes() < posting.size() * sizeof(VertexId) &&
                row->bytes() <= witness_bitmap_budget_ - stats_.witness_bitmap_bytes) {
                stats_.witness_bitmap_bytes += row->bytes();
                witness_rows_[w] = std::move(row);
            }
            stats_.witness_bitmap_build_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
        }
    }
    if (!witness_rows_.empty() && witness_rows_[w]) {
        const auto& row = *witness_rows_[w];
        static const auto count = bitmap_counter();
        ++stats_.witness_bitmap_checks;
        stats_.witness_bitmap_words += row.words.size();
        return count(active_.data(), row.words.data(),
                     row.kind == Kind::DenseBitmap ? nullptr : row.ids.data(), row.words.size());
    }
    std::uint64_t count = 0;
    for (const auto v : posting) count += (active_[v >> 6U] >> (v & 63U)) & 1U;
    stats_.witness_entries_read += posting.size();
    return count;
}

bool ExactNeighborhoodCache::check(VertexId right, std::uint64_t required) {
    HIN_PROFILE_SCOPE(AdaptiveCheck);
    if (active_vertex_ == missing) throw std::logic_error("no active neighborhood");
    if (canonical_[right] == active_vertex_) {
        ++stats_.equal_factor_checks;
        return index_.degree(active_vertex_) >= required;
    }
    const auto right_vertex = canonical_[right];
    const auto key = (std::uint64_t{std::min(active_vertex_, right_vertex)} << 32U) |
                     std::max(active_vertex_, right_vertex);
    PairBound* memo = nullptr;
    if (!pair_bounds_.empty()) {
        auto hash = key;
        hash ^= hash >> 30U; hash *= 0xbf58476d1ce4e5b9ULL;
        hash ^= hash >> 27U; hash *= 0x94d049bb133111ebULL;
        hash ^= hash >> 31U;
        memo = &pair_bounds_[hash & (pair_bounds_.size() - 1)];
        if (memo->key == key) {
            if (required <= memo->lower) { ++stats_.pair_bound_hits; return true; }
            if (required > memo->upper) { ++stats_.pair_bound_hits; return false; }
        }
    }
    auto remember = [&](std::uint64_t lower, std::uint64_t upper) {
        if (memo) {
            if (memo->key != key) *memo = PairBound{key, lower, upper};
            else {
                memo->lower = std::max(memo->lower, lower);
                memo->upper = std::min(memo->upper, upper);
            }
        }
    };
    const bool pressure = budget_ == 0 ||
        (stats_.evictions >= 1024 && stats_.misses > stats_.hits / 4);
    // N(right) is the UNION of its nonempty witness postings. For the fixed
    // active N(left), max |N(left) intersect S(w)| is a lower bound and their
    // SUM is an upper bound. Overlap is never counted as a lower bound.
    // Each posting count is exact and reused only while the active row agrees.
    if (witness_bounds_ && (!pressure_only_ || pressure) &&
        !index_.witnesses(right_vertex).empty()) {
        HIN_PROFILE_SCOPE(WitnessBounds);
        ++stats_.witness_bound_checks;
        std::uint64_t lower = 0, upper = 0;
        for (const auto w : index_.witnesses(right_vertex)) {
            if (witness_epochs_[w] == witness_epoch_) {
                ++stats_.witness_count_hits;
            } else {
                HIN_PROFILE_SCOPE(WitnessCount);
                if (witness_rows_.empty()) {
                    std::uint64_t count = 0;
                    const auto& posting = index_.posting(w);
                    for (const auto v : posting) count += (active_[v >> 6U] >> (v & 63U)) & 1U;
                    witness_counts_[w] = count;
                    stats_.witness_entries_read += posting.size();
                } else {
                    witness_counts_[w] = count_witness(w);
                }
                witness_epochs_[w] = witness_epoch_;
                ++stats_.witness_counts_built;
            }
            lower = std::max(lower, witness_counts_[w]);
            if (witness_exclusion_) {
                // S(w) is a subset of N[right]. Its members outside N[left]
                // are distinct missing neighbors, even if other witnesses
                // overlap. One witness alone can certify dissimilarity.
                const auto missing = index_.posting(w).size() - witness_counts_[w];
                const auto exclusion_upper = std::min(index_.degree(active_vertex_),
                                                       index_.degree(right_vertex) - missing);
                if (exclusion_upper < required) {
                    ++stats_.witness_exclusion_rejects;
                    ++stats_.witness_bound_rejects;
                    remember(lower, exclusion_upper);
                    return false;
                }
            }
            upper = std::min(index_.degree(right_vertex), upper + witness_counts_[w]);
            if (lower >= required) {
                ++stats_.witness_bound_accepts;
                remember(lower, std::min(index_.degree(active_vertex_), index_.degree(right_vertex)));
                return true;
            }
        }
        upper = std::min(upper, index_.degree(active_vertex_));
        remember(lower, upper);
        if (upper < required) {
            ++stats_.witness_bound_rejects;
            return false;
        }
    }
    // Once repeated evictions show that the working set does not fit, don't
    // fully construct every missing right row merely to evict it immediately.
    // Stream that row with exact distinct-unseen bounds. Left rows still enter
    // the cache, so subsequent reuse can return to the cached path naturally.
    HIN_PROFILE_SCOPE(FullIntersection);
    if (!rows_[right_vertex] && pressure) {
        const auto started = std::chrono::steady_clock::now();
        ++stats_.misses;
        ++stats_.streaming_checks;
        for (const auto word : touched_) scratch_[word] = 0;
        touched_.clear();
        std::uint64_t seen = 0, found = 0, scanned = 0;
        const auto degree = index_.degree(right_vertex);
        auto visit = [&](VertexId v) {
            const auto word = v >> 6U;
            const auto bit = std::uint64_t{1} << (v & 63U);
            if (scratch_[word] & bit) return;
            if (scratch_[word] == 0) touched_.push_back(word);
            scratch_[word] |= bit;
            ++seen;
            found += (active_[word] & bit) != 0;
        };
        auto finish = [&](bool answer, std::uint64_t upper) {
            remember(found, upper);
            stats_.streaming_posting_entries += scanned;
            stats_.streaming_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            return answer;
        };
        visit(right_vertex);
        for (const auto w : index_.witnesses(right_vertex)) {
            for (const auto v : index_.posting(w)) {
                visit(v);
                ++scanned;
                if ((scanned & 63U) == 0) {
                    const auto upper = found + degree - seen;
                    if (found >= required) return finish(true, upper);
                    if (upper < required) return finish(false, upper);
                }
            }
        }
        if (seen != degree) throw std::logic_error("streaming neighborhood disagrees with degree");
        return finish(found >= required, found);
    }
    const auto& row = get(right);
    std::uint64_t common = 0;
    if (row.kind == Kind::List) {
        ++stats_.list_checks;
        for (std::size_t start = 0; start < row.ids.size(); start += 64) {
            const auto end = std::min(start + 64, row.ids.size());
            for (auto i = start; i < end; ++i) {
                const auto id = row.ids[i];
                common += (active_[id >> 6U] >> (id & 63U)) & 1U;
            }
            stats_.intersection_units += end - start;
            const auto upper = common + row.ids.size() - end;
            if (common >= required) { remember(common, upper); return true; }
            if (upper < required) { remember(common, upper); return false; }
        }
    } else {
        ++stats_.bitmap_checks;
        static const auto count = bitmap_counter();
        common = count(active_.data(), row.words.data(),
                       row.kind == Kind::DenseBitmap ? nullptr : row.ids.data(), row.words.size());
        stats_.intersection_units += row.words.size();
    }
    remember(common, common);
    return common >= required;
}

std::uint64_t ExactNeighborhoodCache::workspace_bytes() const {
    return rows_.capacity() * sizeof(std::unique_ptr<Row>) +
           (previous_.capacity() + next_.capacity() + touched_.capacity() + canonical_.capacity()) * sizeof(VertexId) +
           (scratch_.capacity() + active_.capacity() + witness_counts_.capacity()) * sizeof(std::uint64_t) +
           witness_epochs_.capacity() * sizeof(std::uint32_t) +
           witness_rows_.capacity() * sizeof(std::unique_ptr<Row>) +
           (witness_prepared_.capacity() + 7) / 8;
}
} // namespace hinscan
