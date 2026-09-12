#include "hin/HinGraph.h"
#include "index/RelationContainmentForest.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
void word(std::ostream& out, std::uint64_t value, unsigned bytes) {
    for (unsigned i=0; i<bytes; ++i) out.put(static_cast<char>((value >> (8*i)) & 255));
}
void save(const std::filesystem::path& path, const hinscan::RelationContainmentForest& f,
          std::uint32_t relation, std::uint32_t direction, std::uint32_t source,
          std::uint32_t target, std::uint64_t n, std::uint64_t m) {
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out.write("RCFORE01", 8);
    for (auto value : {relation, direction, source, target}) word(out,value,4);
    for (auto value : {n,m,std::uint64_t(f.membership.size()),std::uint64_t(f.nodes.size())}) word(out,value,8);
    for (auto item : f.membership) { word(out,item.first,4); word(out,item.second,4); }
    for (const auto& node : f.nodes)
        for (auto value : {node.representative,node.weight,node.parent,node.depth,node.tin,node.tout})
            word(out,value,4);
    out.close();
}
}
int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: diagnose_containment <hin-text-directory> <NEW-output-directory>\n";
        return 2;
    }
    try {
        const std::filesystem::path output(argv[2]);
        if (!std::filesystem::create_directory(output))
            throw std::runtime_error("output directory must not already exist");
        double adjacency_ms = 0;
        const auto begin = Clock::now();
        const auto graph = hinscan::HinGraph::load(argv[1], &adjacency_ms);
        const auto loaded = Clock::now();
        std::ofstream report(output / "structure.json");
        report.exceptions(std::ios::failbit | std::ios::badbit);
        report << std::setprecision(12)
               << "{\n  \"diagnostic_only\": true,\n  \"query_inputs_used\": false,"
               << "\n  \"online_speedup_measured\": false,"
               << "\n  \"graph_load_wall_ms\": " << ms(loaded-begin)
               << ",\n  \"base_adjacency_compute_ms\": " << adjacency_ms
               << ",\n  \"views\": [\n";
        double compute = 0, write = 0;
        std::uint64_t total_bytes = 0;
        bool first = true;
        for (std::uint32_t r=0; r<graph.relations().size(); ++r) {
            const auto& relation = graph.relations()[r];
            for (std::uint32_t direction=0; direction<2; ++direction) {
                const auto& rows = direction ? relation.reverse : relation.forward;
                const auto source = direction ? relation.target_type : relation.source_type;
                const auto target = direction ? relation.source_type : relation.target_type;
                const auto start = Clock::now();
                const auto f = hinscan::RelationContainmentForest::build(
                    rows, graph.vertex_types()[target].count);
                const auto built = Clock::now();
                const auto name = "relation-" + std::to_string(r) + "-" + std::to_string(direction) + ".rcf";
                save(output/name, f, r, direction, source, target,
                     rows.size(),graph.vertex_types()[target].count);
                const auto saved = Clock::now();
                const auto bytes = std::filesystem::file_size(output/name);
                const auto expected = 56ULL + 8ULL*f.membership.size() + 24ULL*f.nodes.size();
                if (bytes != expected) throw std::logic_error("unexpected serialized size");
                compute += ms(built-start); write += ms(saved-built); total_bytes += bytes;
                if (!first) report << ",\n";
                first = false;
                report << "    {\"relation\":" << r << ",\"direction\":" << direction
                       << ",\"source_type\":" << source << ",\"target_type\":" << target
                       << ",\"source_vertices\":" << rows.size()
                       << ",\"nonempty_rows\":" << f.membership.size()
                       << ",\"empty_rows\":" << rows.size()-f.membership.size()
                       << ",\"relation_edges\":" << relation.loaded_edge_count
                       << ",\"classes\":" << f.nodes.size()
                       << ",\"parent_edges\":" << f.parent_edges
                       << ",\"max_depth\":" << f.max_depth << ",\"depth_sum\":" << f.depth_sum
                       << ",\"within_class_pairs\":" << f.within_class_pairs
                       << ",\"comparable_cross_class_pairs\":" << f.comparable_cross_class_pairs
                       << ",\"parent_candidates\":" << f.parent_candidates
                       << ",\"subset_binary_search_comparisons\":" << f.subset_comparisons
                       << ",\"grouping_ms\":" << f.grouping_ms
                       << ",\"parent_search_ms\":" << f.parent_search_ms
                       << ",\"labeling_ms\":" << f.labeling_ms
                       << ",\"forest_compute_ms\":" << ms(built-start)
                       << ",\"forest_bytes\":" << bytes << ",\"file\":\"" << name << "\"}";
                std::cout << "relation=" << r << " direction=" << direction << " classes="
                          << f.nodes.size() << " parents=" << f.parent_edges << std::endl;
            }
        }
        report << "\n  ],\n  \"forest_compute_ms\":" << compute
               << ",\n  \"offline_compute_ms\":" << adjacency_ms+compute
               << ",\n  \"forest_write_ms\":" << write
               << ",\n  \"forest_bytes\":" << total_bytes
               << ",\n  \"forest_bytes_exclude_base_graph\":true,\n  \"complete\":true\n}\n";
        report.close();
        std::cout << "all_completed=1\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "diagnose_containment: " << e.what() << '\n'; return 1;
    }
}
