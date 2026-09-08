#include "index/FingerprintNeighborhoodIndex.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace hinscan {
namespace {

constexpr std::array<char, 8> kMagic{{'F', 'N', 'I', 'C', '1', '1', '\n', 0}};
constexpr std::uint32_t kVersion = 4;
constexpr std::size_t kWords = 32;
constexpr std::size_t kBuckets = kWords * 64;

std::uint32_t fingerprint(VertexId vertex) noexcept {
    std::uint32_t value = vertex;
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value & static_cast<std::uint32_t>(kBuckets - 1);
}

std::uint32_t population_count(std::uint64_t value) noexcept {
#if defined(_MSC_VER)
    return static_cast<std::uint32_t>(__popcnt64(value));
#else
    return static_cast<std::uint32_t>(__builtin_popcountll(value));
#endif
}

template <typename T>
void write_value(std::ostream& output, T value) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "binary value must be trivially copyable");
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if (!output) {
        throw std::runtime_error("failed to write fingerprint index");
    }
}

template <typename T>
T read_value(std::istream& input, const char* field) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "binary value must be trivially copyable");
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) {
        throw std::runtime_error(std::string("truncated fingerprint index at ") +
                                 field);
    }
    return value;
}

template <typename T>
void read_vector(std::istream& input, std::vector<T>* values,
                 const char* field) {
    input.read(reinterpret_cast<char*>(values->data()),
               static_cast<std::streamsize>(values->size() * sizeof(T)));
    if (!input) {
        throw std::runtime_error(std::string("truncated fingerprint index at ") +
                                 field);
    }
}

}  // namespace

FingerprintNeighborhoodIndex FingerprintNeighborhoodIndex::build(
    const FactorIndex& factor) {
    FingerprintNeighborhoodIndex result;
    const auto vertices = factor.vertex_count();
    if (vertices > static_cast<std::uint64_t>(
                       std::numeric_limits<std::size_t>::max()) ||
        vertices > std::numeric_limits<std::size_t>::max() / kWords) {
        throw std::overflow_error("fingerprint vertex count exceeds this machine");
    }
    result.degrees_.resize(static_cast<std::size_t>(vertices));
    result.maximum_bucket_loads_.resize(static_cast<std::size_t>(vertices));
    result.occupancy_words_.assign(static_cast<std::size_t>(vertices) * kWords,
                                   0);
    result.count_offsets_.reserve(static_cast<std::size_t>(vertices) + 1);
    result.count_offsets_.push_back(0);
    result.count_overflow_.assign(static_cast<std::size_t>(vertices), 0);
    std::array<std::uint32_t, kBuckets> bucket_counts{};
    std::vector<std::uint32_t> touched_buckets;

    for (std::uint64_t vertex64 = 0; vertex64 < vertices; ++vertex64) {
        const auto vertex = static_cast<VertexId>(vertex64);
        const auto neighborhood = factor.collect_closed_neighborhood(vertex);
        if (neighborhood.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("fingerprint neighborhood exceeds 32 bits");
        }
        result.degrees_[static_cast<std::size_t>(vertex)] =
            static_cast<std::uint32_t>(neighborhood.size());
        result.fingerprint_count_ += neighborhood.size();
        auto* words = result.occupancy_words_.data() +
                      static_cast<std::size_t>(vertex) * kWords;
        std::uint32_t maximum_load = 0;
        touched_buckets.clear();
        for (const auto neighbor : neighborhood) {
            const auto bucket = fingerprint(neighbor);
            if (bucket_counts[bucket] == 0) {
                touched_buckets.push_back(bucket);
                words[bucket / 64] |= std::uint64_t{1} << (bucket % 64);
            }
            maximum_load = std::max(maximum_load, ++bucket_counts[bucket]);
        }
        result.maximum_bucket_loads_[vertex] = maximum_load;
        std::sort(touched_buckets.begin(), touched_buckets.end());
        for (const auto bucket : touched_buckets) {
            if (bucket_counts[bucket] >
                std::numeric_limits<std::uint8_t>::max()) {
                result.count_overflow_[vertex] = 1;
                result.bucket_counts_.push_back(
                    std::numeric_limits<std::uint8_t>::max());
            } else {
                result.bucket_counts_.push_back(
                    static_cast<std::uint8_t>(bucket_counts[bucket]));
            }
            bucket_counts[bucket] = 0;
        }
        result.count_offsets_.push_back(result.bucket_counts_.size());
    }
    return result;
}

FingerprintNeighborhoodIndex FingerprintNeighborhoodIndex::load(
    const std::filesystem::path& index_file) {
    std::ifstream input(index_file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open fingerprint index: " +
                                 index_file.string());
    }
    std::array<char, 8> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != kMagic) {
        throw std::runtime_error("invalid fingerprint index magic");
    }
    if (read_value<std::uint32_t>(input, "version") != kVersion) {
        throw std::runtime_error("unsupported fingerprint index version");
    }
    const auto vertices = read_value<std::uint64_t>(input, "vertex count");
    const auto count = read_value<std::uint64_t>(input, "fingerprint count");
    if (vertices > static_cast<std::uint64_t>(
                       std::numeric_limits<std::size_t>::max()) ||
        vertices > std::numeric_limits<std::size_t>::max() / kWords) {
        throw std::overflow_error("fingerprint index exceeds this machine");
    }

    FingerprintNeighborhoodIndex result;
    result.fingerprint_count_ = count;
    result.degrees_.resize(static_cast<std::size_t>(vertices));
    result.maximum_bucket_loads_.resize(static_cast<std::size_t>(vertices));
    result.occupancy_words_.resize(static_cast<std::size_t>(vertices) * kWords);
    result.count_offsets_.resize(static_cast<std::size_t>(vertices) + 1);
    result.count_overflow_.resize(static_cast<std::size_t>(vertices));
    read_vector(input, &result.degrees_, "degrees");
    read_vector(input, &result.maximum_bucket_loads_, "maximum bucket loads");
    read_vector(input, &result.occupancy_words_, "occupancy words");
    read_vector(input, &result.count_offsets_, "count offsets");
    if (result.count_offsets_.empty() || result.count_offsets_.front() != 0 ||
        !std::is_sorted(result.count_offsets_.begin(),
                        result.count_offsets_.end())) {
        throw std::runtime_error("invalid fingerprint count offsets");
    }
    result.bucket_counts_.resize(
        static_cast<std::size_t>(result.count_offsets_.back()));
    read_vector(input, &result.bucket_counts_, "bucket counts");
    read_vector(input, &result.count_overflow_, "count overflow flags");
    if (input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("unexpected trailing fingerprint index data");
    }
    std::uint64_t degree_sum = 0;
    for (std::size_t vertex = 0; vertex < result.degrees_.size(); ++vertex) {
        degree_sum += result.degrees_[vertex];
        if (result.degrees_[vertex] != 0 &&
            result.maximum_bucket_loads_[vertex] == 0) {
            throw std::runtime_error("invalid fingerprint maximum bucket load");
        }
        if (result.count_overflow_[vertex] > 1) {
            throw std::runtime_error("invalid fingerprint overflow flag");
        }
        std::uint64_t occupied = 0;
        for (std::size_t word = 0; word < kWords; ++word) {
            occupied += population_count(
                result.occupancy_words_[vertex * kWords + word]);
        }
        if (result.count_offsets_[vertex + 1] -
                result.count_offsets_[vertex] != occupied) {
            throw std::runtime_error(
                "fingerprint occupancy and bucket counts disagree");
        }
    }
    if (degree_sum != count) {
        throw std::runtime_error("fingerprint count and degrees disagree");
    }
    return result;
}

void FingerprintNeighborhoodIndex::save(
    const std::filesystem::path& index_file) const {
    if (!index_file.parent_path().empty()) {
        std::filesystem::create_directories(index_file.parent_path());
    }
    std::ofstream output(index_file, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create fingerprint index: " +
                                 index_file.string());
    }
    output.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    write_value(output, kVersion);
    write_value(output, vertex_count());
    write_value(output, fingerprint_count());
    output.write(reinterpret_cast<const char*>(degrees_.data()),
                 static_cast<std::streamsize>(degrees_.size() *
                                              sizeof(std::uint32_t)));
    output.write(reinterpret_cast<const char*>(maximum_bucket_loads_.data()),
                 static_cast<std::streamsize>(maximum_bucket_loads_.size() *
                                              sizeof(std::uint32_t)));
    output.write(reinterpret_cast<const char*>(occupancy_words_.data()),
                 static_cast<std::streamsize>(occupancy_words_.size() *
                                              sizeof(std::uint64_t)));
    output.write(reinterpret_cast<const char*>(count_offsets_.data()),
                 static_cast<std::streamsize>(count_offsets_.size() *
                                              sizeof(std::uint64_t)));
    output.write(reinterpret_cast<const char*>(bucket_counts_.data()),
                 static_cast<std::streamsize>(bucket_counts_.size()));
    output.write(reinterpret_cast<const char*>(count_overflow_.data()),
                 static_cast<std::streamsize>(count_overflow_.size()));
    if (!output) {
        throw std::runtime_error("failed to write fingerprint index payload");
    }
}

std::uint64_t FingerprintNeighborhoodIndex::vertex_count() const noexcept {
    return degrees_.size();
}

std::uint64_t FingerprintNeighborhoodIndex::fingerprint_count() const noexcept {
    return fingerprint_count_;
}

std::uint64_t FingerprintNeighborhoodIndex::neighborhood_size(
    VertexId vertex) const {
    if (static_cast<std::uint64_t>(vertex) >= vertex_count()) {
        throw std::out_of_range("fingerprint vertex is out of range");
    }
    return degrees_[vertex];
}

std::uint64_t FingerprintNeighborhoodIndex::estimated_bytes() const noexcept {
    return static_cast<std::uint64_t>(degrees_.size()) * sizeof(std::uint32_t) +
           static_cast<std::uint64_t>(maximum_bucket_loads_.size()) *
               sizeof(std::uint32_t) +
           static_cast<std::uint64_t>(occupancy_words_.size()) *
               sizeof(std::uint64_t) +
           static_cast<std::uint64_t>(count_offsets_.size()) *
               sizeof(std::uint64_t) +
           static_cast<std::uint64_t>(bucket_counts_.size()) +
           static_cast<std::uint64_t>(count_overflow_.size());
}

FingerprintBoundResult FingerprintNeighborhoodIndex::check_upper_bound(
    VertexId left,
    VertexId right,
    std::uint64_t required_common_neighbors) const {
    if (static_cast<std::uint64_t>(left) >= vertex_count() ||
        static_cast<std::uint64_t>(right) >= vertex_count()) {
        throw std::out_of_range("fingerprint similarity vertex is out of range");
    }
    FingerprintBoundResult result;
    const auto maximum_load = std::min(maximum_bucket_loads_[left],
                                       maximum_bucket_loads_[right]);
    const auto* left_words = occupancy_words_.data() +
                             static_cast<std::size_t>(left) * kWords;
    const auto* right_words = occupancy_words_.data() +
                              static_cast<std::size_t>(right) * kWords;
    for (std::size_t word = 0; word < kWords; ++word) {
        result.fingerprint_matches +=
            population_count(left_words[word] & right_words[word]);
        ++result.entries_read;
    }
    if (result.fingerprint_matches * maximum_load <
        required_common_neighbors) {
        result.may_reach_required = false;
        return result;
    }
    if (count_overflow_[left] != 0 || count_overflow_[right] != 0) {
        return result;
    }

    // Refine the coarse max-load bound with the exact population of each
    // common hash bucket.  Hash collisions can only increase this sum, so a
    // value below required is still a proof of dissimilarity.
    result.fingerprint_matches = 0;
    auto left_count = count_offsets_[left];
    auto right_count = count_offsets_[right];
    for (std::size_t word = 0; word < kWords; ++word) {
        const auto left_word = left_words[word];
        const auto right_word = right_words[word];
        auto common_word = left_word & right_word;
        while (common_word != 0) {
#if defined(_MSC_VER)
            unsigned long bit = 0;
            _BitScanForward64(&bit, common_word);
            const auto bit_index = static_cast<std::uint32_t>(bit);
#else
            const auto bit_index = static_cast<std::uint32_t>(
                __builtin_ctzll(common_word));
#endif
            const auto lower_mask = bit_index == 0
                ? std::uint64_t{0}
                : ((std::uint64_t{1} << bit_index) - 1);
            const auto left_rank = population_count(left_word & lower_mask);
            const auto right_rank = population_count(right_word & lower_mask);
            result.fingerprint_matches += std::min(
                bucket_counts_[left_count + left_rank],
                bucket_counts_[right_count + right_rank]);
            ++result.entries_read;
            if (result.fingerprint_matches >= required_common_neighbors) {
                return result;
            }
            common_word &= common_word - 1;
        }
        left_count += population_count(left_word);
        right_count += population_count(right_word);
    }
    result.may_reach_required = false;
    return result;
}

}  // namespace hinscan
