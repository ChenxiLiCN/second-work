#include "hin/HinGraph.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <hin-directory> <output.bri>\n";
        return 2;
    }
    try {
        const auto begin = std::chrono::steady_clock::now();
#ifdef HINSCAN_MEASURE_BUILD
        double relation_build_ms = 0;
        auto graph = hinscan::HinGraph::load(argv[1], &relation_build_ms);
#else
        auto graph = hinscan::HinGraph::load(argv[1]);
#endif
        const auto loaded = std::chrono::steady_clock::now();
#ifdef HINSCAN_BUILD_ROUNDTRIP
        graph.prepare_roundtrip_metadata();
#endif
        const auto prepared = std::chrono::steady_clock::now();
        graph.save_binary(argv[2]);
        const auto saved = std::chrono::steady_clock::now();
        std::uint64_t edges = 0;
        for (const auto& relation : graph.relations()) {
            edges += relation.loaded_edge_count;
        }
        std::cout << "vertex_types=" << graph.vertex_types().size() << '\n'
#ifdef HINSCAN_MEASURE_BUILD
                  << "offline_compute_ms=" << relation_build_ms << '\n'
                  << "offline_timing_scope=adjacency_allocation_population_sort_dedup_from_buffered_pairs\n"
#endif
                  << "relation_types=" << graph.relations().size() << '\n'
                  << "relation_edges=" << edges << '\n'
                  << "hin_load_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         loaded - begin).count() << '\n'
                  << "roundtrip_prepare_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(prepared-loaded).count() << '\n'
                  << "bri_save_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         saved - prepared).count() << '\n'
                  << "bri_bytes=" << std::filesystem::file_size(argv[2])
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bri_build_index: " << error.what() << '\n';
        return 1;
    }
}
