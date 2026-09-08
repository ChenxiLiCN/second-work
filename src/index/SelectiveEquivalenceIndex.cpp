#include "index/SelectiveEquivalenceIndex.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hinscan {
namespace {

using Clock = std::chrono::steady_clock;
using Wide = unsigned __int128;

template <typename T>
T read_stream_value(std::istream& input, const char* field) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) {
        throw std::runtime_error(std::string("cannot read ") + field);
    }
    return value;
}

template <typename T>
void write_stream_value(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if (!output) {
        throw std::runtime_error("failed to write SECI index");
    }
}

struct MaterializedGraph {
    std::uint32_t vertices = 0;
    std::uint64_t directed_edges = 0;
    std::vector<std::uint32_t> degrees;
    std::vector<std::uint64_t> offsets;
};

MaterializedGraph read_graph_header(const std::filesystem::path& directory) {
    std::ifstream input(directory / "b_degree.bin", std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open materialized b_degree.bin");
    }
    const auto integer_size = read_stream_value<std::int32_t>(input, "int size");
    const auto vertices = read_stream_value<std::int32_t>(input, "vertices");
    const auto directed_edges =
        read_stream_value<std::int32_t>(input, "directed edges");
    if (integer_size != 4 || vertices < 0 || directed_edges < 0) {
        throw std::runtime_error("invalid pSCAN graph header");
    }
    MaterializedGraph graph;
    graph.vertices = static_cast<std::uint32_t>(vertices);
    graph.directed_edges = static_cast<std::uint64_t>(directed_edges);
    graph.degrees.resize(graph.vertices);
    graph.offsets.resize(static_cast<std::size_t>(graph.vertices) + 1, 0);
    std::uint64_t total = 0;
    for (std::uint32_t vertex = 0; vertex < graph.vertices; ++vertex) {
        const auto degree = read_stream_value<std::int32_t>(input, "degree");
        if (degree < 0) {
            throw std::runtime_error("negative materialized degree");
        }
        graph.degrees[vertex] = static_cast<std::uint32_t>(degree);
        total += graph.degrees[vertex];
        graph.offsets[static_cast<std::size_t>(vertex) + 1] = total;
    }
    if (total != graph.directed_edges || (total & 1U) != 0) {
        throw std::runtime_error("materialized degree sum is inconsistent");
    }
    return graph;
}

class ReadOnlyAdjacency {
public:
    ReadOnlyAdjacency(const std::filesystem::path& file,
                      std::uint64_t expected_integers) {
        const auto expected_bytes = expected_integers * sizeof(std::int32_t);
#ifdef _WIN32
        file_ = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("cannot open materialized b_adj.bin");
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size) || size.QuadPart < 0 ||
            static_cast<std::uint64_t>(size.QuadPart) != expected_bytes) {
            close();
            throw std::runtime_error("unexpected materialized adjacency size");
        }
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_ == nullptr) {
            close();
            throw std::runtime_error("cannot create adjacency mapping");
        }
        data_ = static_cast<const std::int32_t*>(
            MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (data_ == nullptr) {
            close();
            throw std::runtime_error("cannot map materialized adjacency");
        }
#else
        std::ifstream input(file, std::ios::binary | std::ios::ate);
        if (!input || static_cast<std::uint64_t>(input.tellg()) != expected_bytes) {
            throw std::runtime_error("unexpected materialized adjacency size");
        }
        input.seekg(0);
        owned_.resize(static_cast<std::size_t>(expected_integers));
        input.read(reinterpret_cast<char*>(owned_.data()),
                   static_cast<std::streamsize>(expected_bytes));
        if (!input) {
            throw std::runtime_error("cannot read materialized adjacency");
        }
        data_ = owned_.data();
#endif
    }

    ~ReadOnlyAdjacency() { close(); }
    ReadOnlyAdjacency(const ReadOnlyAdjacency&) = delete;
    ReadOnlyAdjacency& operator=(const ReadOnlyAdjacency&) = delete;
    const std::int32_t* data() const noexcept { return data_; }

private:
    void close() noexcept {
#ifdef _WIN32
        if (data_ != nullptr) {
            UnmapViewOfFile(data_);
            data_ = nullptr;
        }
        if (mapping_ != nullptr) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
#else
        data_ = nullptr;
        owned_.clear();
#endif
    }
    const std::int32_t* data_ = nullptr;
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
#else
    std::vector<std::int32_t> owned_;
#endif
};

struct Signature {
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::uint64_t size = 0;
    bool operator==(const Signature& other) const noexcept {
        return first == other.first && second == other.second &&
               size == other.size;
    }
};

struct SignatureHash {
    std::size_t operator()(const Signature& value) const noexcept {
        return static_cast<std::size_t>(
            value.first ^ (value.second + 0x9e3779b97f4a7c15ULL +
                           (value.first << 6U) + (value.first >> 2U)) ^
            value.size);
    }
};

template <typename Consumer>
void for_each_closed(const std::int32_t* neighbors, std::uint32_t degree,
                     std::uint32_t vertex, Consumer&& consume) {
    bool emitted_self = false;
    for (std::uint32_t i = 0; i < degree; ++i) {
        const auto neighbor = static_cast<std::uint32_t>(neighbors[i]);
        if (!emitted_self && vertex < neighbor) {
            consume(vertex);
            emitted_self = true;
        }
        consume(neighbor);
    }
    if (!emitted_self) { consume(vertex); }
}

Signature closed_signature(const MaterializedGraph& graph,
                           const std::int32_t* adjacency,
                           std::uint32_t vertex) {
    Signature result{1469598103934665603ULL, 0x9e3779b97f4a7c15ULL,
                     static_cast<std::uint64_t>(graph.degrees[vertex]) + 1};
    for_each_closed(adjacency + graph.offsets[vertex], graph.degrees[vertex],
                    vertex, [&](std::uint32_t value) {
        result.first ^= static_cast<std::uint64_t>(value) + 1;
        result.first *= 1099511628211ULL;
        result.second ^= static_cast<std::uint64_t>(value) +
                         0x9e3779b97f4a7c15ULL + (result.second << 6U) +
                         (result.second >> 2U);
    });
    return result;
}

Signature neighborhood_signature(const std::vector<VertexId>& neighborhood) {
    Signature result{1469598103934665603ULL, 0x9e3779b97f4a7c15ULL,
                     neighborhood.size()};
    for (const auto value : neighborhood) {
        result.first ^= static_cast<std::uint64_t>(value) + 1;
        result.first *= 1099511628211ULL;
        result.second ^= static_cast<std::uint64_t>(value) +
                         0x9e3779b97f4a7c15ULL + (result.second << 6U) +
                         (result.second >> 2U);
    }
    return result;
}

bool closed_equal(const MaterializedGraph& graph, const std::int32_t* adjacency,
                  std::uint32_t left, std::uint32_t right) {
    if (graph.degrees[left] != graph.degrees[right]) { return false; }
    std::vector<std::uint32_t> values;
    values.reserve(static_cast<std::size_t>(graph.degrees[left]) + 1);
    for_each_closed(adjacency + graph.offsets[left], graph.degrees[left], left,
                    [&](std::uint32_t value) { values.push_back(value); });
    std::size_t position = 0;
    bool equal = true;
    for_each_closed(adjacency + graph.offsets[right], graph.degrees[right], right,
                    [&](std::uint32_t value) {
        equal = equal && position < values.size() &&
                values[position] == value;
        ++position;
    });
    return equal && position == values.size();
}

std::optional<std::uint32_t> thresholded_closed_intersection(
    const std::int32_t* left, std::uint32_t left_size,
    const std::int32_t* right, std::uint32_t right_size,
    std::uint64_t required) {
    std::uint32_t common = 2;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    while (i < left_size && j < right_size) {
        if (left[i] < right[j]) {
            ++i;
        } else if (right[j] < left[i]) {
            ++j;
        } else {
            ++common;
            ++i;
            ++j;
        }
        if (static_cast<std::uint64_t>(common) +
                std::min(left_size - i, right_size - j) < required) {
            return std::nullopt;
        }
    }
    if (common < required) { return std::nullopt; }
    return common;
}

std::optional<std::uint32_t> thresholded_intersection(
    const std::vector<VertexId>& left,
    const std::vector<VertexId>& right,
    std::uint64_t required) {
    std::uint32_t common = 0;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < left.size() && j < right.size()) {
        if (left[i] < right[j]) {
            ++i;
        } else if (right[j] < left[i]) {
            ++j;
        } else {
            ++common;
            ++i;
            ++j;
        }
        if (static_cast<std::uint64_t>(common) +
                std::min(left.size() - i, right.size() - j) < required) {
            return std::nullopt;
        }
    }
    if (common < required) { return std::nullopt; }
    return common;
}

Wide square(std::uint64_t value) {
    return static_cast<Wide>(value) * value;
}

bool threshold_at_least(const SimilarityThreshold& left,
                        const SimilarityThreshold& right) {
    return static_cast<Wide>(left.numerator) * right.denominator >=
           static_cast<Wide>(right.numerator) * left.denominator;
}

class DisjointSet {
public:
    explicit DisjointSet(std::size_t size) : parent_(size), rank_(size, 0) {
        std::iota(parent_.begin(), parent_.end(), 0);
    }
    VertexId find(VertexId value) {
        if (parent_[value] != value) { parent_[value] = find(parent_[value]); }
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

void sort_certificates(
    const std::vector<std::uint32_t>& degrees,
    std::vector<SelectiveEquivalenceCertificate>* certificates) {
    auto score_greater = [&](const auto& left, const auto& right) {
        const Wide left_score = square(left.common_neighbors) *
            degrees[right.left_class] * degrees[right.right_class];
        const Wide right_score = square(right.common_neighbors) *
            degrees[left.left_class] * degrees[left.right_class];
        if (left_score != right_score) { return left_score > right_score; }
        if (left.left_class != right.left_class) {
            return left.left_class < right.left_class;
        }
        return left.right_class < right.right_class;
    };
    std::sort(certificates->begin(), certificates->end(), score_greater);
}

}  // namespace

SelectiveEquivalenceIndex SelectiveEquivalenceIndex::build(
    const FactorIndex& factor,
    const SimilarityThreshold& coverage_floor) {
    if (coverage_floor.numerator == 0 ||
        coverage_floor.numerator > coverage_floor.denominator) {
        throw std::invalid_argument("SECI coverage floor must be in (0,1]");
    }
    if (factor.vertex_count() > std::numeric_limits<VertexId>::max()) {
        throw std::overflow_error("SECI factor graph exceeds 32-bit ids");
    }
    const auto begin = Clock::now();
    SelectiveEquivalenceIndex result;
    result.coverage_floor_ = coverage_floor;
    result.loaded_floor_ = coverage_floor;
    result.stats_.vertices = factor.vertex_count();
    result.stats_.projected_edges = factor.stats().exact_projected_edges;

    std::vector<VertexId> class_of(
        static_cast<std::size_t>(factor.vertex_count()));
    std::vector<std::vector<VertexId>> representatives;
    std::vector<std::vector<VertexId>> class_members;
    std::unordered_map<Signature, std::vector<VertexId>, SignatureHash> buckets;
    buckets.reserve(static_cast<std::size_t>(factor.vertex_count() / 2));
    for (std::uint64_t vertex64 = 0; vertex64 < factor.vertex_count(); ++vertex64) {
        const auto vertex = static_cast<VertexId>(vertex64);
        auto neighborhood = factor.collect_closed_neighborhood(vertex);
        const auto key = neighborhood_signature(neighborhood);
        auto& candidates = buckets[key];
        VertexId class_id = std::numeric_limits<VertexId>::max();
        for (const auto candidate : candidates) {
            if (representatives[candidate] == neighborhood) {
                class_id = candidate;
                break;
            }
        }
        if (class_id == std::numeric_limits<VertexId>::max()) {
            class_id = static_cast<VertexId>(representatives.size());
            representatives.push_back(std::move(neighborhood));
            candidates.push_back(class_id);
            class_members.emplace_back();
            result.degrees_.push_back(
                static_cast<std::uint32_t>(representatives.back().size()));
        }
        class_of[vertex] = class_id;
        class_members[class_id].push_back(vertex);
    }
    result.stats_.classes = representatives.size();
    result.member_offsets_.resize(class_members.size() + 1, 0);
    result.members_flat_.reserve(static_cast<std::size_t>(factor.vertex_count()));
    for (std::size_t class_id = 0; class_id < class_members.size(); ++class_id) {
        result.members_flat_.insert(result.members_flat_.end(),
                                    class_members[class_id].begin(),
                                    class_members[class_id].end());
        result.member_offsets_[class_id + 1] = result.members_flat_.size();
    }

    std::vector<VertexId> neighbor_classes;
    for (VertexId left_class = 0; left_class < representatives.size();
         ++left_class) {
        const auto& left_neighborhood = representatives[left_class];
        neighbor_classes.clear();
        neighbor_classes.reserve(left_neighborhood.size());
        for (const auto vertex : left_neighborhood) {
            neighbor_classes.push_back(class_of[vertex]);
        }
        std::sort(neighbor_classes.begin(), neighbor_classes.end());
        neighbor_classes.erase(
            std::unique(neighbor_classes.begin(), neighbor_classes.end()),
            neighbor_classes.end());
        for (const auto right_class : neighbor_classes) {
            if (right_class <= left_class) { continue; }
            ++result.stats_.quotient_edges;
            const auto left_degree = result.degrees_[left_class];
            const auto right_degree = result.degrees_[right_class];
            if (coverage_floor.fails_degree_ratio(left_degree, right_degree)) {
                continue;
            }
            ++result.stats_.degree_candidate_edges;
            const auto required = coverage_floor.required_common_neighbors(
                left_degree, right_degree);
            const auto common = thresholded_intersection(
                left_neighborhood, representatives[right_class], required);
            if (common) {
                result.certificates_.push_back(
                    {left_class, right_class, *common});
            }
        }
    }
    sort_certificates(result.degrees_, &result.certificates_);
    result.stats_.retained_certificates = result.certificates_.size();
    result.stats_.build_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin)
            .count());
    return result;
}

SelectiveEquivalenceIndex SelectiveEquivalenceIndex::build_from_materialized_graph(
    const std::filesystem::path& graph_directory,
    const SimilarityThreshold& coverage_floor) {
    if (coverage_floor.numerator == 0 ||
        coverage_floor.numerator > coverage_floor.denominator) {
        throw std::invalid_argument("SECI coverage floor must be in (0,1]");
    }
    const auto begin = Clock::now();
    const auto graph = read_graph_header(graph_directory);
    ReadOnlyAdjacency adjacency(graph_directory / "b_adj.bin",
                                graph.directed_edges);
    SelectiveEquivalenceIndex result;
    result.coverage_floor_ = coverage_floor;
    result.loaded_floor_ = coverage_floor;
    result.stats_.vertices = graph.vertices;
    result.stats_.projected_edges = graph.directed_edges / 2;

    std::vector<VertexId> class_of(graph.vertices);
    std::vector<VertexId> representatives;
    std::vector<std::vector<VertexId>> class_members;
    std::unordered_map<Signature, std::vector<VertexId>, SignatureHash> buckets;
    buckets.reserve(graph.vertices / 2);
    for (VertexId vertex = 0; vertex < graph.vertices; ++vertex) {
        const auto key = closed_signature(graph, adjacency.data(), vertex);
        auto& candidates = buckets[key];
        VertexId class_id = std::numeric_limits<VertexId>::max();
        for (const auto candidate : candidates) {
            if (closed_equal(graph, adjacency.data(), vertex,
                             representatives[candidate])) {
                class_id = candidate;
                break;
            }
        }
        if (class_id == std::numeric_limits<VertexId>::max()) {
            class_id = static_cast<VertexId>(representatives.size());
            representatives.push_back(vertex);
            candidates.push_back(class_id);
            class_members.emplace_back();
            result.degrees_.push_back(graph.degrees[vertex] + 1);
        }
        class_of[vertex] = class_id;
        class_members[class_id].push_back(vertex);
    }
    result.stats_.classes = representatives.size();
    result.member_offsets_.resize(class_members.size() + 1, 0);
    result.members_flat_.reserve(graph.vertices);
    for (std::size_t class_id = 0; class_id < class_members.size(); ++class_id) {
        result.members_flat_.insert(result.members_flat_.end(),
                                    class_members[class_id].begin(),
                                    class_members[class_id].end());
        result.member_offsets_[class_id + 1] = result.members_flat_.size();
    }
    std::cerr << "SECI phase=equivalence classes=" << result.stats_.classes
              << '\n';

    std::vector<VertexId> neighbor_classes;
    for (VertexId left_class = 0; left_class < representatives.size();
         ++left_class) {
        const auto left = representatives[left_class];
        neighbor_classes.clear();
        neighbor_classes.reserve(static_cast<std::size_t>(graph.degrees[left]) + 1);
        neighbor_classes.push_back(left_class);
        const auto* left_neighbors = adjacency.data() + graph.offsets[left];
        for (std::uint32_t i = 0; i < graph.degrees[left]; ++i) {
            neighbor_classes.push_back(
                class_of[static_cast<VertexId>(left_neighbors[i])]);
        }
        std::sort(neighbor_classes.begin(), neighbor_classes.end());
        neighbor_classes.erase(
            std::unique(neighbor_classes.begin(), neighbor_classes.end()),
            neighbor_classes.end());
        for (const auto right_class : neighbor_classes) {
            if (right_class <= left_class) { continue; }
            ++result.stats_.quotient_edges;
            const auto right = representatives[right_class];
            const auto left_degree = result.degrees_[left_class];
            const auto right_degree = result.degrees_[right_class];
            if (coverage_floor.fails_degree_ratio(left_degree, right_degree)) {
                continue;
            }
            ++result.stats_.degree_candidate_edges;
            const auto required = coverage_floor.required_common_neighbors(
                left_degree, right_degree);
            const auto common = thresholded_closed_intersection(
                left_neighbors, graph.degrees[left],
                adjacency.data() + graph.offsets[right], graph.degrees[right],
                required);
            if (common) {
                result.certificates_.push_back(
                    {left_class, right_class, *common});
            }
        }
        if (left_class != 0 && left_class % 20000 == 0) {
            std::cerr << "SECI phase=quotient classes_done=" << left_class
                      << " retained=" << result.certificates_.size() << '\n';
        }
    }
    sort_certificates(result.degrees_, &result.certificates_);
    result.stats_.retained_certificates = result.certificates_.size();
    result.stats_.build_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin)
            .count());
    return result;
}

void SelectiveEquivalenceIndex::save(const std::filesystem::path& file) const {
    if (certificates_.size() != stats_.retained_certificates) {
        throw std::logic_error(
            "cannot save a threshold-loaded partial SECI index");
    }
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path());
    }
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output) { throw std::runtime_error("cannot create SECI index"); }
    constexpr char magic[8] = {'S', 'E', 'C', 'I', 'I', 'D', 'X', '\0'};
    output.write(magic, sizeof(magic));
    write_stream_value(output, static_cast<std::uint32_t>(2));
    write_stream_value(output, static_cast<std::uint32_t>(0x01020304U));
    write_stream_value(output, coverage_floor_.numerator);
    write_stream_value(output, coverage_floor_.denominator);
    write_stream_value(output, stats_.vertices);
    write_stream_value(output, stats_.classes);
    write_stream_value(output, stats_.projected_edges);
    write_stream_value(output, stats_.quotient_edges);
    write_stream_value(output, stats_.degree_candidate_edges);
    write_stream_value(output, stats_.retained_certificates);
    write_stream_value(output, stats_.build_milliseconds);
    output.write(reinterpret_cast<const char*>(degrees_.data()),
                 static_cast<std::streamsize>(degrees_.size() *
                                              sizeof(std::uint32_t)));
    for (std::size_t class_id = 0; class_id < degrees_.size(); ++class_id) {
        const auto count = static_cast<std::uint32_t>(
            member_offsets_[class_id + 1] - member_offsets_[class_id]);
        write_stream_value(output, count);
    }
    output.write(reinterpret_cast<const char*>(members_flat_.data()),
                 static_cast<std::streamsize>(members_flat_.size() *
                                              sizeof(VertexId)));
    static_assert(sizeof(SelectiveEquivalenceCertificate) == 12,
                  "SECI certificate must be three packed 32-bit integers");
    output.write(reinterpret_cast<const char*>(certificates_.data()),
                 static_cast<std::streamsize>(certificates_.size() *
                     sizeof(SelectiveEquivalenceCertificate)));
    if (!output) {
        throw std::runtime_error("failed to write SECI payload");
    }
}

SelectiveEquivalenceIndex SelectiveEquivalenceIndex::load(
    const std::filesystem::path& file) {
    return load_impl(file, nullptr);
}

SelectiveEquivalenceIndex SelectiveEquivalenceIndex::load(
    const std::filesystem::path& file,
    const SimilarityThreshold& query_threshold) {
    return load_impl(file, &query_threshold);
}

SelectiveEquivalenceIndex SelectiveEquivalenceIndex::load_impl(
    const std::filesystem::path& file,
    const SimilarityThreshold* query_threshold) {
    std::ifstream input(file, std::ios::binary);
    if (!input) { throw std::runtime_error("cannot open SECI index"); }
    constexpr char expected[8] = {'S', 'E', 'C', 'I', 'I', 'D', 'X', '\0'};
    char magic[8]{};
    input.read(magic, sizeof(magic));
    const auto version = read_stream_value<std::uint32_t>(input, "version");
    const auto endian = read_stream_value<std::uint32_t>(input, "endian");
    if (std::memcmp(magic, expected, sizeof(magic)) != 0 ||
        (version != 1 && version != 2) ||
        endian != 0x01020304U) {
        throw std::runtime_error("unsupported SECI index format");
    }
    SelectiveEquivalenceIndex result;
    result.coverage_floor_.numerator =
        read_stream_value<std::uint64_t>(input, "coverage numerator");
    result.coverage_floor_.denominator =
        read_stream_value<std::uint64_t>(input, "coverage denominator");
    result.loaded_floor_ = result.coverage_floor_;
    result.stats_.vertices = read_stream_value<std::uint64_t>(input, "vertices");
    result.stats_.classes = read_stream_value<std::uint64_t>(input, "classes");
    result.stats_.projected_edges =
        read_stream_value<std::uint64_t>(input, "projected edges");
    result.stats_.quotient_edges =
        read_stream_value<std::uint64_t>(input, "quotient edges");
    result.stats_.degree_candidate_edges =
        read_stream_value<std::uint64_t>(input, "candidate edges");
    result.stats_.retained_certificates =
        read_stream_value<std::uint64_t>(input, "retained certificates");
    result.stats_.build_milliseconds =
        read_stream_value<std::uint64_t>(input, "build time");
    if (result.stats_.vertices > std::numeric_limits<VertexId>::max() ||
        result.stats_.classes > std::numeric_limits<VertexId>::max()) {
        throw std::runtime_error("SECI index exceeds 32-bit ids");
    }
    const auto class_count = static_cast<std::size_t>(result.stats_.classes);
    result.degrees_.resize(class_count);
    result.member_offsets_.resize(class_count + 1, 0);
    result.members_flat_.resize(static_cast<std::size_t>(result.stats_.vertices));
    std::uint64_t member_total = 0;
    if (version == 1) {
        for (std::size_t class_id = 0; class_id < class_count; ++class_id) {
            result.degrees_[class_id] =
                read_stream_value<std::uint32_t>(input, "class degree");
            const auto count =
                read_stream_value<std::uint32_t>(input, "class size");
            if (count > result.stats_.vertices - member_total) {
                throw std::runtime_error("invalid SECI class size");
            }
            result.member_offsets_[class_id] = member_total;
            input.read(reinterpret_cast<char*>(result.members_flat_.data() +
                                               member_total),
                       static_cast<std::streamsize>(count * sizeof(VertexId)));
            if (!input) { throw std::runtime_error("truncated SECI members"); }
            member_total += count;
        }
    } else {
        input.read(reinterpret_cast<char*>(result.degrees_.data()),
                   static_cast<std::streamsize>(result.degrees_.size() *
                                                sizeof(std::uint32_t)));
        if (!input) { throw std::runtime_error("truncated SECI degrees"); }
        for (std::size_t class_id = 0; class_id < class_count; ++class_id) {
            const auto count =
                read_stream_value<std::uint32_t>(input, "class size");
            if (count > result.stats_.vertices - member_total) {
                throw std::runtime_error("invalid SECI class size");
            }
            result.member_offsets_[class_id] = member_total;
            member_total += count;
        }
        input.read(reinterpret_cast<char*>(result.members_flat_.data()),
                   static_cast<std::streamsize>(result.members_flat_.size() *
                                                sizeof(VertexId)));
        if (!input) { throw std::runtime_error("truncated SECI members"); }
    }
    result.member_offsets_[class_count] = member_total;
    if (member_total != result.stats_.vertices) {
        throw std::runtime_error("SECI classes do not cover all vertices");
    }
    for (const auto member : result.members_flat_) {
        if (member >= result.stats_.vertices) {
            throw std::runtime_error("SECI member is out of range");
        }
    }
    static_assert(sizeof(SelectiveEquivalenceCertificate) == 12,
                  "SECI certificate must be three packed 32-bit integers");
    const auto certificate_start_position = input.tellg();
    const auto certificate_start =
        static_cast<std::streamoff>(certificate_start_position);
    if (certificate_start < 0) {
        throw std::runtime_error("cannot locate SECI certificate payload");
    }
    const auto total_certificates = static_cast<std::size_t>(
        result.stats_.retained_certificates);
    const Wide expected_end = static_cast<std::uint64_t>(certificate_start) +
        static_cast<Wide>(total_certificates) *
            sizeof(SelectiveEquivalenceCertificate);
    if (expected_end > std::filesystem::file_size(file)) {
        throw std::runtime_error("truncated SECI certificates");
    }

    std::size_t certificates_to_load = total_certificates;
    if (query_threshold != nullptr) {
        if (!result.covers(*query_threshold)) {
            throw std::invalid_argument(
                "query epsilon is below the SECI coverage floor; use FLI fallback");
        }
        result.loaded_floor_ = *query_threshold;
        std::size_t first = 0;
        std::size_t last = total_certificates;
        while (first < last) {
            const auto middle = first + (last - first) / 2;
            const auto byte_offset =
                static_cast<std::uint64_t>(certificate_start) +
                static_cast<std::uint64_t>(middle) *
                    sizeof(SelectiveEquivalenceCertificate);
            input.clear();
            input.seekg(static_cast<std::streamoff>(byte_offset),
                        std::ios::beg);
            SelectiveEquivalenceCertificate certificate;
            input.read(reinterpret_cast<char*>(&certificate),
                       sizeof(certificate));
            if (!input) {
                throw std::runtime_error(
                    "cannot probe SECI certificate prefix");
            }
            if (certificate.left_class >= result.stats_.classes ||
                certificate.right_class >= result.stats_.classes ||
                certificate.left_class >= certificate.right_class) {
                throw std::runtime_error("invalid SECI certificate endpoint");
            }
            if (result.score_at_least(certificate, *query_threshold)) {
                first = middle + 1;
            } else {
                last = middle;
            }
        }
        certificates_to_load = first;
    }
    result.certificates_.resize(certificates_to_load);
    if (!result.certificates_.empty()) {
        input.clear();
        input.seekg(certificate_start, std::ios::beg);
        input.read(reinterpret_cast<char*>(result.certificates_.data()),
                   static_cast<std::streamsize>(result.certificates_.size() *
                       sizeof(SelectiveEquivalenceCertificate)));
        if (!input) {
            throw std::runtime_error("truncated SECI certificate prefix");
        }
    }
    for (const auto& certificate : result.certificates_) {
        if (certificate.left_class >= result.stats_.classes ||
            certificate.right_class >= result.stats_.classes ||
            certificate.left_class >= certificate.right_class) {
            throw std::runtime_error("invalid SECI certificate endpoint");
        }
    }
    return result;
}

const SimilarityThreshold& SelectiveEquivalenceIndex::coverage_floor() const
    noexcept { return coverage_floor_; }

const SelectiveEquivalenceIndexStats& SelectiveEquivalenceIndex::stats() const
    noexcept { return stats_; }

VertexMemberRange SelectiveEquivalenceIndex::members(VertexId class_id) const {
    if (class_id >= stats_.classes) {
        throw std::out_of_range("SECI class id is out of range");
    }
    const auto begin = member_offsets_[class_id];
    const auto end = member_offsets_[static_cast<std::size_t>(class_id) + 1];
    return VertexMemberRange(members_flat_.data() + begin,
                             static_cast<std::size_t>(end - begin));
}

std::uint32_t SelectiveEquivalenceIndex::degree(VertexId class_id) const {
    return degrees_.at(class_id);
}

const std::vector<SelectiveEquivalenceCertificate>&
SelectiveEquivalenceIndex::certificates() const noexcept { return certificates_; }

bool SelectiveEquivalenceIndex::covers(
    const SimilarityThreshold& threshold) const noexcept {
    return threshold_at_least(threshold, coverage_floor_);
}

SelectiveEquivalenceIndex SelectiveEquivalenceIndex::derive_coverage_layer(
    const SimilarityThreshold& higher_floor) const {
    if (!threshold_at_least(higher_floor, coverage_floor_) ||
        higher_floor.numerator > higher_floor.denominator) {
        throw std::invalid_argument(
            "derived SECI floor must be within [base floor, 1]");
    }
    SelectiveEquivalenceIndex result;
    result.coverage_floor_ = higher_floor;
    result.loaded_floor_ = higher_floor;
    result.stats_ = stats_;
    result.degrees_ = degrees_;
    result.members_flat_ = members_flat_;
    result.member_offsets_ = member_offsets_;
    const auto prefix = similar_prefix_size(higher_floor);
    result.certificates_.assign(certificates_.begin(),
                                certificates_.begin() + prefix);
    result.stats_.retained_certificates = prefix;
    return result;
}

std::uint64_t SelectiveEquivalenceIndex::serialized_bytes() const {
    constexpr std::uint64_t header_bytes =
        8 + 4 + 4 + 2 * 8 + 7 * 8;
    const Wide bytes = static_cast<Wide>(header_bytes) +
        static_cast<Wide>(stats_.classes) * 2 * sizeof(std::uint32_t) +
        static_cast<Wide>(stats_.vertices) * sizeof(VertexId) +
        static_cast<Wide>(stats_.retained_certificates) *
            (2 * sizeof(VertexId) + sizeof(std::uint32_t));
    if (bytes > std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("SECI serialized size exceeds 64 bits");
    }
    return static_cast<std::uint64_t>(bytes);
}

bool SelectiveEquivalenceIndex::score_at_least(
    const SelectiveEquivalenceCertificate& certificate,
    const SimilarityThreshold& threshold) const {
    const Wide left = square(certificate.common_neighbors) *
                      square(threshold.denominator);
    const Wide right = static_cast<Wide>(degrees_[certificate.left_class]) *
                       degrees_[certificate.right_class] *
                       square(threshold.numerator);
    return left >= right;
}

std::size_t SelectiveEquivalenceIndex::similar_prefix_size(
    const SimilarityThreshold& threshold) const {
    if (!threshold_at_least(threshold, loaded_floor_)) {
        throw std::invalid_argument(
            "query epsilon is below the loaded SECI certificate prefix");
    }
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

SelectiveEquivalenceQueryResult run_pscan_on_selective_equivalence_index(
    const SelectiveEquivalenceIndex& index,
    const SimilarityThreshold& threshold,
    std::uint64_t mu) {
    if (mu == 0) { throw std::invalid_argument("mu must be positive"); }
    const auto begin = Clock::now();
    SelectiveEquivalenceQueryResult output;
    const auto prefix = index.similar_prefix_size(threshold);
    output.similar_class_edges = prefix;
    output.certificates_skipped =
        index.stats().retained_certificates - prefix;
    const auto class_count = static_cast<std::size_t>(index.stats().classes);
    std::vector<std::uint64_t> similar_degree(class_count, 0);
    for (VertexId class_id = 0; class_id < class_count; ++class_id) {
        similar_degree[class_id] = index.members(class_id).size() - 1;
    }
    for (std::size_t i = 0; i < prefix; ++i) {
        const auto& edge = index.certificates()[i];
        similar_degree[edge.left_class] += index.members(edge.right_class).size();
        similar_degree[edge.right_class] += index.members(edge.left_class).size();
    }
    std::vector<bool> core(class_count, false);
    for (std::size_t class_id = 0; class_id < class_count; ++class_id) {
        core[class_id] = similar_degree[class_id] >= mu;
    }
    DisjointSet components(class_count);
    for (std::size_t i = 0; i < prefix; ++i) {
        const auto& edge = index.certificates()[i];
        if (core[edge.left_class] && core[edge.right_class]) {
            components.unite(edge.left_class, edge.right_class);
        }
    }
    std::vector<VertexId> cluster_id(
        class_count, std::numeric_limits<VertexId>::max());
    for (VertexId class_id = 0; class_id < class_count; ++class_id) {
        if (!core[class_id]) { continue; }
        const auto root = components.find(class_id);
        cluster_id[root] = std::min(cluster_id[root],
                                    index.members(class_id).front());
    }
    std::vector<std::vector<VertexId>> noncore_clusters(class_count);
    for (std::size_t i = 0; i < prefix; ++i) {
        const auto& edge = index.certificates()[i];
        if (core[edge.left_class] && !core[edge.right_class]) {
            noncore_clusters[edge.right_class].push_back(
                cluster_id[components.find(edge.left_class)]);
        } else if (!core[edge.left_class] && core[edge.right_class]) {
            noncore_clusters[edge.left_class].push_back(
                cluster_id[components.find(edge.right_class)]);
        }
    }
    auto& result = output.clustering;
    result.is_core.resize(index.stats().vertices, false);
    result.core_cluster.resize(index.stats().vertices,
                               std::numeric_limits<VertexId>::max());
    result.noncore_clusters.resize(index.stats().vertices);
    result.roles.resize(index.stats().vertices, PscanVertexRole::Outlier);
    for (VertexId class_id = 0; class_id < class_count; ++class_id) {
        if (core[class_id]) {
            const auto root = components.find(class_id);
            const auto cluster = cluster_id[root];
            for (const auto member : index.members(class_id)) {
                result.is_core[member] = true;
                result.core_cluster[member] = cluster;
                result.roles[member] = PscanVertexRole::Core;
                ++result.stats.core_vertices;
            }
            if (index.members(class_id).front() == cluster) {
                ++result.stats.clusters;
            }
            continue;
        }
        auto& attachments = noncore_clusters[class_id];
        std::sort(attachments.begin(), attachments.end());
        attachments.erase(std::unique(attachments.begin(), attachments.end()),
                          attachments.end());
        for (const auto member : index.members(class_id)) {
            result.noncore_clusters[member] = attachments;
            if (attachments.empty()) {
                result.roles[member] = PscanVertexRole::Outlier;
                ++result.stats.outlier_vertices;
            } else if (attachments.size() == 1) {
                result.roles[member] = PscanVertexRole::Border;
                ++result.stats.border_vertices;
            } else {
                result.roles[member] = PscanVertexRole::Hub;
                ++result.stats.hub_vertices;
            }
        }
    }
    result.stats.projected_edges_seen_in_prune = index.stats().projected_edges;
    result.stats.exact_similarity_checks = prefix;
    result.stats.query_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin)
            .count());
    return output;
}

}  // namespace hinscan
