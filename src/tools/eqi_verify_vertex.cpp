#include "index/EquivalenceIndex.h"
#include "index/FactorIndex.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <factor.fli> <index.eqi> <vertex> <epsilon>\n";
        return 2;
    }
    try {
        const auto factor = hinscan::FactorIndex::load(argv[1]);
        const auto eq = hinscan::EquivalenceIndex::load(argv[2]);
        const auto vertex = static_cast<hinscan::VertexId>(std::stoul(argv[3]));
        const auto threshold = hinscan::SimilarityThreshold::parse(argv[4]);
        hinscan::VertexId class_id = std::numeric_limits<hinscan::VertexId>::max();
        for (hinscan::VertexId candidate = 0; candidate < eq.stats().classes;
             ++candidate) {
            const auto& members = eq.members(candidate);
            if (std::binary_search(members.begin(), members.end(), vertex)) {
                class_id = candidate;
                break;
            }
        }
        if (class_id == std::numeric_limits<hinscan::VertexId>::max()) {
            throw std::runtime_error("vertex has no equivalence class");
        }
        std::uint64_t factor_similar = 0;
        const auto neighborhood = factor.collect_closed_neighborhood(vertex);
        for (const auto neighbor : neighborhood) {
            if (neighbor != vertex &&
                factor.check_similarity(vertex, neighbor, threshold).similar) {
                ++factor_similar;
            }
        }
        std::uint64_t quotient_similar = eq.members(class_id).size() - 1;
        for (const auto& edge : eq.neighbors(class_id)) {
            const auto required = threshold.required_common_neighbors(
                eq.degree(class_id), eq.degree(edge.neighbor_class));
            if (edge.common_neighbors >= required) {
                quotient_similar += eq.members(edge.neighbor_class).size();
            }
        }
        std::cout << "vertex=" << vertex << '\n'
                  << "class=" << class_id << '\n'
                  << "class_size=" << eq.members(class_id).size() << '\n'
                  << "degree=" << eq.degree(class_id) << '\n'
                  << "factor_similar_degree=" << factor_similar << '\n'
                  << "quotient_similar_degree=" << quotient_similar << '\n';
        return factor_similar == quotient_similar ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "eqi_verify_vertex: " << error.what() << '\n';
        return 2;
    }
}
