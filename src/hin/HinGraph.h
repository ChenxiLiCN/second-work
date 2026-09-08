#ifndef HINSCAN_HIN_GRAPH_H
#define HINSCAN_HIN_GRAPH_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hinscan {

using VertexId = std::uint32_t;

struct VertexType {
    std::string name;
    std::uint64_t count = 0;
};

struct Relation {
    std::uint32_t source_type = 0;
    std::uint32_t target_type = 0;
    std::uint64_t declared_edge_count = 0;
    std::uint64_t loaded_edge_count = 0;
    std::vector<std::vector<VertexId>> forward;
    std::vector<std::vector<VertexId>> reverse;
};

struct Transition {
    const Relation* relation = nullptr;
    bool use_reverse = false;

    const std::vector<VertexId>& neighbors(VertexId vertex) const;
};

class HinGraph {
public:
    static HinGraph load(const std::filesystem::path& dataset_directory);
    static HinGraph load_binary(const std::filesystem::path& index_file);
    void save_binary(const std::filesystem::path& index_file) const;

    const std::vector<VertexType>& vertex_types() const noexcept;
    const std::vector<Relation>& relations() const noexcept;
    std::uint32_t type_id(const std::string& token) const;
    Transition transition(std::uint32_t source_type,
                          std::uint32_t target_type) const;

private:
    std::vector<VertexType> vertex_types_;
    std::vector<Relation> relations_;
};

struct MaterializationStats {
    std::uint64_t vertices = 0;
    std::uint64_t undirected_edges = 0;
    std::uint64_t generated_path_endpoints = 0;
    std::uint64_t materialization_milliseconds = 0;
};

std::vector<std::uint32_t> parse_meta_path(const HinGraph& graph,
                                           const std::string& specification);

std::vector<std::vector<VertexId>> materialize_symmetric_meta_path(
    const HinGraph& graph,
    const std::vector<std::uint32_t>& meta_path,
    MaterializationStats* stats);

void write_pscan_binary(const std::filesystem::path& output_directory,
                        const std::vector<std::vector<VertexId>>& adjacency);

}  // namespace hinscan

#endif
