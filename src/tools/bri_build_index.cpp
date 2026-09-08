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
        const auto graph = hinscan::HinGraph::load(argv[1]);
        const auto loaded = std::chrono::steady_clock::now();
        graph.save_binary(argv[2]);
        const auto saved = std::chrono::steady_clock::now();
        std::uint64_t edges = 0;
        for (const auto& relation : graph.relations()) {
            edges += relation.loaded_edge_count;
        }
        std::cout << "vertex_types=" << graph.vertex_types().size() << '\n'
                  << "relation_types=" << graph.relations().size() << '\n'
                  << "relation_edges=" << edges << '\n'
                  << "hin_load_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         loaded - begin).count() << '\n'
                  << "bri_save_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         saved - loaded).count() << '\n'
                  << "bri_bytes=" << std::filesystem::file_size(argv[2])
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bri_build_index: " << error.what() << '\n';
        return 1;
    }
}
