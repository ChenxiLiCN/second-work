#include "hin/HinGraph.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void print_usage(const char* executable) {
    std::cerr << "Usage: " << executable
              << " <hin-dataset-directory> <symmetric-meta-path> <output-directory>\n"
              << "Example: " << executable
              << " data/raw/hin_text/dblp_small A-P-A "
                 "data/derived/transformed/dblp_small/APA\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        print_usage(argv[0]);
        return 2;
    }

    try {
        const std::filesystem::path input_directory = argv[1];
        const std::string meta_path_specification = argv[2];
        const std::filesystem::path output_directory = argv[3];

        const auto load_begin = std::chrono::steady_clock::now();
        const auto graph = hinscan::HinGraph::load(input_directory);
        const auto meta_path =
            hinscan::parse_meta_path(graph, meta_path_specification);
        const auto load_end = std::chrono::steady_clock::now();

        hinscan::MaterializationStats stats;
        const auto adjacency = hinscan::materialize_symmetric_meta_path(
            graph, meta_path, &stats);
        hinscan::write_pscan_binary(output_directory, adjacency);

        const auto load_milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(load_end -
                                                                  load_begin)
                .count();
        std::cout << "dataset=" << input_directory.string() << '\n'
                  << "meta_path=" << meta_path_specification << '\n'
                  << "vertices=" << stats.vertices << '\n'
                  << "undirected_edges=" << stats.undirected_edges << '\n'
                  << "generated_path_endpoints="
                  << stats.generated_path_endpoints << '\n'
                  << "load_ms=" << load_milliseconds << '\n'
                  << "materialize_ms=" << stats.materialization_milliseconds
                  << '\n'
                  << "output=" << output_directory.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hin_materialize: " << error.what() << '\n';
        return 1;
    }
}
