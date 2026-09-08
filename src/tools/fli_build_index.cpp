#include "hin/HinGraph.h"
#include "index/FactorIndex.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void print_usage(const char* executable) {
    std::cerr << "Usage: " << executable
              << " <hin-dataset-directory> <meta-path> <output-index.fli>\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        print_usage(argv[0]);
        return 2;
    }

    try {
        const std::filesystem::path dataset_directory = argv[1];
        const std::string meta_path_text = argv[2];
        const std::filesystem::path index_file = argv[3];
        const auto load_begin = std::chrono::steady_clock::now();
        const auto graph = hinscan::HinGraph::load(dataset_directory);
        const auto meta_path = hinscan::parse_meta_path(graph, meta_path_text);
        const auto load_end = std::chrono::steady_clock::now();
        const auto index = hinscan::FactorIndex::build(graph, meta_path);
        const auto save_begin = std::chrono::steady_clock::now();
        index.save(index_file);
        const auto save_end = std::chrono::steady_clock::now();

        const auto load_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(load_end -
                                                                  load_begin)
                .count();
        const auto save_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(save_end -
                                                                  save_begin)
                .count();
        const auto& stats = index.stats();
        std::cout << "dataset=" << dataset_directory.string() << '\n'
                  << "meta_path=" << meta_path_text << '\n'
                  << "load_ms=" << load_ms << '\n'
                  << "index_build_ms=" << stats.build_milliseconds << '\n'
                  << "half_expansion_ms=" << stats.half_expansion_ms << '\n'
                  << "degree_compute_ms=" << stats.degree_compute_ms << '\n'
                  << "posting_order_ms=" << stats.posting_order_ms << '\n'
                  << "half_expansion_entries=" << stats.half_expansion_entries << '\n'
                  << "index_save_ms=" << save_ms << '\n'
                  << "target_vertices=" << stats.target_vertices << '\n'
                  << "center_vertices=" << stats.center_vertices << '\n'
                  << "half_path_incidences=" << stats.half_path_incidences
                  << '\n'
                  << "projected_edges=" << stats.exact_projected_edges << '\n'
                  << "index_file=" << index_file.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fli_build_index: " << error.what() << '\n';
        return 1;
    }
}
