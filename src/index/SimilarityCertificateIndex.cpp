#include "index/SimilarityCertificateIndex.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hinscan {
namespace {

#if !defined(__SIZEOF_INT128__)
#error "SimilarityCertificateIndex currently requires unsigned __int128"
#endif

using Wide = unsigned __int128;

Wide square_wide(std::uint64_t value) {
    return static_cast<Wide>(value) * static_cast<Wide>(value);
}

std::uint64_t intersection_size(const std::vector<VertexId>& left,
                                const std::vector<VertexId>& right) {
    std::uint64_t common = 0;
    auto l = left.begin();
    auto r = right.begin();
    while (l != left.end() && r != right.end()) {
        if (*l < *r) {
            ++l;
        } else if (*r < *l) {
            ++r;
        } else {
            ++common;
            ++l;
            ++r;
        }
    }
    return common;
}

template <typename T>
void write_value(std::ofstream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if (!output) {
        throw std::runtime_error("failed to write similarity certificate index");
    }
}

template <typename T>
T read_value(std::ifstream& input, const char* field) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!input) {
        throw std::runtime_error(
            std::string("truncated similarity certificate index at ") + field);
    }
    return value;
}

std::uint32_t bits_required(std::uint64_t maximum_value) {
    std::uint32_t bits = 0;
    do {
        ++bits;
        maximum_value >>= 1U;
    } while (maximum_value != 0);
    return bits;
}

class BitWriter {
public:
    explicit BitWriter(std::uint64_t total_bits)
        : words_(static_cast<std::size_t>((total_bits + 63) / 64), 0),
          total_bits_(total_bits) {}

    void append(std::uint32_t value, std::uint32_t bits) {
        if (bits == 0 || bits > 32 ||
            (bits < 32 && value >= (1ULL << bits))) {
            throw std::logic_error("SCI value does not fit its packed width");
        }
        const auto word = static_cast<std::size_t>(position_ / 64);
        const auto offset = static_cast<std::uint32_t>(position_ % 64);
        words_[word] |= static_cast<std::uint64_t>(value) << offset;
        if (offset + bits > 64) {
            words_[word + 1] |=
                static_cast<std::uint64_t>(value) >> (64 - offset);
        }
        position_ += bits;
    }

    std::uint64_t byte_size() const noexcept {
        return (total_bits_ + 7) / 8;
    }

    const char* data() const noexcept {
        return reinterpret_cast<const char*>(words_.data());
    }

    void require_complete() const {
        if (position_ != total_bits_) {
            throw std::logic_error("SCI packed payload size is inconsistent");
        }
    }

private:
    std::vector<std::uint64_t> words_;
    std::uint64_t total_bits_ = 0;
    std::uint64_t position_ = 0;
};

class BitReader {
public:
    BitReader(std::vector<std::uint64_t> words, std::uint64_t total_bits)
        : words_(std::move(words)), total_bits_(total_bits) {}

    std::uint32_t consume(std::uint32_t bits) {
        if (bits == 0 || bits > 32 || position_ + bits > total_bits_) {
            throw std::runtime_error("truncated SCI packed payload");
        }
        const auto word = static_cast<std::size_t>(position_ / 64);
        const auto offset = static_cast<std::uint32_t>(position_ % 64);
        std::uint64_t value = words_[word] >> offset;
        if (offset + bits > 64) {
            value |= words_[word + 1] << (64 - offset);
        }
        if (bits < 32) {
            value &= (1ULL << bits) - 1;
        } else {
            value &= std::numeric_limits<std::uint32_t>::max();
        }
        position_ += bits;
        return static_cast<std::uint32_t>(value);
    }

    void require_complete() const {
        if (position_ != total_bits_) {
            throw std::runtime_error("SCI packed payload has trailing bits");
        }
    }

private:
    std::vector<std::uint64_t> words_;
    std::uint64_t total_bits_ = 0;
    std::uint64_t position_ = 0;
};

class DisjointSet {
public:
    explicit DisjointSet(std::size_t size) : parent_(size), rank_(size, 0) {
        std::iota(parent_.begin(), parent_.end(), 0);
    }

    VertexId find(VertexId value) {
        if (parent_[value] != value) {
            parent_[value] = find(parent_[value]);
        }
        return parent_[value];
    }

    void unite(VertexId left, VertexId right) {
        left = find(left);
        right = find(right);
        if (left == right) { return; }
        if (rank_[left] < rank_[right]) { std::swap(left, right); }
        parent_[right] = left;
        if (rank_[left] == rank_[right]) { ++rank_[left]; }
    }

private:
    std::vector<VertexId> parent_;
    std::vector<std::uint8_t> rank_;
};

}  // namespace

SimilarityCertificateIndex SimilarityCertificateIndex::build(
    const FactorIndex& factor) {
    const auto begin = std::chrono::steady_clock::now();
    SimilarityCertificateIndex result;
    result.stats_.vertices = factor.vertex_count();
    result.stats_.projected_edges = factor.stats().exact_projected_edges;
    if (factor.vertex_count() > std::numeric_limits<VertexId>::max()) {
        throw std::overflow_error("SCI supports at most 32-bit vertex ids");
    }

    std::vector<std::vector<VertexId>> neighborhoods(
        static_cast<std::size_t>(factor.vertex_count()));
    result.degrees_.resize(neighborhoods.size());
    for (std::uint64_t vertex64 = 0; vertex64 < factor.vertex_count(); ++vertex64) {
        const auto vertex = static_cast<VertexId>(vertex64);
        neighborhoods[vertex] = factor.collect_closed_neighborhood(vertex);
        if (neighborhoods[vertex].size() >
            std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("SCI closed degree exceeds 32 bits");
        }
        result.degrees_[vertex] =
            static_cast<std::uint32_t>(neighborhoods[vertex].size());
        result.stats_.closed_neighborhood_entries +=
            neighborhoods[vertex].size();
    }

    result.certificates_.reserve(
        static_cast<std::size_t>(result.stats_.projected_edges));
    for (std::uint64_t left64 = 0; left64 < factor.vertex_count(); ++left64) {
        const auto left = static_cast<VertexId>(left64);
        for (const auto right : neighborhoods[left]) {
            if (right <= left) { continue; }
            const auto common = intersection_size(neighborhoods[left],
                                                  neighborhoods[right]);
            if (common > std::numeric_limits<std::uint32_t>::max()) {
                throw std::overflow_error("SCI common-neighbor count exceeds 32 bits");
            }
            result.certificates_.push_back(
                {left, right, static_cast<std::uint32_t>(common)});
        }
    }
    if (result.certificates_.size() != result.stats_.projected_edges) {
        throw std::logic_error("SCI edge enumeration disagrees with factor index");
    }

    const auto score_greater = [&](const SimilarityCertificate& left,
                                   const SimilarityCertificate& right) {
        const Wide left_numerator = square_wide(left.common_neighbors);
        const Wide right_numerator = square_wide(right.common_neighbors);
        const Wide left_denominator =
            static_cast<Wide>(result.degrees_[left.left]) *
            result.degrees_[left.right];
        const Wide right_denominator =
            static_cast<Wide>(result.degrees_[right.left]) *
            result.degrees_[right.right];
        const Wide lhs = left_numerator * right_denominator;
        const Wide rhs = right_numerator * left_denominator;
        if (lhs != rhs) { return lhs > rhs; }
        if (left.left != right.left) { return left.left < right.left; }
        return left.right < right.right;
    };
    std::sort(result.certificates_.begin(), result.certificates_.end(),
              score_greater);

    const auto end = std::chrono::steady_clock::now();
    result.stats_.build_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
    return result;
}

void SimilarityCertificateIndex::save(const std::filesystem::path& file) const {
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path());
    }
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create SCI index: " + file.string());
    }
    constexpr char magic[8] = {'S', 'C', 'I', '0', 'I', 'D', 'X', '\0'};
    output.write(magic, sizeof(magic));
    write_value(output, static_cast<std::uint32_t>(3));
    write_value(output, static_cast<std::uint32_t>(0x01020304U));
    const auto id_bits = bits_required(stats_.vertices == 0
                                           ? 0
                                           : stats_.vertices - 1);
    std::uint32_t maximum_count = 0;
    for (const auto degree : degrees_) {
        maximum_count = std::max(maximum_count, degree);
    }
    for (const auto& certificate : certificates_) {
        maximum_count = std::max(maximum_count,
                                 certificate.common_neighbors);
    }
    const auto count_bits = bits_required(maximum_count);
    write_value(output, id_bits);
    write_value(output, count_bits);
    write_value(output, stats_.vertices);
    write_value(output, stats_.projected_edges);
    write_value(output, stats_.closed_neighborhood_entries);
    write_value(output, stats_.build_milliseconds);
    const Wide total_bits_wide =
        static_cast<Wide>(stats_.vertices) * count_bits +
        static_cast<Wide>(stats_.projected_edges) *
            (2 * id_bits + count_bits);
    if (total_bits_wide > std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("SCI compact payload is too large");
    }
    const auto total_bits = static_cast<std::uint64_t>(total_bits_wide);
    BitWriter payload(total_bits);
    for (const auto degree : degrees_) {
        payload.append(degree, count_bits);
    }
    for (const auto& certificate : certificates_) {
        payload.append(certificate.left, id_bits);
        payload.append(certificate.right, id_bits);
        payload.append(certificate.common_neighbors, count_bits);
    }
    payload.require_complete();
    output.write(payload.data(),
                 static_cast<std::streamsize>(payload.byte_size()));
    if (!output) {
        throw std::runtime_error("failed to write SCI compact payload");
    }
}

SimilarityCertificateIndex SimilarityCertificateIndex::load(
    const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open SCI index: " + file.string());
    }
    constexpr char expected[8] = {'S', 'C', 'I', '0', 'I', 'D', 'X', '\0'};
    char magic[8]{};
    input.read(magic, sizeof(magic));
    const auto version = read_value<std::uint32_t>(input, "version");
    const auto endian = read_value<std::uint32_t>(input, "endian marker");
    if (std::memcmp(magic, expected, sizeof(magic)) != 0 ||
        (version != 1 && version != 2 && version != 3) ||
        endian != 0x01020304U) {
        throw std::runtime_error("unsupported SCI index format");
    }
    const auto stored_id_width =
        version >= 2 ? read_value<std::uint32_t>(input, "id width") : 4U;
    const auto stored_count_width =
        version >= 2 ? read_value<std::uint32_t>(input, "count width") : 4U;
    const auto id_bits = version >= 3 ? stored_id_width : 8 * stored_id_width;
    const auto count_bits =
        version >= 3 ? stored_count_width : 8 * stored_count_width;
    if (id_bits == 0 || id_bits > 32 || count_bits == 0 ||
        count_bits > 32) {
        throw std::runtime_error("unsupported SCI compact integer width");
    }
    SimilarityCertificateIndex result;
    result.stats_.vertices = read_value<std::uint64_t>(input, "vertices");
    result.stats_.projected_edges = read_value<std::uint64_t>(input, "edges");
    result.stats_.closed_neighborhood_entries =
        read_value<std::uint64_t>(input, "neighborhood entries");
    result.stats_.build_milliseconds = read_value<std::uint64_t>(input, "build time");
    if (result.stats_.vertices > std::numeric_limits<VertexId>::max() ||
        result.stats_.projected_edges >
            std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("SCI dimensions exceed this build");
    }
    result.degrees_.resize(static_cast<std::size_t>(result.stats_.vertices));
    result.certificates_.resize(
        static_cast<std::size_t>(result.stats_.projected_edges));
    const Wide total_bits_wide =
        static_cast<Wide>(result.stats_.vertices) * count_bits +
        static_cast<Wide>(result.stats_.projected_edges) *
            (2 * id_bits + count_bits);
    if (total_bits_wide > std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("SCI payload dimensions overflow");
    }
    const auto total_bits = static_cast<std::uint64_t>(total_bits_wide);
    const auto payload_size = static_cast<std::size_t>((total_bits + 7) / 8);
    std::vector<std::uint64_t> words((total_bits + 63) / 64, 0);
    input.read(reinterpret_cast<char*>(words.data()),
               static_cast<std::streamsize>(payload_size));
    if (!input) {
        throw std::runtime_error("truncated SCI compact payload");
    }
    BitReader payload(std::move(words), total_bits);
    for (auto& degree : result.degrees_) {
        degree = payload.consume(count_bits);
    }
    for (auto& certificate : result.certificates_) {
        certificate.left = payload.consume(id_bits);
        certificate.right = payload.consume(id_bits);
        certificate.common_neighbors = payload.consume(count_bits);
        if (certificate.left >= result.stats_.vertices ||
            certificate.right >= result.stats_.vertices ||
            certificate.left >= certificate.right) {
            throw std::runtime_error("SCI certificate endpoint is invalid");
        }
    }
    payload.require_complete();
    return result;
}

std::uint64_t SimilarityCertificateIndex::vertex_count() const noexcept {
    return stats_.vertices;
}

std::uint32_t SimilarityCertificateIndex::degree(VertexId vertex) const {
    return degrees_.at(vertex);
}

const std::vector<SimilarityCertificate>&
SimilarityCertificateIndex::certificates() const noexcept {
    return certificates_;
}

const SimilarityCertificateIndexStats&
SimilarityCertificateIndex::stats() const noexcept {
    return stats_;
}

std::uint64_t SimilarityCertificateIndex::serialized_bytes() const {
    const auto id_bits = bits_required(stats_.vertices == 0
                                           ? 0
                                           : stats_.vertices - 1);
    std::uint32_t maximum_count = 0;
    for (const auto degree : degrees_) {
        maximum_count = std::max(maximum_count, degree);
    }
    for (const auto& certificate : certificates_) {
        maximum_count = std::max(maximum_count,
                                 certificate.common_neighbors);
    }
    const auto count_bits = bits_required(maximum_count);
    constexpr std::uint64_t header_bytes =
        8 + 4 + 4 + 4 + 4 + 8 + 8 + 8 + 8;
    const Wide payload_bits =
        static_cast<Wide>(stats_.vertices) * count_bits +
        static_cast<Wide>(stats_.projected_edges) *
            (2 * id_bits + count_bits);
    const Wide bytes = static_cast<Wide>(header_bytes) +
                       (payload_bits + 7) / 8;
    if (bytes > std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("SCI serialized size exceeds 64 bits");
    }
    return static_cast<std::uint64_t>(bytes);
}

bool SimilarityCertificateIndex::score_at_least(
    const SimilarityCertificate& certificate,
    const SimilarityThreshold& threshold) const {
    if (threshold.numerator > std::numeric_limits<std::uint32_t>::max() ||
        threshold.denominator > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(
            "SCI epsilon precision currently supports at most 32-bit fractions");
    }
    const Wide left = square_wide(certificate.common_neighbors) *
                      square_wide(threshold.denominator);
    const Wide right = static_cast<Wide>(degrees_[certificate.left]) *
                       degrees_[certificate.right] *
                       square_wide(threshold.numerator);
    return left >= right;
}

std::size_t SimilarityCertificateIndex::similar_prefix_size(
    const SimilarityThreshold& threshold) const {
    std::size_t begin = 0;
    std::size_t end = certificates_.size();
    while (begin < end) {
        const auto middle = begin + (end - begin) / 2;
        if (score_at_least(certificates_[middle], threshold)) {
            begin = middle + 1;
        } else {
            end = middle;
        }
    }
    return begin;
}

SimilarityCertificateQueryResult run_pscan_on_similarity_certificates(
    const SimilarityCertificateIndex& index,
    const SimilarityThreshold& threshold,
    std::uint64_t mu) {
    if (mu == 0) { throw std::invalid_argument("mu must be positive"); }
    const auto begin = std::chrono::steady_clock::now();
    SimilarityCertificateQueryResult output;
    const auto prefix = index.similar_prefix_size(threshold);
    output.similar_edges = prefix;
    output.certificate_edges_skipped =
        index.certificates().size() - prefix;

    const auto vertex_count = static_cast<std::size_t>(index.vertex_count());
    std::vector<std::uint64_t> similar_degree(vertex_count, 0);
    for (std::size_t i = 0; i < prefix; ++i) {
        const auto& edge = index.certificates()[i];
        ++similar_degree[edge.left];
        ++similar_degree[edge.right];
    }

    std::vector<bool> core(vertex_count, false);
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        core[vertex] = similar_degree[vertex] >= mu;
    }
    DisjointSet components(vertex_count);
    for (std::size_t i = 0; i < prefix; ++i) {
        const auto& edge = index.certificates()[i];
        if (core[edge.left] && core[edge.right]) {
            components.unite(edge.left, edge.right);
        }
    }

    auto& result = output.clustering;
    result.is_core = core;
    result.core_cluster.assign(vertex_count,
                               std::numeric_limits<VertexId>::max());
    result.noncore_clusters.resize(vertex_count);
    result.roles.assign(vertex_count, PscanVertexRole::Outlier);
    std::vector<VertexId> component_minimum(
        vertex_count, std::numeric_limits<VertexId>::max());
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        if (!core[vertex]) { continue; }
        const auto root = components.find(static_cast<VertexId>(vertex));
        component_minimum[root] = std::min(
            component_minimum[root], static_cast<VertexId>(vertex));
    }
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        if (!core[vertex]) { continue; }
        const auto root = components.find(static_cast<VertexId>(vertex));
        result.core_cluster[vertex] = component_minimum[root];
        result.roles[vertex] = PscanVertexRole::Core;
        ++result.stats.core_vertices;
        if (component_minimum[root] == vertex) { ++result.stats.clusters; }
    }

    for (std::size_t i = 0; i < prefix; ++i) {
        const auto& edge = index.certificates()[i];
        if (core[edge.left] && !core[edge.right]) {
            result.noncore_clusters[edge.right].push_back(
                result.core_cluster[edge.left]);
        } else if (!core[edge.left] && core[edge.right]) {
            result.noncore_clusters[edge.left].push_back(
                result.core_cluster[edge.right]);
        }
    }
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        if (core[vertex]) { continue; }
        auto& memberships = result.noncore_clusters[vertex];
        std::sort(memberships.begin(), memberships.end());
        memberships.erase(std::unique(memberships.begin(), memberships.end()),
                          memberships.end());
        if (memberships.empty()) {
            result.roles[vertex] = PscanVertexRole::Outlier;
            ++result.stats.outlier_vertices;
        } else if (memberships.size() == 1) {
            result.roles[vertex] = PscanVertexRole::Border;
            ++result.stats.border_vertices;
        } else {
            result.roles[vertex] = PscanVertexRole::Hub;
            ++result.stats.hub_vertices;
        }
    }

    result.stats.projected_edges_seen_in_prune =
        index.stats().projected_edges;
    result.stats.exact_similarity_checks = 0;
    const auto end = std::chrono::steady_clock::now();
    result.stats.query_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
    return output;
}

}  // namespace hinscan
