#include "hin/HinGraph.h"
#include "index/FactorIndex.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct ReferenceGraph {
    std::vector<std::vector<hinscan::VertexId>> adjacency;
};

std::int32_t read_int32(std::ifstream& input, const std::string& description) {
    std::int32_t value = 0;
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) {
        throw std::runtime_error("cannot read " + description);
    }
    return value;
}

ReferenceGraph read_pscan_graph(const std::filesystem::path& directory) {
    std::ifstream degree_input(directory / "b_degree.bin", std::ios::binary);
    std::ifstream adjacency_input(directory / "b_adj.bin", std::ios::binary);
    if (!degree_input || !adjacency_input) {
        throw std::runtime_error("reference pSCAN graph is incomplete");
    }

    const auto int_size = read_int32(degree_input, "integer size");
    const auto vertex_count = read_int32(degree_input, "vertex count");
    const auto directed_edges = read_int32(degree_input, "edge count");
    if (int_size != static_cast<std::int32_t>(sizeof(std::int32_t)) ||
        vertex_count < 0 || directed_edges < 0) {
        throw std::runtime_error("invalid pSCAN degree header");
    }

    std::vector<std::int32_t> degrees(static_cast<std::size_t>(vertex_count));
    std::uint64_t degree_sum = 0;
    for (auto& degree : degrees) {
        degree = read_int32(degree_input, "degree");
        if (degree < 0) {
            throw std::runtime_error("negative degree in pSCAN graph");
        }
        degree_sum += static_cast<std::uint64_t>(degree);
    }
    if (degree_sum != static_cast<std::uint64_t>(directed_edges)) {
        throw std::runtime_error("pSCAN degree sum disagrees with edge count");
    }

    ReferenceGraph graph;
    graph.adjacency.resize(static_cast<std::size_t>(vertex_count));
    for (std::size_t vertex = 0; vertex < graph.adjacency.size(); ++vertex) {
        auto& neighbors = graph.adjacency[vertex];
        neighbors.reserve(static_cast<std::size_t>(degrees[vertex]));
        for (std::int32_t i = 0; i < degrees[vertex]; ++i) {
            const auto neighbor = read_int32(adjacency_input, "adjacency entry");
            if (neighbor < 0 || neighbor >= vertex_count ||
                neighbor == static_cast<std::int32_t>(vertex)) {
                throw std::runtime_error("invalid pSCAN adjacency entry");
            }
            neighbors.push_back(static_cast<hinscan::VertexId>(neighbor));
        }
        if (!std::is_sorted(neighbors.begin(), neighbors.end()) ||
            std::adjacent_find(neighbors.begin(), neighbors.end()) !=
                neighbors.end()) {
            throw std::runtime_error("pSCAN adjacency is not strictly sorted");
        }
    }
    return graph;
}

std::uint64_t reference_common_neighbors(
    hinscan::VertexId left,
    hinscan::VertexId right,
    const ReferenceGraph& graph) {
    std::vector<hinscan::VertexId> left_closed = graph.adjacency[left];
    std::vector<hinscan::VertexId> right_closed = graph.adjacency[right];
    left_closed.insert(
        std::lower_bound(left_closed.begin(), left_closed.end(), left), left);
    right_closed.insert(
        std::lower_bound(right_closed.begin(), right_closed.end(), right), right);

    std::uint64_t intersection = 0;
    auto left_iterator = left_closed.begin();
    auto right_iterator = right_closed.begin();
    while (left_iterator != left_closed.end() &&
           right_iterator != right_closed.end()) {
        if (*left_iterator == *right_iterator) {
            ++intersection;
            ++left_iterator;
            ++right_iterator;
        } else if (*left_iterator < *right_iterator) {
            ++left_iterator;
        } else {
            ++right_iterator;
        }
    }
    return intersection;
}

void print_usage(const char* executable) {
    std::cerr << "Usage: " << executable
              << " <hin-dataset-directory> <meta-path> "
                 "<materialized-pscan-directory> [epsilon]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        print_usage(argv[0]);
        return 2;
    }

    try {
        const auto graph = hinscan::HinGraph::load(argv[1]);
        const auto meta_path = hinscan::parse_meta_path(graph, argv[2]);
        const auto index = hinscan::FactorIndex::build(graph, meta_path);
        const auto reference = read_pscan_graph(argv[3]);
        if (index.vertex_count() != reference.adjacency.size()) {
            throw std::runtime_error("FLI and materialized graph vertex counts differ");
        }

        std::uint64_t missing_neighbors = 0;
        std::uint64_t extra_neighbors = 0;
        std::uint64_t degree_mismatches = 0;
        std::uint64_t similarity_mismatches = 0;
        std::uint64_t checked_edges = 0;
        std::uint64_t degree_pruned_edges = 0;
        std::uint64_t posting_entries_read = 0;
        const bool check_similarity = argc == 5;
        const auto threshold = check_similarity
                                   ? hinscan::SimilarityThreshold::parse(argv[4])
                                   : hinscan::SimilarityThreshold{};

        for (std::size_t vertex = 0; vertex < reference.adjacency.size(); ++vertex) {
            if (index.degree(static_cast<hinscan::VertexId>(vertex)) !=
                reference.adjacency[vertex].size() + 1) {
                ++degree_mismatches;
            }

            const auto factor_neighborhood = index.collect_closed_neighborhood(
                static_cast<hinscan::VertexId>(vertex));
            auto reference_neighborhood = reference.adjacency[vertex];
            reference_neighborhood.insert(
                std::lower_bound(reference_neighborhood.begin(),
                                 reference_neighborhood.end(),
                                 static_cast<hinscan::VertexId>(vertex)),
                static_cast<hinscan::VertexId>(vertex));
            std::vector<hinscan::VertexId> difference;
            std::set_difference(reference_neighborhood.begin(),
                                reference_neighborhood.end(),
                                factor_neighborhood.begin(),
                                factor_neighborhood.end(),
                                std::back_inserter(difference));
            missing_neighbors += difference.size();
            difference.clear();
            std::set_difference(factor_neighborhood.begin(),
                                factor_neighborhood.end(),
                                reference_neighborhood.begin(),
                                reference_neighborhood.end(),
                                std::back_inserter(difference));
            extra_neighbors += difference.size();

            if (!check_similarity) {
                continue;
            }
            for (const auto neighbor : reference.adjacency[vertex]) {
                if (neighbor <= vertex) {
                    continue;
                }
                ++checked_edges;
                const auto result = index.check_similarity(
                    static_cast<hinscan::VertexId>(vertex), neighbor, threshold);
                degree_pruned_edges += result.degree_pruned ? 1 : 0;
                posting_entries_read += result.posting_entries_read;
                const auto common = reference_common_neighbors(
                    static_cast<hinscan::VertexId>(vertex), neighbor, reference);
                const bool expected =
                    common >= result.required_common_neighbors;
                if (result.similar != expected ||
                    (!result.degree_pruned && result.common_neighbors != common)) {
                    ++similarity_mismatches;
                }
            }
        }

        const auto& stats = index.stats();
        std::cout << "target_vertices=" << stats.target_vertices << '\n'
                  << "center_vertices=" << stats.center_vertices << '\n'
                  << "half_path_incidences=" << stats.half_path_incidences
                  << '\n'
                  << "exact_projected_edges=" << stats.exact_projected_edges
                  << '\n'
                  << "estimated_index_bytes=" << stats.estimated_index_bytes
                  << '\n'
                  << "build_ms=" << stats.build_milliseconds << '\n'
                  << "degree_mismatches=" << degree_mismatches << '\n'
                  << "missing_neighbors=" << missing_neighbors << '\n'
                  << "extra_neighbors=" << extra_neighbors << '\n'
                  << "checked_edges=" << checked_edges << '\n'
                  << "degree_pruned_edges=" << degree_pruned_edges << '\n'
                  << "similarity_mismatches=" << similarity_mismatches << '\n'
                  << "query_posting_entries_read=" << posting_entries_read << '\n';

        return degree_mismatches == 0 && missing_neighbors == 0 &&
                       extra_neighbors == 0 && similarity_mismatches == 0
                   ? 0
                   : 1;
    } catch (const std::exception& error) {
        std::cerr << "fli_verify: " << error.what() << '\n';
        return 1;
    }
}
