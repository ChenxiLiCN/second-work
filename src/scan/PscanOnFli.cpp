#include "scan/PscanOnFli.h"
#include "scan/QueryProfile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <list>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace hinscan {
namespace {

enum class EdgeState : std::int8_t {
    Dissimilar = -1,
    Unknown = 0,
    Similar = 1,
};

class NeighborhoodCache {
public:
    explicit NeighborhoodCache(std::uint64_t byte_budget)
        : byte_budget_(byte_budget) {}

    const std::vector<VertexId>* get(VertexId vertex) {
        const auto iterator = entries_.find(vertex);
        if (iterator == entries_.end()) {
            return nullptr;
        }
        lru_.splice(lru_.begin(), lru_, iterator->second.position);
        iterator->second.position = lru_.begin();
        return &iterator->second.neighborhood;
    }

    std::uint64_t put(VertexId vertex,
                      const std::vector<VertexId>& neighborhood) {
        if (byte_budget_ == 0 || entries_.find(vertex) != entries_.end()) {
            return 0;
        }
        std::vector<VertexId> stored(neighborhood.begin(), neighborhood.end());
        const auto bytes = static_cast<std::uint64_t>(
            stored.capacity() * sizeof(VertexId));
        if (bytes > byte_budget_) {
            return 0;
        }

        std::uint64_t evictions = 0;
        while (!lru_.empty() && payload_bytes_ + bytes > byte_budget_) {
            const auto victim = lru_.back();
            const auto victim_iterator = entries_.find(victim);
            payload_bytes_ -= static_cast<std::uint64_t>(
                victim_iterator->second.neighborhood.capacity() *
                sizeof(VertexId));
            entries_.erase(victim_iterator);
            lru_.pop_back();
            ++evictions;
        }

        lru_.push_front(vertex);
        entries_.emplace(
            vertex, Entry{std::move(stored), lru_.begin()});
        payload_bytes_ += bytes;
        peak_payload_bytes_ = std::max(peak_payload_bytes_, payload_bytes_);
        return evictions;
    }

    std::uint64_t size() const noexcept { return entries_.size(); }
    std::uint64_t payload_bytes() const noexcept { return payload_bytes_; }
    std::uint64_t peak_payload_bytes() const noexcept {
        return peak_payload_bytes_;
    }

private:
    struct Entry {
        std::vector<VertexId> neighborhood;
        std::list<VertexId>::iterator position;
    };

    std::uint64_t byte_budget_ = 0;
    std::uint64_t payload_bytes_ = 0;
    std::uint64_t peak_payload_bytes_ = 0;
    std::list<VertexId> lru_;
    std::unordered_map<VertexId, Entry> entries_;
};

class SimilarityWorkspace {
public:
    SimilarityWorkspace(const FactorIndex& index,
                        const SimilarityThreshold& threshold)
        : index_(index),
          threshold_(threshold),
          left_marks_(static_cast<std::size_t>(index.vertex_count()), 0),
          seen_(static_cast<std::size_t>(index.vertex_count()), 0),
          candidate_seen_(static_cast<std::size_t>(index.vertex_count()), 0) {}

    std::vector<VertexId> generate_full_neighborhood(VertexId left,
                                                      PscanOnFliStats* stats) {
        advance_epoch(&candidate_epoch_, &candidate_seen_);
        std::vector<VertexId> neighborhood;
        neighborhood.reserve(static_cast<std::size_t>(index_.degree(left)));

        auto append_once = [&](VertexId candidate) {
            if (candidate_seen_[candidate] != candidate_epoch_) {
                candidate_seen_[candidate] = candidate_epoch_;
                neighborhood.push_back(candidate);
            }
        };
        append_once(left);
        for (const auto witness : index_.witnesses(left)) {
            for (const auto candidate : index_.posting(witness)) {
                ++stats->neighborhood_posting_entries_read;
                append_once(candidate);
            }
        }
        if (neighborhood.size() != index_.degree(left)) {
            throw std::logic_error(
                "timestamp neighborhood disagrees with stored degree");
        }
        std::sort(neighborhood.begin(), neighborhood.end());

        activate(left, neighborhood);
        ++stats->neighborhood_generations;
        stats->peak_ephemeral_neighborhood_bytes = std::max(
            stats->peak_ephemeral_neighborhood_bytes,
            static_cast<std::uint64_t>(neighborhood.capacity() *
                                       sizeof(VertexId)));
        return neighborhood;
    }

    std::vector<VertexId> generate_degree_compatible_candidates(
        VertexId left,
        PscanOnFliStats* stats,
        const std::vector<VertexId>* seed_components = nullptr,
        const std::vector<std::size_t>* certified_prefix = nullptr,
        const std::vector<bool>* final_cores = nullptr) {
        advance_epoch(&candidate_epoch_, &candidate_seen_);
        std::vector<VertexId> candidates;
        candidates.reserve(static_cast<std::size_t>(seed_components || final_cores
            ? std::min<std::uint64_t>(index_.degree(left), 64) : index_.degree(left)));

        const auto left_degree = index_.degree(left);
        const long double numerator =
            static_cast<long double>(threshold_.numerator);
        const long double denominator =
            static_cast<long double>(threshold_.denominator);
        const long double epsilon_squared =
            (numerator * numerator) / (denominator * denominator);
        // The bucket range is deliberately conservative.  A direct ceil/floor
        // around a floating-point epsilon^2 can exclude a valid degree at an
        // exact integer boundary (for example epsilon=0.6).  floor/ceil in the
        // opposite directions may admit at most the two boundary buckets; the
        // exact rational degree-ratio predicate below removes those extras.
        const long double minimum_value =
            static_cast<long double>(left_degree) * epsilon_squared;
        const long double maximum_value =
            static_cast<long double>(left_degree) / epsilon_squared;
        const auto maximum_uint64 =
            static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
        const auto minimum_degree = minimum_value >= maximum_uint64
                                        ? std::numeric_limits<std::uint64_t>::max()
                                        : static_cast<std::uint64_t>(
                                              std::floor(minimum_value));
        const auto maximum_degree = maximum_value >= maximum_uint64
                                        ? std::numeric_limits<std::uint64_t>::max()
                                        : static_cast<std::uint64_t>(
                                              std::ceil(maximum_value));

        auto append_once = [&](VertexId candidate) {
            if (candidate != left &&
                candidate_seen_[candidate] != candidate_epoch_) {
                candidate_seen_[candidate] = candidate_epoch_;
                candidates.push_back(candidate);
            }
        };

        for (const auto witness : index_.witnesses(left)) {
            const auto& posting = index_.degree_ordered_posting(witness);
            auto begin = std::lower_bound(
                posting.begin(), posting.end(), minimum_degree,
                [&](VertexId candidate, std::uint64_t value) {
                    return index_.degree(candidate) < value;
                });
            const auto end = std::upper_bound(
                begin, posting.end(), maximum_degree,
                [&](std::uint64_t value, VertexId candidate) {
                    return value < index_.degree(candidate);
                });
            stats->degree_bucket_posting_entries_skipped +=
                static_cast<std::uint64_t>(posting.size() -
                                           std::distance(begin, end));
            if (certified_prefix && (*certified_prefix)[witness] != 0 &&
                (final_cores || (seed_components &&
                  (*seed_components)[left] != std::numeric_limits<VertexId>::max() &&
                  (*seed_components)[left] == (*seed_components)[posting.front()]))) {
                const auto skip_end = std::min(end, posting.begin() + (*certified_prefix)[witness]);
                if (skip_end > begin) {
                    stats->block_prefix_entries_skipped += std::distance(begin, skip_end);
                    begin = skip_end;
                }
            }
            for (auto iterator = begin; iterator != end; ++iterator) {
                ++stats->degree_bucket_posting_entries_read;
                if (final_cores && (*final_cores)[*iterator]) {
                    ++stats->final_core_entries_skipped;
                    continue;
                }
                if (seed_components && (*seed_components)[left] != std::numeric_limits<VertexId>::max() &&
                    (*seed_components)[left] == (*seed_components)[*iterator]) {
                    ++stats->seed_component_entries_skipped;
                    continue;
                }
                if (!threshold_.fails_degree_ratio(
                        left_degree, index_.degree(*iterator))) {
                    append_once(*iterator);
                } else {
                    ++stats->degree_bucket_posting_entries_skipped;
                }
            }
        }
        std::sort(candidates.begin(), candidates.end());
        ++stats->degree_bucket_candidate_generations;
        stats->candidate_vertices_emitted += candidates.size();
        return candidates;
    }

    void activate(VertexId left,
                  const std::vector<VertexId>& neighborhood) {
        advance_epoch(&left_epoch_, &left_marks_);
        for (const auto candidate : neighborhood) {
            left_marks_[candidate] = left_epoch_;
        }
        current_left_ = left;
        has_current_left_ = true;
    }

    bool check(VertexId left,
               VertexId right,
               const std::vector<VertexId>* cached_right_neighborhood,
               PscanOnFliStats* stats) {
        if (!has_current_left_ || current_left_ != left) {
            throw std::logic_error(
                "similarity check has no matching active left neighborhood");
        }
        const auto required = threshold_.required_common_neighbors(
            index_.degree(left), index_.degree(right));

        if (cached_right_neighborhood != nullptr) {
            ++stats->cached_similarity_checks;
            std::uint64_t common = 0;
            std::uint64_t remaining = cached_right_neighborhood->size();
            for (const auto candidate : *cached_right_neighborhood) {
                --remaining;
                ++stats->cached_similarity_entries_read;
                if (left_marks_[candidate] == left_epoch_) {
                    ++common;
                    if (common >= required) {
                        ++stats->early_accept_checks;
                        return true;
                    }
                }
                if (common + remaining < required) {
                    ++stats->early_reject_checks;
                    return false;
                }
            }
            return common >= required;
        }

        advance_epoch(&seen_epoch_, &seen_);

        // index_.degree(right) is the exact number of distinct vertices in the
        // closed projected neighborhood.  The old implementation used the
        // number of raw posting entries as the rejection upper bound.  A
        // vertex may occur in many postings, so that bound can be orders of
        // magnitude too loose and forces us to scan duplicates that cannot
        // possibly change the answer.  Track unseen *distinct* neighbors
        // instead.  This remains an exact, epsilon-independent structural
        // bound: at every point common + remaining_distinct is the largest
        // intersection that can still be reached.
        std::uint64_t remaining_distinct = index_.degree(right);

        std::uint64_t common = 0;
        auto visit = [&](VertexId candidate, bool posting_entry) {
            if (posting_entry) {
                ++stats->similarity_posting_entries_read;
            }
            if (seen_[candidate] != seen_epoch_) {
                seen_[candidate] = seen_epoch_;
                --remaining_distinct;
                if (left_marks_[candidate] == left_epoch_) {
                    ++common;
                }
            }
        };

        visit(right, false);
        if (common >= required) {
            ++stats->early_accept_checks;
            return true;
        }
        for (const auto witness : index_.witnesses(right)) {
            for (const auto candidate : index_.posting(witness)) {
                visit(candidate, true);
                if (common >= required) {
                    ++stats->early_accept_checks;
                    return true;
                }
                if (common + remaining_distinct < required) {
                    ++stats->early_reject_checks;
                    return false;
                }
            }
        }
        return common >= required;
    }

    std::uint64_t bytes() const noexcept {
        return (left_marks_.size() + seen_.size() + candidate_seen_.size()) *
               sizeof(std::uint32_t);
    }

private:
    static void advance_epoch(std::uint32_t* epoch,
                              std::vector<std::uint32_t>* values) {
        ++(*epoch);
        if (*epoch == 0) {
            std::fill(values->begin(), values->end(), 0);
            *epoch = 1;
        }
    }

    const FactorIndex& index_;
    const SimilarityThreshold& threshold_;
    std::vector<std::uint32_t> left_marks_;
    std::vector<std::uint32_t> seen_;
    std::vector<std::uint32_t> candidate_seen_;
    std::uint32_t left_epoch_ = 0;
    std::uint32_t seen_epoch_ = 0;
    std::uint32_t candidate_epoch_ = 0;
    VertexId current_left_ = 0;
    bool has_current_left_ = false;
};

std::uint64_t edge_key(VertexId left, VertexId right) {
    if (left > right) {
        std::swap(left, right);
    }
    return (static_cast<std::uint64_t>(left) << 32U) |
           static_cast<std::uint64_t>(right);
}

class EdgeCertificateTable {
public:
    bool get(std::uint64_t key, EdgeState* value) const {
        HIN_PROFILE_SCOPE(CertificateGet);
        if (keys_.empty()) {
            return false;
        }
        auto position = bucket(key);
        while (keys_[position] != empty_key()) {
            if (keys_[position] == key) {
                *value = static_cast<EdgeState>(values_[position]);
                return true;
            }
            position = (position + 1) & (keys_.size() - 1);
        }
        return false;
    }

    void insert(std::uint64_t key, EdgeState value) {
        if (keys_.empty()) {
            rehash(1024);
        } else if ((size_ + 1) * 10 >= keys_.size() * 7) {
            rehash(keys_.size() * 2);
        }
        HIN_PROFILE_SCOPE(CertificatePut);
        insert_without_growth(key, value);
    }

    std::uint64_t size() const noexcept { return size_; }

    std::uint64_t bytes() const noexcept {
        return static_cast<std::uint64_t>(
            keys_.capacity() * sizeof(std::uint64_t) +
            values_.capacity() * sizeof(std::int8_t));
    }

private:
    static constexpr std::uint64_t empty_key() noexcept {
        return std::numeric_limits<std::uint64_t>::max();
    }

    static std::uint64_t mix(std::uint64_t value) noexcept {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    std::size_t bucket(std::uint64_t key) const noexcept {
        return static_cast<std::size_t>(mix(key)) & (keys_.size() - 1);
    }

    void insert_without_growth(std::uint64_t key, EdgeState value) {
        auto position = bucket(key);
        while (keys_[position] != empty_key()) {
            if (keys_[position] == key) {
                values_[position] = static_cast<std::int8_t>(value);
                return;
            }
            position = (position + 1) & (keys_.size() - 1);
        }
        keys_[position] = key;
        values_[position] = static_cast<std::int8_t>(value);
        ++size_;
    }

    void rehash(std::size_t capacity) {
        HIN_PROFILE_SCOPE(CertificateRehash);
        std::vector<std::uint64_t> old_keys = std::move(keys_);
        std::vector<std::int8_t> old_values = std::move(values_);
        keys_.assign(capacity, empty_key());
        values_.assign(capacity, 0);
        size_ = 0;
        for (std::size_t i = 0; i < old_keys.size(); ++i) {
            if (old_keys[i] != empty_key()) {
                insert_without_growth(
                    old_keys[i], static_cast<EdgeState>(old_values[i]));
            }
        }
    }

    std::vector<std::uint64_t> keys_;
    std::vector<std::int8_t> values_;
    std::uint64_t size_ = 0;
};

class DisjointSet {
public:
    explicit DisjointSet(std::size_t size) : parent_(size), rank_(size, 0) {
        std::iota(parent_.begin(), parent_.end(), 0);
    }

    VertexId find(VertexId vertex) {
        HIN_PROFILE_SCOPE(Find);
        return find_impl(vertex);
    }

private:
    VertexId find_impl(VertexId vertex) {
        if (parent_[vertex] != vertex) {
            parent_[vertex] = find_impl(parent_[vertex]);
        }
        return parent_[vertex];
    }

public:
    void unite(VertexId left, VertexId right) {
        HIN_PROFILE_SCOPE(Unite);
        left = find_impl(left);
        right = find_impl(right);
        if (left == right) {
            return;
        }
        if (rank_[left] < rank_[right]) {
            std::swap(left, right);
        }
        parent_[right] = left;
        if (rank_[left] == rank_[right]) {
            ++rank_[left];
        }
    }

private:
    std::vector<VertexId> parent_;
    std::vector<std::uint8_t> rank_;
};

class PscanExecution {
public:
    PscanExecution(const FactorIndex& index,
                   const SimilarityThreshold& threshold,
                   std::uint64_t mu,
                   std::uint64_t neighborhood_cache_bytes,
                   const FingerprintNeighborhoodIndex* fingerprint_index,
                   const BudgetedSimilarityIndex* budgeted_index,
                   bool adaptive_neighborhoods,
                   BlockExecutionMode block_mode)
        : index_(index),
          threshold_(threshold),
          mu_(mu),
          similar_degree_(static_cast<std::size_t>(index.vertex_count()), 0),
          effective_degree_(static_cast<std::size_t>(index.vertex_count()), 0),
          components_(static_cast<std::size_t>(index.vertex_count())),
          similarity_workspace_(index, threshold),
          neighborhood_cache_(neighborhood_cache_bytes),
          fingerprint_index_(fingerprint_index),
          budgeted_index_(budgeted_index), block_mode_(block_mode) {
        if (adaptive_neighborhoods)
            adaptive_cache_ = std::make_unique<ExactNeighborhoodCache>(index, neighborhood_cache_bytes,
                block_mode >= BlockExecutionMode::WitnessBounds &&
                    block_mode != BlockExecutionMode::CoreResumable &&
                    block_mode != BlockExecutionMode::CoreAnchor &&
                    block_mode != BlockExecutionMode::CoreSinglePassLean &&
                    block_mode != BlockExecutionMode::CoreSinglePass &&
                    block_mode != BlockExecutionMode::LazyNoWitness,
                block_mode >= BlockExecutionMode::AdaptiveWitnessBounds &&
                    block_mode != BlockExecutionMode::LazyAlwaysExclusion,
                block_mode == BlockExecutionMode::WitnessBitmaps,
                block_mode == BlockExecutionMode::WitnessExclusion ||
                    block_mode == BlockExecutionMode::LazyAlwaysExclusion ||
                    block_mode == BlockExecutionMode::CoreConnectivity,
                block_mode == BlockExecutionMode::CoreSinglePass || block_mode == BlockExecutionMode::CoreSinglePassLean || block_mode == BlockExecutionMode::CoreAnchor || block_mode == BlockExecutionMode::CoreResumable,
                block_mode == BlockExecutionMode::CoreSinglePassLean || block_mode == BlockExecutionMode::CoreAnchor || block_mode == BlockExecutionMode::CoreResumable,
                block_mode == BlockExecutionMode::CoreResumable);
        if (block_mode == BlockExecutionMode::CoreAnchor)
            anchor_filter_=std::make_unique<AnchorFilter>(index,neighborhood_cache_bytes/8);
        stats_.timestamp_workspace_bytes = similarity_workspace_.bytes();
#ifdef HINSCAN_SHARED_GROUPS
        if (adaptive_neighborhoods && block_mode==BlockExecutionMode::CoreSinglePassLean)
            shared_groups_=std::make_unique<SharedGroupDiagnostics>(index);
#endif
        if (fingerprint_index_ != nullptr) {
            if (fingerprint_index_->vertex_count() != index_.vertex_count()) {
                throw std::invalid_argument(
                    "fingerprint and factor indexes have different vertex counts");
            }
            for (std::uint64_t vertex = 0; vertex < index_.vertex_count();
                 ++vertex) {
                if (fingerprint_index_->neighborhood_size(
                        static_cast<VertexId>(vertex)) != index_.degree(vertex)) {
                    throw std::invalid_argument(
                        "fingerprint and factor neighborhood sizes disagree");
                }
            }
        }
        if (budgeted_index_ != nullptr &&
            budgeted_index_->vertex_count() != index_.vertex_count()) {
            throw std::invalid_argument(
                "budgeted similarity and factor indexes have different vertex counts");
        }
    }

    PscanOnFliResult run() {
        const auto begin = std::chrono::steady_clock::now();
        profile::set_phase(profile::Phase::Prune);
        if (block_mode_ == BlockExecutionMode::Disabled) {
            prune_and_seed_cores();
        } else {
            discover_core_blocks();
            prune_unproven_vertices();
        }
        initialize_effective_degree_queue();
        const auto pruned = std::chrono::steady_clock::now();
        profile::set_phase(profile::Phase::Core);
        cluster_core_vertices();
        const auto cores = std::chrono::steady_clock::now();
        profile::set_phase(profile::Phase::Noncore);
        auto result = cluster_noncore_vertices();
        result.stats = stats_;
#ifdef HINSCAN_SHARED_GROUPS
        if (shared_groups_) {
            shared_groups_->finish();
            result.stats.shared_groups=std::make_shared<SharedGroupStats>(shared_groups_->stats());
        }
#endif
        const auto end = std::chrono::steady_clock::now();
        result.stats.prune_ms = std::chrono::duration<double, std::milli>(pruned - begin).count();
        result.stats.core_ms = std::chrono::duration<double, std::milli>(cores - pruned).count();
        result.stats.noncore_ms = std::chrono::duration<double, std::milli>(end - cores).count();
        if (adaptive_cache_) {
            result.stats.adaptive_cache = adaptive_cache_->stats();
            result.stats.adaptive_workspace_bytes = adaptive_cache_->workspace_bytes();
        }
        result.stats.query_milliseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - begin)
                .count());
        if (anchor_filter_) result.stats.anchor=anchor_filter_->stats();
        result.stats.sparse_certificate_entries = certificates_.size();
        result.stats.sparse_certificate_table_bytes = certificates_.bytes();
        result.stats.neighborhood_cache_entries = neighborhood_cache_.size();
        result.stats.neighborhood_cache_payload_bytes =
            neighborhood_cache_.payload_bytes();
        result.stats.neighborhood_cache_peak_payload_bytes =
            neighborhood_cache_.peak_payload_bytes();
        return result;
    }

private:
    struct EffectiveEntry {
        std::int64_t effective_degree = 0;
        VertexId vertex = 0;

        bool operator<(const EffectiveEntry& other) const {
            if (effective_degree != other.effective_degree) {
                return effective_degree < other.effective_degree;
            }
            return vertex < other.vertex;
        }
    };

    std::vector<VertexId> neighborhood(VertexId vertex,
                                       bool activate_full_neighborhood,
                                       bool skip_seed_internal = false,
                                       const std::vector<bool>* final_cores = nullptr) {
        HIN_PROFILE_SCOPE(Candidates);
        auto candidates = similarity_workspace_.generate_degree_compatible_candidates(
            vertex, &stats_,
            block_mode_ >= BlockExecutionMode::SkipCertified && skip_seed_internal ? &seed_components_ : nullptr,
            block_mode_ >= BlockExecutionMode::SkipCertified && (skip_seed_internal || final_cores) ? &certified_prefix_ : nullptr,
            block_mode_ >= BlockExecutionMode::SkipCertified ? final_cores : nullptr);
        if (activate_full_neighborhood && block_mode_ < BlockExecutionMode::AdaptiveWitnessBounds &&
            (block_mode_ == BlockExecutionMode::Disabled || !candidates.empty())) {
            activate_neighborhood(vertex);
        }
        return candidates;
    }

    void activate_neighborhood(VertexId vertex) {
        HIN_PROFILE_SCOPE(Activation);
        if (adaptive_cache_) {
            adaptive_cache_->activate(vertex);
        } else if (const auto* cached = neighborhood_cache_.get(vertex)) {
            ++stats_.neighborhood_cache_hits;
            similarity_workspace_.activate(vertex, *cached);
        } else {
            ++stats_.neighborhood_cache_misses;
            const auto closed = similarity_workspace_.generate_full_neighborhood(vertex, &stats_);
            stats_.neighborhood_cache_evictions += neighborhood_cache_.put(vertex, closed);
        }
        activated_vertex_ = vertex;
    }

    void discover_core_blocks() {
        const auto started = std::chrono::steady_clock::now();
        seed_components_.assign(index_.vertex_count(), std::numeric_limits<VertexId>::max());
        certified_prefix_.assign(index_.center_count(), 0);
        for (VertexId w = 0; w < index_.center_count(); ++w) {
            const auto& posting = index_.degree_ordered_posting(w);
            // Every pair in a posting shares at least |posting| closed neighbors.
            // Legacy mode proves a clique. Connectivity mode proves each prefix
            // vertex core and similar to its minimum-degree anchor, not a clique.
            const bool connectivity = block_mode_ == BlockExecutionMode::CoreConnectivity ||
                                      block_mode_ == BlockExecutionMode::CoreSinglePass ||
                                      block_mode_ == BlockExecutionMode::CoreSinglePassLean ||
                                      block_mode_ == BlockExecutionMode::CoreAnchor ||
                                      block_mode_ == BlockExecutionMode::CoreResumable;
            if (connectivity && posting.size() <= mu_) continue;
            std::uint64_t partner=0;
            if (connectivity) {
                const auto a=index_.degree(posting[static_cast<std::size_t>(mu_-1)]);
                const auto b=index_.degree(posting[static_cast<std::size_t>(mu_)]);
                partner=threshold_.certifies_common(posting.size(),a,b) ? a : b;
            }
            const auto end = std::partition_point(posting.begin(), posting.end(), [&](VertexId v) {
                if (connectivity)
                    return threshold_.certifies_common(posting.size(),index_.degree(v),partner);
                return threshold_.required_common_neighbors(index_.degree(v), index_.degree(v)) <= posting.size();
            });
            const auto count = static_cast<std::size_t>(std::distance(posting.begin(), end));
            // Connectivity prefixes may be smaller than mu+1: the supporting
            // similar neighbors need not themselves be core or in the prefix.
            if (connectivity ? count == 0 : count <= mu_) continue;
            certified_prefix_[w] = count;
            ++stats_.certified_blocks;
            stats_.block_incidences += count;
            for (auto it = posting.begin(); it != end; ++it) {
                const auto v = *it;
                if (seed_components_[v] == std::numeric_limits<VertexId>::max()) {
                    seed_components_[v] = v;
                    similar_degree_[v] = static_cast<std::int64_t>(mu_);
                    // An upper bound suffices: this vertex is already proved core.
                    effective_degree_[v] = static_cast<std::int64_t>(index_.degree(v) - 1);
                    core_stack_.push_back(v);
                    ++stats_.block_core_vertices;
                }
                components_.unite(posting.front(), v);
            }
        }
        for (VertexId v = 0; v < index_.vertex_count(); ++v)
            if (seed_components_[v] != std::numeric_limits<VertexId>::max())
                seed_components_[v] = components_.find(v);
        stats_.block_workspace_bytes = seed_components_.capacity() * sizeof(VertexId) +
                                       certified_prefix_.capacity() * sizeof(std::size_t);
        stats_.block_discovery_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
    }

    void prune_unproven_vertices() {
        // Certified vertices do not need their full candidate degree enumerated.
        // Every OTHER vertex gets its own complete sd/ed initialization; unlike
        // the legacy undirected loop this cannot lose a contribution whose other
        // endpoint was skipped. Overlapping blocks never add duplicate counts.
        for (VertexId v = 0; v < index_.vertex_count(); ++v) {
            if (seed_components_[v] != std::numeric_limits<VertexId>::max()) continue;
            const auto neighbors = neighborhood(v, false);
            effective_degree_[v] = static_cast<std::int64_t>(neighbors.size());
            for (const auto other : neighbors) {
                if (default_state(v, other) == EdgeState::Similar) {
                    ++similar_degree_[v];
                    push_core_if_new(v);
                }
            }
        }
        // Legacy all-edge prune counters are intentionally not inferred here:
        // the whole point is that certified internal pairs were not enumerated.
        stats_.projected_edges_seen_in_prune = index_.stats().exact_projected_edges;
    }

    EdgeState default_state(VertexId left, VertexId right) const {
        const auto left_degree = index_.degree(left);
        const auto right_degree = index_.degree(right);
        if (threshold_.fails_degree_ratio(left_degree, right_degree)) {
            return EdgeState::Dissimilar;
        }
        if (threshold_.required_common_neighbors(left_degree, right_degree) <= 2) {
            return EdgeState::Similar;
        }
        return EdgeState::Unknown;
    }

    EdgeState state(VertexId left, VertexId right) {
        HIN_PROFILE_SCOPE(State);
        EdgeState certificate = EdgeState::Unknown;
        if (certificates_.get(edge_key(left, right), &certificate)) {
            ++stats_.certificate_hits;
            return certificate;
        }
        return default_state(left, right);
    }

    EdgeState exact_check(VertexId left, VertexId right) {
        HIN_PROFILE_SCOPE(Exact);
        const auto key = edge_key(left, right);
        EdgeState existing = EdgeState::Unknown;
        if (certificates_.get(key, &existing)) {
            ++stats_.certificate_hits;
            return existing;
        }
        if (budgeted_index_ != nullptr) {
            ++stats_.budgeted_index_checks;
            std::uint32_t common = 0;
            if (budgeted_index_->find(left, right, &common)) {
                ++stats_.budgeted_index_hits;
                const auto required = threshold_.required_common_neighbors(
                    index_.degree(left), index_.degree(right));
                const auto result = common >= required
                    ? EdgeState::Similar
                    : EdgeState::Dissimilar;
                certificates_.insert(key, result);
                if (result == EdgeState::Similar) {
                    ++stats_.exact_similar_certificates;
                } else {
                    ++stats_.exact_dissimilar_certificates;
                }
                return result;
            }
        }
        if (fingerprint_index_ != nullptr) {
            const auto required = threshold_.required_common_neighbors(
                index_.degree(left), index_.degree(right));
            const auto bound = fingerprint_index_->check_upper_bound(
                left, right, required);
            ++stats_.fingerprint_bound_checks;
            stats_.fingerprint_entries_read += bound.entries_read;
            if (!bound.may_reach_required) {
                ++stats_.fingerprint_bound_rejections;
                ++stats_.exact_dissimilar_certificates;
                certificates_.insert(key, EdgeState::Dissimilar);
                return EdgeState::Dissimilar;
            }
        }
        // Certified/cached decisions need no materialized active neighborhood.
        // Rejection only: preserve the existing candidate enumeration, edge
        // certificates, endpoint updates and all pSCAN core/role semantics.
        if (anchor_filter_ && anchor_filter_->reject(left,right,
                threshold_.required_common_neighbors(index_.degree(left),index_.degree(right)))) {
            ++stats_.exact_dissimilar_certificates;
            certificates_.insert(key,EdgeState::Dissimilar);
            return EdgeState::Dissimilar;
        }
        // Delay activation until a remaining predicate actually uses it.
        if (block_mode_ >= BlockExecutionMode::AdaptiveWitnessBounds && activated_vertex_ != left)
            activate_neighborhood(left);
#ifdef HINSCAN_SHARED_GROUPS
        const auto shared_started=std::chrono::steady_clock::now();
        const auto shared_entries=adaptive_cache_?
            adaptive_cache_->stats().posting_entries+adaptive_cache_->stats().streaming_posting_entries:0;
        const auto shared_units=adaptive_cache_?adaptive_cache_->stats().intersection_units:0;
#endif
        const auto similar = adaptive_cache_
            ? adaptive_cache_->check(right, threshold_.required_common_neighbors(
                                              index_.degree(left), index_.degree(right)))
            : similarity_workspace_.check(left, right, neighborhood_cache_.get(right), &stats_);
        ++stats_.exact_similarity_checks;
#ifdef HINSCAN_SHARED_GROUPS
        const auto predicate_ms=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-shared_started).count();
        if (shared_groups_) {
            const auto& s=adaptive_cache_->stats();
            shared_groups_->observe(left,right,
                threshold_.required_common_neighbors(index_.degree(left),index_.degree(right)),
                similar,predicate_ms,s.posting_entries+s.streaming_posting_entries-shared_entries,
                s.intersection_units-shared_units);
        }
#endif
        const auto result = similar ? EdgeState::Similar : EdgeState::Dissimilar;
        certificates_.insert(key, result);
        if (result == EdgeState::Similar) {
            ++stats_.exact_similar_certificates;
        } else {
            ++stats_.exact_dissimilar_certificates;
        }
        return result;
    }

    void push_core_if_new(VertexId vertex) {
        if (similar_degree_[vertex] == static_cast<std::int64_t>(mu_)) {
            core_stack_.push_back(vertex);
        }
    }

    void push_effective(VertexId vertex) {
        if (effective_degree_[vertex] >= static_cast<std::int64_t>(mu_)) {
            effective_queue_.push(
                EffectiveEntry{effective_degree_[vertex], vertex});
        }
    }

    void prune_and_seed_cores() {
        std::uint64_t compatible_edges = 0;
        for (std::uint64_t left64 = 0; left64 < index_.vertex_count(); ++left64) {
            const auto left = static_cast<VertexId>(left64);
            const auto neighbors = neighborhood(left, false);
            effective_degree_[left] =
                static_cast<std::int64_t>(neighbors.size());
            for (const auto right : neighbors) {
                if (right <= left) {
                    continue;
                }
                ++compatible_edges;
                const auto edge_state = default_state(left, right);
                if (edge_state == EdgeState::Dissimilar) {
                    throw std::logic_error(
                        "degree bucket emitted a degree-pruned candidate");
                }
                if (edge_state == EdgeState::Similar) {
                    ++similar_degree_[left];
                    ++similar_degree_[right];
                    push_core_if_new(left);
                    push_core_if_new(right);
                    ++stats_.automatically_similar_edges;
                } else {
                    ++stats_.initially_uncertain_edges;
                }
            }
        }
        stats_.projected_edges_seen_in_prune =
            index_.stats().exact_projected_edges;
        stats_.degree_compatible_edges_enumerated = compatible_edges;
        if (compatible_edges > stats_.projected_edges_seen_in_prune) {
            throw std::logic_error(
                "degree-compatible edge count exceeds projected edges");
        }
        stats_.degree_pruned_edges =
            stats_.projected_edges_seen_in_prune - compatible_edges;
    }

    void initialize_effective_degree_queue() {
        for (std::uint64_t vertex = 0; vertex < index_.vertex_count(); ++vertex) {
            push_effective(static_cast<VertexId>(vertex));
        }
    }

    bool next_vertex(VertexId* vertex) {
        while (!core_stack_.empty()) {
            const auto candidate = core_stack_.back();
            core_stack_.pop_back();
            if (effective_degree_[candidate] >= 0) {
                *vertex = candidate;
                return true;
            }
        }
        while (!effective_queue_.empty()) {
            const auto entry = effective_queue_.top();
            effective_queue_.pop();
            if (effective_degree_[entry.vertex] == entry.effective_degree &&
                entry.effective_degree >= static_cast<std::int64_t>(mu_)) {
                *vertex = entry.vertex;
                return true;
            }
        }
        return false;
    }

    void update_other_after_check(VertexId other, EdgeState checked_state) {
        if (effective_degree_[other] < 0) {
            return;
        }
        if (checked_state == EdgeState::Similar) {
            ++similar_degree_[other];
            push_core_if_new(other);
        } else {
            --effective_degree_[other];
            push_effective(other);
        }
    }

    void cluster_core_vertices() {
        VertexId vertex = 0;
        while (next_vertex(&vertex)) {
            auto edge_buffer = neighborhood(vertex, true, true);
            edge_buffer.erase(
                std::remove_if(edge_buffer.begin(),
                               edge_buffer.end(),
                               [&](VertexId neighbor) {
                                   if (state(vertex, neighbor) ==
                                       EdgeState::Dissimilar) {
                                       return true;
                                   }
                                   return similar_degree_[vertex] >=
                                              static_cast<std::int64_t>(mu_) &&
                                          components_.find(vertex) ==
                                              components_.find(neighbor);
                               }),
                edge_buffer.end());

            std::size_t position = 0;
            while (similar_degree_[vertex] < static_cast<std::int64_t>(mu_) &&
                   effective_degree_[vertex] >= static_cast<std::int64_t>(mu_) &&
                   position < edge_buffer.size()) {
                const auto neighbor = edge_buffer[position];
                auto edge_state = state(vertex, neighbor);
                if (edge_state == EdgeState::Unknown) {
                    edge_state = exact_check(vertex, neighbor);
                    if (edge_state == EdgeState::Similar) {
                        ++similar_degree_[vertex];
                    } else {
                        --effective_degree_[vertex];
                    }
                    update_other_after_check(neighbor, edge_state);
                }
                ++position;
            }

            effective_degree_[vertex] = -1;
            if (similar_degree_[vertex] < static_cast<std::int64_t>(mu_)) {
                continue;
            }

            for (const auto neighbor : edge_buffer) {
                if (state(vertex, neighbor) == EdgeState::Similar &&
                    similar_degree_[neighbor] >= static_cast<std::int64_t>(mu_)) {
                    components_.unite(vertex, neighbor);
                }
            }

            while (position < edge_buffer.size()) {
                const auto neighbor = edge_buffer[position++];
                auto edge_state = state(vertex, neighbor);
                if (edge_state != EdgeState::Unknown ||
                    similar_degree_[neighbor] < static_cast<std::int64_t>(mu_) ||
                    components_.find(vertex) == components_.find(neighbor)) {
                    continue;
                }
                edge_state = exact_check(vertex, neighbor);
                update_other_after_check(neighbor, edge_state);
                if (edge_state == EdgeState::Similar) {
                    components_.unite(vertex, neighbor);
                }
            }
        }
    }

    PscanOnFliResult cluster_noncore_vertices() {
        PscanOnFliResult result;
        const auto vertex_count = static_cast<std::size_t>(index_.vertex_count());
        result.is_core.resize(vertex_count, false);
        result.core_cluster.assign(vertex_count,
                                   std::numeric_limits<VertexId>::max());

        std::vector<VertexId> component_minimum(
            vertex_count, std::numeric_limits<VertexId>::max());
        for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
            if (similar_degree_[vertex] < static_cast<std::int64_t>(mu_)) {
                continue;
            }
            result.is_core[vertex] = true;
            ++stats_.core_vertices;
            const auto root = components_.find(static_cast<VertexId>(vertex));
            component_minimum[root] =
                std::min(component_minimum[root], static_cast<VertexId>(vertex));
        }
        for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
            if (!result.is_core[vertex]) {
                continue;
            }
            const auto root = components_.find(static_cast<VertexId>(vertex));
            result.core_cluster[vertex] = component_minimum[root];
            if (component_minimum[root] == vertex) {
                ++stats_.clusters;
            }
        }

        result.noncore_clusters.resize(vertex_count);
        for (std::size_t core = 0; core < vertex_count; ++core) {
            if (!result.is_core[core]) {
                continue;
            }
            const auto neighbors =
                neighborhood(static_cast<VertexId>(core), true, false, &result.is_core);
            for (const auto neighbor : neighbors) {
                if (result.is_core[neighbor]) {
                    continue;
                }
                auto edge_state = state(static_cast<VertexId>(core), neighbor);
                if (edge_state == EdgeState::Unknown) {
                    edge_state = exact_check(static_cast<VertexId>(core), neighbor);
                }
                if (edge_state == EdgeState::Similar) {
                    result.noncore_clusters[neighbor].push_back(
                        result.core_cluster[core]);
                }
            }
        }

        result.roles.resize(vertex_count, PscanVertexRole::Outlier);
        for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
            if (result.is_core[vertex]) {
                result.roles[vertex] = PscanVertexRole::Core;
                continue;
            }
            auto& memberships = result.noncore_clusters[vertex];
            std::sort(memberships.begin(), memberships.end());
            memberships.erase(std::unique(memberships.begin(), memberships.end()),
                              memberships.end());
            if (memberships.empty()) {
                result.roles[vertex] = PscanVertexRole::Outlier;
                ++stats_.outlier_vertices;
            } else if (memberships.size() == 1) {
                result.roles[vertex] = PscanVertexRole::Border;
                ++stats_.border_vertices;
            } else {
                result.roles[vertex] = PscanVertexRole::Hub;
                ++stats_.hub_vertices;
            }
        }
        return result;
    }

    const FactorIndex& index_;
    const SimilarityThreshold& threshold_;
    std::uint64_t mu_ = 0;
    std::unique_ptr<AnchorFilter> anchor_filter_;
    std::vector<std::int64_t> similar_degree_;
    std::vector<std::int64_t> effective_degree_;
    DisjointSet components_;
    std::vector<VertexId> core_stack_;
    std::priority_queue<EffectiveEntry> effective_queue_;
    EdgeCertificateTable certificates_;
    SimilarityWorkspace similarity_workspace_;
    NeighborhoodCache neighborhood_cache_;
    const FingerprintNeighborhoodIndex* fingerprint_index_ = nullptr;
    const BudgetedSimilarityIndex* budgeted_index_ = nullptr;
    std::unique_ptr<ExactNeighborhoodCache> adaptive_cache_;
#ifdef HINSCAN_SHARED_GROUPS
    std::unique_ptr<SharedGroupDiagnostics> shared_groups_;
#endif
    BlockExecutionMode block_mode_;
    VertexId activated_vertex_ = std::numeric_limits<VertexId>::max();
    std::vector<VertexId> seed_components_;
    std::vector<std::size_t> certified_prefix_;
    PscanOnFliStats stats_;
};

}  // namespace

const char* pscan_role_name(PscanVertexRole role) noexcept {
    switch (role) {
        case PscanVertexRole::Core:
            return "core";
        case PscanVertexRole::Border:
            return "border";
        case PscanVertexRole::Hub:
            return "hub";
        case PscanVertexRole::Outlier:
            return "outlier";
    }
    return "unknown";
}

PscanOnFliResult run_pscan_on_fli(const FactorIndex& index,
                                  const SimilarityThreshold& threshold,
                                  std::uint64_t mu,
                                  std::uint64_t neighborhood_cache_bytes,
                                  const FingerprintNeighborhoodIndex*
                                      fingerprint_index,
                                  const BudgetedSimilarityIndex*
                                      budgeted_index,
                                  bool adaptive_neighborhoods,
                                  BlockExecutionMode block_mode) {
    if (mu == 0) {
        throw std::invalid_argument("mu must be positive");
    }
    return PscanExecution(index, threshold, mu, neighborhood_cache_bytes,
                          fingerprint_index, budgeted_index, adaptive_neighborhoods, block_mode)
        .run();
}

void write_pscan_on_fli_results(
    const std::filesystem::path& output_directory,
    const std::string& epsilon_text,
    std::uint64_t mu,
    const PscanOnFliResult& result) {
    std::filesystem::create_directories(output_directory);
    const auto suffix = epsilon_text + "-" + std::to_string(mu) + ".txt";
    std::ofstream output(output_directory / ("result-" + suffix),
                         std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create pSCAN-on-FLI result file");
    }
    output << "c/n vertex_id cluster_id\n";
    for (std::size_t vertex = 0; vertex < result.is_core.size(); ++vertex) {
        if (result.is_core[vertex]) {
            output << "c " << vertex << ' ' << result.core_cluster[vertex] << '\n';
        }
    }

    std::vector<std::pair<VertexId, VertexId>> memberships;
    for (std::size_t vertex = 0; vertex < result.noncore_clusters.size(); ++vertex) {
        for (const auto cluster : result.noncore_clusters[vertex]) {
            memberships.emplace_back(cluster, static_cast<VertexId>(vertex));
        }
    }
    std::sort(memberships.begin(), memberships.end());
    for (const auto& [cluster, vertex] : memberships) {
        output << "n " << vertex << ' ' << cluster << '\n';
    }

    std::ofstream roles(output_directory / ("roles-" + suffix),
                        std::ios::trunc);
    if (!roles) {
        throw std::runtime_error("cannot create pSCAN-on-FLI role file");
    }
    roles << "vertex_id role cluster_count clusters\n";
    for (std::size_t vertex = 0; vertex < result.roles.size(); ++vertex) {
        roles << vertex << ' ' << pscan_role_name(result.roles[vertex]) << ' ';
        if (result.is_core[vertex]) {
            roles << 1 << ' ' << result.core_cluster[vertex];
        } else {
            roles << result.noncore_clusters[vertex].size();
            for (const auto cluster : result.noncore_clusters[vertex]) {
                roles << ' ' << cluster;
            }
        }
        roles << '\n';
    }
}

}  // namespace hinscan
