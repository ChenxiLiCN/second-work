#include "index/BudgetedSimilarityIndex.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace hinscan {
namespace {

constexpr std::array<char, 8> kMagic{{'B', 'S', 'I', '1', '\r', '\n', 0, 0}};
constexpr std::uint32_t kVersion = 1;

std::uint64_t make_edge_key(VertexId left, VertexId right) noexcept {
    if (left > right) {
        std::swap(left, right);
    }
    return (static_cast<std::uint64_t>(left) << 32U) | right;
}

std::uint32_t intersection_size(const std::vector<VertexId>& left,
                                const std::vector<VertexId>& right) {
    std::size_t left_position = 0;
    std::size_t right_position = 0;
    std::uint64_t count = 0;
    while (left_position < left.size() && right_position < right.size()) {
        if (left[left_position] < right[right_position]) {
            ++left_position;
        } else if (right[right_position] < left[left_position]) {
            ++right_position;
        } else {
            ++count;
            ++left_position;
            ++right_position;
        }
    }
    if (count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("budgeted common-neighbor count exceeds 32 bits");
    }
    return static_cast<std::uint32_t>(count);
}

template <typename T>
void write_value(std::ostream& output, T value) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "binary value must be trivially copyable");
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if (!output) {
        throw std::runtime_error("failed to write budgeted similarity index");
    }
}

template <typename T>
T read_value(std::istream& input, const char* field) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) {
        throw std::runtime_error(std::string("truncated budgeted index at ") +
                                 field);
    }
    return value;
}

struct Candidate {
    std::uint64_t score = 0;
    std::uint64_t edge_key = 0;
};

struct CandidateGreater {
    bool operator()(const Candidate& left, const Candidate& right) const {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.edge_key > right.edge_key;
    }
};

}  // namespace

BudgetedSimilarityIndex BudgetedSimilarityIndex::build(
    const FactorIndex& factor, std::uint64_t maximum_entries) {
    const auto begin = std::chrono::steady_clock::now();
    BudgetedSimilarityIndex result;
    result.vertex_count_ = factor.vertex_count();
    if (maximum_entries == 0 || result.vertex_count_ == 0) {
        return result;
    }

    std::vector<std::vector<VertexId>> neighborhoods(
        static_cast<std::size_t>(result.vertex_count_));
    std::vector<std::uint64_t> scan_costs(
        static_cast<std::size_t>(result.vertex_count_), 1);
    for (std::uint64_t vertex64 = 0; vertex64 < result.vertex_count_; ++vertex64) {
        const auto vertex = static_cast<VertexId>(vertex64);
        neighborhoods[vertex] = factor.collect_closed_neighborhood(vertex);
        result.stats_.temporary_neighborhood_bytes +=
            neighborhoods[vertex].capacity() * sizeof(VertexId);
        for (const auto witness : factor.witnesses(vertex)) {
            scan_costs[vertex] += factor.posting(witness).size();
        }
    }

    std::priority_queue<Candidate, std::vector<Candidate>, CandidateGreater>
        selected;
    for (std::uint64_t left64 = 0; left64 < result.vertex_count_; ++left64) {
        const auto left = static_cast<VertexId>(left64);
        for (const auto right : neighborhoods[left]) {
            if (right <= left) {
                continue;
            }
            ++result.stats_.candidate_edges;
            // Similarity queries first eliminate strongly mismatched degrees.
            // Favor expensive edges whose endpoint degrees are also close,
            // without referring to any particular epsilon or mu.
            const auto left_degree = factor.degree(left);
            const auto right_degree = factor.degree(right);
            const auto smaller_degree = std::min(left_degree, right_degree);
            const auto larger_degree = std::max(left_degree, right_degree);
            const auto raw_cost = std::max(scan_costs[left], scan_costs[right]);
            const auto ratio_scale = std::uint64_t{1} << 20U;
            const auto degree_ratio =
                (smaller_degree + 1) * ratio_scale / (larger_degree + 1);
            const auto score = raw_cost >
                    std::numeric_limits<std::uint64_t>::max() /
                        std::max<std::uint64_t>(degree_ratio, 1)
                ? std::numeric_limits<std::uint64_t>::max()
                : raw_cost * std::max<std::uint64_t>(degree_ratio, 1);
            const Candidate candidate{score, make_edge_key(left, right)};
            if (selected.size() < maximum_entries) {
                selected.push(candidate);
            } else if (candidate.score > selected.top().score ||
                       (candidate.score == selected.top().score &&
                        candidate.edge_key < selected.top().edge_key)) {
                selected.pop();
                selected.push(candidate);
            }
        }
    }

    std::vector<Candidate> retained;
    retained.reserve(selected.size());
    while (!selected.empty()) {
        retained.push_back(selected.top());
        selected.pop();
    }
    result.entries_.reserve(retained.size());
    for (const auto& candidate : retained) {
        const auto left = static_cast<VertexId>(candidate.edge_key >> 32U);
        const auto right = static_cast<VertexId>(candidate.edge_key);
        result.entries_.push_back(BudgetedSimilarityEntry{
            candidate.edge_key,
            intersection_size(neighborhoods[left], neighborhoods[right])});
    }
    std::sort(result.entries_.begin(), result.entries_.end(),
              [](const auto& left, const auto& right) {
                  return left.edge_key < right.edge_key;
              });
    result.stats_.retained_edges = result.entries_.size();
    result.stats_.build_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count());
    return result;
}

BudgetedSimilarityIndex BudgetedSimilarityIndex::load(
    const std::filesystem::path& index_file) {
    std::ifstream input(index_file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open budgeted similarity index: " +
                                 index_file.string());
    }
    std::array<char, 8> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != kMagic) {
        throw std::runtime_error("invalid budgeted similarity index magic");
    }
    if (read_value<std::uint32_t>(input, "version") != kVersion) {
        throw std::runtime_error("unsupported budgeted similarity index version");
    }
    BudgetedSimilarityIndex result;
    result.vertex_count_ = read_value<std::uint64_t>(input, "vertex count");
    const auto count = read_value<std::uint64_t>(input, "entry count");
    if (count > std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error("budgeted similarity index exceeds this machine");
    }
    result.entries_.resize(static_cast<std::size_t>(count));
    for (auto& entry : result.entries_) {
        entry.edge_key = read_value<std::uint64_t>(input, "edge key");
        entry.common_neighbors =
            read_value<std::uint32_t>(input, "common-neighbor count");
    }
    if (!std::is_sorted(result.entries_.begin(), result.entries_.end(),
                        [](const auto& left, const auto& right) {
                            return left.edge_key < right.edge_key;
                        })) {
        throw std::runtime_error("budgeted similarity entries are not sorted");
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("unexpected trailing budgeted index data");
    }
    result.stats_.retained_edges = count;
    return result;
}

void BudgetedSimilarityIndex::save(
    const std::filesystem::path& index_file) const {
    if (!index_file.parent_path().empty()) {
        std::filesystem::create_directories(index_file.parent_path());
    }
    std::ofstream output(index_file, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create budgeted similarity index: " +
                                 index_file.string());
    }
    output.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    write_value(output, kVersion);
    write_value(output, vertex_count_);
    write_value(output, entry_count());
    for (const auto& entry : entries_) {
        write_value(output, entry.edge_key);
        write_value(output, entry.common_neighbors);
    }
}

bool BudgetedSimilarityIndex::find(VertexId left, VertexId right,
                                   std::uint32_t* common_neighbors) const {
    const auto key = make_edge_key(left, right);
    const auto iterator = std::lower_bound(
        entries_.begin(), entries_.end(), key,
        [](const BudgetedSimilarityEntry& entry, std::uint64_t value) {
            return entry.edge_key < value;
        });
    if (iterator == entries_.end() || iterator->edge_key != key) {
        return false;
    }
    *common_neighbors = iterator->common_neighbors;
    return true;
}

std::uint64_t BudgetedSimilarityIndex::vertex_count() const noexcept {
    return vertex_count_;
}

std::uint64_t BudgetedSimilarityIndex::entry_count() const noexcept {
    return entries_.size();
}

std::uint64_t BudgetedSimilarityIndex::estimated_bytes() const noexcept {
    return static_cast<std::uint64_t>(entries_.size()) *
           (sizeof(std::uint64_t) + sizeof(std::uint32_t));
}

const BudgetedSimilarityBuildStats& BudgetedSimilarityIndex::stats() const
    noexcept {
    return stats_;
}

}  // namespace hinscan
