#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

template <typename T>
T read_value(std::istream& input, const char* description) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) {
        throw std::runtime_error(std::string("cannot read ") + description);
    }
    return value;
}

std::uint32_t bits_required(std::uint64_t maximum) {
    std::uint32_t bits = 1;
    while ((maximum >>= 1U) != 0) {
        ++bits;
    }
    return bits;
}

struct GraphHeader {
    std::uint32_t vertices = 0;
    std::uint64_t directed_edges = 0;
    std::vector<std::uint32_t> degrees;
    std::vector<std::uint64_t> offsets;
    std::uint32_t maximum_open_degree = 0;
};

GraphHeader read_header(const std::filesystem::path& graph_directory) {
    std::ifstream input(graph_directory / "b_degree.bin", std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open b_degree.bin");
    }
    const auto integer_size = read_value<std::int32_t>(input, "integer size");
    const auto vertices_signed = read_value<std::int32_t>(input, "vertex count");
    const auto edges_signed = read_value<std::int32_t>(input, "directed edge count");
    if (integer_size != static_cast<std::int32_t>(sizeof(std::int32_t)) ||
        vertices_signed < 0 || edges_signed < 0) {
        throw std::runtime_error("invalid pSCAN degree header");
    }

    GraphHeader graph;
    graph.vertices = static_cast<std::uint32_t>(vertices_signed);
    graph.directed_edges = static_cast<std::uint64_t>(edges_signed);
    graph.degrees.resize(graph.vertices);
    graph.offsets.resize(static_cast<std::size_t>(graph.vertices) + 1, 0);
    std::uint64_t degree_sum = 0;
    for (std::uint32_t vertex = 0; vertex < graph.vertices; ++vertex) {
        const auto degree = read_value<std::int32_t>(input, "vertex degree");
        if (degree < 0) {
            throw std::runtime_error("negative vertex degree");
        }
        graph.degrees[vertex] = static_cast<std::uint32_t>(degree);
        graph.maximum_open_degree =
            std::max(graph.maximum_open_degree, graph.degrees[vertex]);
        degree_sum += graph.degrees[vertex];
        graph.offsets[static_cast<std::size_t>(vertex) + 1] = degree_sum;
    }
    if (degree_sum != graph.directed_edges || (degree_sum & 1U) != 0) {
        throw std::runtime_error("degree sum disagrees with directed edge count");
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
            throw std::runtime_error("cannot open b_adj.bin");
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size) || size.QuadPart < 0 ||
            static_cast<std::uint64_t>(size.QuadPart) != expected_bytes) {
            close();
            throw std::runtime_error("unexpected b_adj.bin size");
        }
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_ == nullptr) {
            close();
            throw std::runtime_error("cannot create adjacency file mapping");
        }
        data_ = static_cast<const std::int32_t*>(
            MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (data_ == nullptr) {
            close();
            throw std::runtime_error("cannot map adjacency file");
        }
#else
        std::ifstream input(file, std::ios::binary | std::ios::ate);
        if (!input || static_cast<std::uint64_t>(input.tellg()) != expected_bytes) {
            throw std::runtime_error("unexpected b_adj.bin size");
        }
        input.seekg(0);
        owned_.resize(static_cast<std::size_t>(expected_integers));
        input.read(reinterpret_cast<char*>(owned_.data()),
                   static_cast<std::streamsize>(expected_bytes));
        if (!input) {
            throw std::runtime_error("cannot read b_adj.bin");
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

std::uint32_t closed_intersection(const std::int32_t* left,
                                  std::uint32_t left_size,
                                  const std::int32_t* right,
                                  std::uint32_t right_size) {
    std::uint32_t common = 2;  // The edge endpoints occur in both closed sets.
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
    }
    return common;
}

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
    if (!emitted_self) {
        consume(vertex);
    }
}

Signature closed_signature(const std::int32_t* neighbors,
                           std::uint32_t degree, std::uint32_t vertex) {
    Signature result{1469598103934665603ULL, 0x9e3779b97f4a7c15ULL,
                     static_cast<std::uint64_t>(degree) + 1};
    for_each_closed(neighbors, degree, vertex, [&](std::uint32_t value) {
        result.first ^= static_cast<std::uint64_t>(value) + 1;
        result.first *= 1099511628211ULL;
        result.second ^= static_cast<std::uint64_t>(value) +
                         0x9e3779b97f4a7c15ULL + (result.second << 6U) +
                         (result.second >> 2U);
    });
    return result;
}

bool closed_equal(const GraphHeader& graph, const std::int32_t* adjacency,
                  std::uint32_t left, std::uint32_t right) {
    if (graph.degrees[left] != graph.degrees[right]) {
        return false;
    }
    const auto degree = graph.degrees[left];
    std::vector<std::uint32_t> left_values;
    left_values.reserve(static_cast<std::size_t>(degree) + 1);
    for_each_closed(adjacency + graph.offsets[left], degree, left,
                    [&](std::uint32_t value) { left_values.push_back(value); });
    std::size_t position = 0;
    bool equal = true;
    for_each_closed(adjacency + graph.offsets[right], degree, right,
                    [&](std::uint32_t value) {
                        if (position >= left_values.size() ||
                            left_values[position] != value) {
                            equal = false;
                        }
                        ++position;
                    });
    return equal && position == left_values.size();
}

double seconds(Clock::duration duration) {
    return std::chrono::duration<double>(duration).count();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <materialized-graph-directory> [sample-edges]"
                     " [--equivalence-0.9]\n";
        return 2;
    }
    try {
        const std::filesystem::path graph_directory = argv[1];
        std::uint64_t requested_samples = 200000;
        if (argc >= 3) {
            requested_samples = std::stoull(argv[2]);
            if (requested_samples == 0) {
                throw std::runtime_error("sample-edges must be positive");
            }
        }
        const bool run_equivalence =
            argc == 4 && std::string(argv[3]) == "--equivalence-0.9";
        if (argc == 4 && !run_equivalence) {
            throw std::runtime_error("unknown fourth argument");
        }

        const auto load_begin = Clock::now();
        const auto graph = read_header(graph_directory);
        ReadOnlyAdjacency adjacency(graph_directory / "b_adj.bin",
                                    graph.directed_edges);
        const auto load_end = Clock::now();
        const auto undirected_edges = graph.directed_edges / 2;
        const auto sample_target = std::min(requested_samples, undirected_edges);
        if (undirected_edges == 0) {
            throw std::runtime_error("graph has no edges");
        }

        std::uint64_t sampled = 0;
        std::uint64_t undirected_index = 0;
        std::uint64_t next_sample = 0;
        std::uint64_t checksum = 0;
        std::uint32_t sampled_maximum_common = 0;
        Clock::duration intersection_duration{};
        constexpr std::array<double, 6> thresholds{
            0.30, 0.50, 0.70, 0.80, 0.90, 0.95};
        std::array<std::uint64_t, thresholds.size()> degree_candidates{};
        std::array<std::uint64_t, thresholds.size()> similar_edges{};
        std::array<Clock::duration, thresholds.size()>
            candidate_intersection_duration{};
        const auto scan_begin = Clock::now();
        for (std::uint32_t left = 0; left < graph.vertices; ++left) {
            const auto left_offset = graph.offsets[left];
            const auto left_degree = graph.degrees[left];
            const auto* left_neighbors = adjacency.data() + left_offset;
            for (std::uint32_t position = 0; position < left_degree; ++position) {
                const auto right_signed = left_neighbors[position];
                if (right_signed < 0 ||
                    static_cast<std::uint32_t>(right_signed) >= graph.vertices) {
                    throw std::runtime_error("adjacency endpoint is out of range");
                }
                const auto right = static_cast<std::uint32_t>(right_signed);
                if (right <= left) {
                    continue;
                }
                if (sampled < sample_target && undirected_index == next_sample) {
                    const auto begin = Clock::now();
                    const auto common = closed_intersection(
                        left_neighbors, left_degree,
                        adjacency.data() + graph.offsets[right],
                        graph.degrees[right]);
                    const auto one_intersection_duration = Clock::now() - begin;
                    intersection_duration += one_intersection_duration;
                    checksum += static_cast<std::uint64_t>(common) *
                                (static_cast<std::uint64_t>(left) + right + 1);
                    sampled_maximum_common =
                        std::max(sampled_maximum_common, common);
                    const auto left_closed =
                        static_cast<std::uint64_t>(left_degree) + 1;
                    const auto right_closed =
                        static_cast<std::uint64_t>(graph.degrees[right]) + 1;
                    const double degree_upper_bound = std::sqrt(
                        static_cast<double>(std::min(left_closed, right_closed)) /
                        static_cast<double>(std::max(left_closed, right_closed)));
                    const double similarity = static_cast<double>(common) /
                        std::sqrt(static_cast<double>(left_closed) *
                                  static_cast<double>(right_closed));
                    for (std::size_t threshold_index = 0;
                         threshold_index < thresholds.size(); ++threshold_index) {
                        if (degree_upper_bound >= thresholds[threshold_index]) {
                            ++degree_candidates[threshold_index];
                            candidate_intersection_duration[threshold_index] +=
                                one_intersection_duration;
                        }
                        if (similarity >= thresholds[threshold_index]) {
                            ++similar_edges[threshold_index];
                        }
                    }
                    ++sampled;
                    if (sampled < sample_target) {
                        next_sample = static_cast<std::uint64_t>(
                            (static_cast<unsigned long long>(sampled) *
                             undirected_edges) /
                            sample_target);
                    }
                }
                ++undirected_index;
            }
        }
        const auto scan_end = Clock::now();
        if (undirected_index != undirected_edges || sampled != sample_target) {
            throw std::runtime_error("edge count or sampling count disagrees");
        }

        const double load_seconds = seconds(load_end - load_begin);
        const double scan_seconds = seconds(scan_end - scan_begin);
        const double intersection_seconds = seconds(intersection_duration);
        const double scan_only_seconds =
            std::max(0.0, scan_seconds - intersection_seconds);
        const double mean_intersection_us =
            intersection_seconds * 1.0e6 / static_cast<double>(sampled);
        const double estimated_build_seconds =
            scan_only_seconds + intersection_seconds *
                static_cast<double>(undirected_edges) /
                static_cast<double>(sampled);

        const auto id_bits = bits_required(graph.vertices - 1);
        const auto sampled_count_bits = bits_required(sampled_maximum_common);
        const auto safe_count_bits = bits_required(
            static_cast<std::uint64_t>(graph.maximum_open_degree) + 1);
        constexpr std::uint64_t header_bytes = 56;
        const auto payload_bytes = [&](std::uint32_t count_bits) {
            const long double payload_bits =
                static_cast<long double>(graph.vertices) * count_bits +
                static_cast<long double>(undirected_edges) *
                    (2 * id_bits + count_bits);
            return static_cast<std::uint64_t>((payload_bits + 7) / 8);
        };
        const auto estimated_file_lower =
            header_bytes + payload_bytes(sampled_count_bits);
        const auto estimated_file_upper =
            header_bytes + payload_bytes(safe_count_bits);

        const long double closed_entries =
            static_cast<long double>(graph.directed_edges) + graph.vertices;
        const long double current_builder_bytes =
            closed_entries * sizeof(std::uint32_t) +
            static_cast<long double>(undirected_edges) * 12.0L;

        std::cout << std::fixed << std::setprecision(3)
                  << "vertices=" << graph.vertices << '\n'
                  << "undirected_edges=" << undirected_edges << '\n'
                  << "maximum_open_degree=" << graph.maximum_open_degree << '\n'
                  << "mapped_load_seconds=" << load_seconds << '\n'
                  << "full_edge_scan_seconds=" << scan_seconds << '\n'
                  << "sampled_edges=" << sampled << '\n'
                  << "sampled_maximum_common=" << sampled_maximum_common << '\n'
                  << "mean_intersection_microseconds="
                  << mean_intersection_us << '\n'
                  << "estimated_full_certificate_seconds="
                  << estimated_build_seconds << '\n'
                  << "id_bits=" << id_bits << '\n'
                  << "sampled_count_bits=" << sampled_count_bits << '\n'
                  << "safe_count_bits=" << safe_count_bits << '\n'
                  << "estimated_sci_bytes_lower=" << estimated_file_lower << '\n'
                  << "estimated_sci_bytes_upper=" << estimated_file_upper << '\n'
                  << "current_builder_core_bytes="
                  << static_cast<std::uint64_t>(current_builder_bytes) << '\n'
                  << "checksum=" << checksum << '\n';
        for (std::size_t threshold_index = 0;
             threshold_index < thresholds.size(); ++threshold_index) {
            const double candidate_rate =
                static_cast<double>(degree_candidates[threshold_index]) /
                static_cast<double>(sampled);
            const double similar_rate =
                static_cast<double>(similar_edges[threshold_index]) /
                static_cast<double>(sampled);
            const auto estimated_survivors = static_cast<std::uint64_t>(
                similar_rate * static_cast<double>(undirected_edges));
            const auto estimated_selective_bytes = header_bytes +
                payload_bytes(safe_count_bits) -
                static_cast<std::uint64_t>(undirected_edges) *
                    (2 * id_bits + safe_count_bits) / 8 +
                (estimated_survivors *
                    static_cast<std::uint64_t>(2 * id_bits + safe_count_bits) + 7) /
                    8;
            const double estimated_selective_build_seconds =
                scan_only_seconds +
                seconds(candidate_intersection_duration[threshold_index]) *
                    static_cast<double>(undirected_edges) /
                    static_cast<double>(sampled);
            std::cout << "threshold=" << thresholds[threshold_index]
                      << " degree_candidate_rate=" << candidate_rate
                      << " similar_edge_rate=" << similar_rate
                      << " estimated_similar_edges=" << estimated_survivors
                      << " estimated_selective_sci_bytes="
                      << estimated_selective_bytes
                      << " estimated_selective_build_seconds="
                      << estimated_selective_build_seconds << '\n';
        }

        if (run_equivalence) {
            const auto equivalence_begin = Clock::now();
            std::vector<std::uint32_t> class_of(graph.vertices);
            std::vector<std::uint32_t> representatives;
            std::vector<std::uint32_t> class_sizes;
            std::unordered_map<Signature, std::vector<std::uint32_t>,
                               SignatureHash> buckets;
            buckets.reserve(graph.vertices / 2);
            for (std::uint32_t vertex = 0; vertex < graph.vertices; ++vertex) {
                const auto key = closed_signature(
                    adjacency.data() + graph.offsets[vertex],
                    graph.degrees[vertex], vertex);
                auto& candidates = buckets[key];
                std::uint32_t class_id = std::numeric_limits<std::uint32_t>::max();
                for (const auto candidate : candidates) {
                    if (closed_equal(graph, adjacency.data(), vertex,
                                     representatives[candidate])) {
                        class_id = candidate;
                        break;
                    }
                }
                if (class_id == std::numeric_limits<std::uint32_t>::max()) {
                    class_id = static_cast<std::uint32_t>(representatives.size());
                    representatives.push_back(vertex);
                    class_sizes.push_back(0);
                    candidates.push_back(class_id);
                }
                class_of[vertex] = class_id;
                ++class_sizes[class_id];
            }
            const auto equivalence_end = Clock::now();

            std::uint64_t quotient_edges = 0;
            std::uint64_t degree_candidate_class_edges = 0;
            std::uint64_t similar_class_edges = 0;
            std::uint64_t represented_internal_similar_edges = 0;
            std::uint64_t represented_cross_similar_edges = 0;
            Clock::duration quotient_intersection_duration{};
            for (const auto size : class_sizes) {
                represented_internal_similar_edges +=
                    static_cast<std::uint64_t>(size) * (size - 1) / 2;
            }
            const auto quotient_begin = Clock::now();
            std::vector<std::uint32_t> neighbor_classes;
            for (std::uint32_t left_class = 0;
                 left_class < representatives.size(); ++left_class) {
                const auto left = representatives[left_class];
                neighbor_classes.clear();
                neighbor_classes.reserve(
                    static_cast<std::size_t>(graph.degrees[left]) + 1);
                neighbor_classes.push_back(left_class);
                const auto* neighbors = adjacency.data() + graph.offsets[left];
                for (std::uint32_t i = 0; i < graph.degrees[left]; ++i) {
                    neighbor_classes.push_back(
                        class_of[static_cast<std::uint32_t>(neighbors[i])]);
                }
                std::sort(neighbor_classes.begin(), neighbor_classes.end());
                neighbor_classes.erase(
                    std::unique(neighbor_classes.begin(), neighbor_classes.end()),
                    neighbor_classes.end());
                const auto left_closed =
                    static_cast<std::uint64_t>(graph.degrees[left]) + 1;
                for (const auto right_class : neighbor_classes) {
                    if (right_class <= left_class) {
                        continue;
                    }
                    ++quotient_edges;
                    const auto right = representatives[right_class];
                    const auto right_closed =
                        static_cast<std::uint64_t>(graph.degrees[right]) + 1;
                    if (100 * std::min(left_closed, right_closed) <
                        81 * std::max(left_closed, right_closed)) {
                        continue;
                    }
                    ++degree_candidate_class_edges;
                    const auto intersection_begin = Clock::now();
                    const auto common = closed_intersection(
                        adjacency.data() + graph.offsets[left],
                        graph.degrees[left],
                        adjacency.data() + graph.offsets[right],
                        graph.degrees[right]);
                    quotient_intersection_duration +=
                        Clock::now() - intersection_begin;
                    if (100 * static_cast<std::uint64_t>(common) * common <
                        81 * left_closed * right_closed) {
                        continue;
                    }
                    ++similar_class_edges;
                    represented_cross_similar_edges +=
                        static_cast<std::uint64_t>(class_sizes[left_class]) *
                        class_sizes[right_class];
                }
            }
            const auto quotient_end = Clock::now();
            const auto class_bits = bits_required(representatives.size() - 1);
            const auto vertex_bits = bits_required(graph.vertices - 1);
            const auto class_size_bits = bits_required(
                *std::max_element(class_sizes.begin(), class_sizes.end()));
            const long double selective_quotient_bits =
                static_cast<long double>(graph.vertices) * class_bits +
                static_cast<long double>(representatives.size()) *
                    (vertex_bits + class_size_bits + safe_count_bits) +
                static_cast<long double>(similar_class_edges) *
                    (2 * class_bits + safe_count_bits);
            std::cout << "equivalence_build_seconds="
                      << seconds(equivalence_end - equivalence_begin) << '\n'
                      << "equivalence_classes=" << representatives.size() << '\n'
                      << "quotient_scan_seconds="
                      << seconds(quotient_end - quotient_begin) << '\n'
                      << "quotient_intersection_seconds="
                      << seconds(quotient_intersection_duration) << '\n'
                      << "quotient_edges=" << quotient_edges << '\n'
                      << "degree_candidate_class_edges_0.9="
                      << degree_candidate_class_edges << '\n'
                      << "similar_class_edges_0.9=" << similar_class_edges << '\n'
                      << "represented_internal_similar_edges="
                      << represented_internal_similar_edges << '\n'
                      << "represented_cross_similar_edges_0.9="
                      << represented_cross_similar_edges << '\n'
                      << "represented_total_similar_edges_0.9="
                      << represented_internal_similar_edges +
                             represented_cross_similar_edges
                      << '\n'
                      << "estimated_selective_quotient_bytes_0.9="
                      << static_cast<std::uint64_t>(
                             (selective_quotient_bits + 7) / 8 + 64)
                      << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sci_cost_probe: " << error.what() << '\n';
        return 1;
    }
}
