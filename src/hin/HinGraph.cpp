#include "hin/HinGraph.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace hinscan {
namespace {

std::runtime_error data_error(const std::filesystem::path& file,
                              const std::string& message) {
    return std::runtime_error(file.string() + ": " + message);
}

template <typename T>   
void write_binary(std::ofstream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if (!output) {
        throw std::runtime_error("failed to write pSCAN binary output");
    }
}

template <typename T>
T read_binary(std::ifstream& input, const char* field) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) {
        throw std::runtime_error(std::string("failed to read BRI ") + field);
    }
    return value;
}

void write_adjacency_lists(
    std::ofstream& output,
    const std::vector<std::vector<VertexId>>& lists) {
    for (const auto& list : lists) {
        write_binary(output, static_cast<std::uint64_t>(list.size()));
        if (!list.empty()) {
            output.write(reinterpret_cast<const char*>(list.data()),
                         static_cast<std::streamsize>(
                             list.size() * sizeof(VertexId)));
            if (!output) {
                throw std::runtime_error("failed to write BRI adjacency");
            }
        }
    }
}

std::vector<std::vector<VertexId>> read_adjacency_lists(
    std::ifstream& input,
    std::uint64_t list_count,
    std::uint64_t value_limit,
    std::uint64_t expected_values, bool id_sorted = true) {
    if (list_count > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("BRI adjacency list count exceeds this machine");
    }
    std::vector<std::vector<VertexId>> lists(
        static_cast<std::size_t>(list_count));
    std::uint64_t values = 0;
    for (auto& list : lists) {
        const auto size = read_binary<std::uint64_t>(input, "list size");
        if (size > std::numeric_limits<std::size_t>::max() ||
            size > expected_values - values) {
            throw std::runtime_error("invalid BRI adjacency list size");
        }
        list.resize(static_cast<std::size_t>(size));
        if (!list.empty()) {
            input.read(reinterpret_cast<char*>(list.data()),
                       static_cast<std::streamsize>(
                           list.size() * sizeof(VertexId)));
            if (!input) {
                throw std::runtime_error("truncated BRI adjacency");
            }
        }
        if (id_sorted && (!std::is_sorted(list.begin(), list.end()) ||
            std::adjacent_find(list.begin(), list.end()) != list.end())) {
            throw std::runtime_error("BRI adjacency is not strictly sorted");
        }
        for (const auto value : list) {
            if (value >= value_limit) {
                throw std::runtime_error("BRI adjacency endpoint is out of range");
            }
        }
        values += size;
    }
    if (values != expected_values) {
        throw std::runtime_error("BRI relation edge count is inconsistent");
    }
    return lists;
}

std::vector<std::string> split_meta_path(const std::string& specification) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream input(specification);
    while (std::getline(input, token, '-')) {
        if (token.empty()) {
            throw std::invalid_argument("meta-path contains an empty type token: " +
                                        specification);
        }
        tokens.push_back(token);
    }
    if (tokens.empty()) {
        throw std::invalid_argument("meta-path is empty");
    }
    return tokens;
}

void sort_and_deduplicate(std::vector<std::vector<VertexId>>& lists) {
    for (auto& list : lists) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }
}

}  // namespace

const std::vector<VertexId>& Transition::neighbors(VertexId vertex) const {
    if (relation == nullptr) {
        throw std::logic_error("uninitialized HIN transition");
    }
    const auto& lists = use_reverse ? relation->reverse : relation->forward;
    if (vertex >= lists.size()) {
        throw std::out_of_range("vertex id exceeds transition domain");
    }
    return lists[vertex];
}

HinGraph HinGraph::load(const std::filesystem::path& dataset_directory, double* relation_build_ms) {
    if (relation_build_ms) *relation_build_ms = 0;
    HinGraph graph;
    const auto schema_file = dataset_directory / "base.txt";
    std::ifstream schema(schema_file);
    if (!schema) {
        throw data_error(schema_file, "cannot open schema");
    }

    std::uint32_t type_count = 0;
    if (!(schema >> type_count) || type_count == 0) {
        throw data_error(schema_file, "invalid vertex type count");
    }
    graph.vertex_types_.reserve(type_count);
    for (std::uint32_t i = 0; i < type_count; ++i) {
        VertexType type;
        if (!(schema >> type.name >> type.count)) {
            throw data_error(schema_file, "invalid vertex type declaration");
        }
        if (type.count > std::numeric_limits<VertexId>::max()) {
            throw data_error(schema_file, "one vertex type exceeds 32-bit local ids");
        }
        graph.vertex_types_.push_back(std::move(type));
    }

    std::uint32_t relation_count = 0;
    if (!(schema >> relation_count)) {
        throw data_error(schema_file, "invalid relation type count");
    }
    graph.relations_.reserve(relation_count);
    for (std::uint32_t i = 0; i < relation_count; ++i) {
        Relation relation;
        if (!(schema >> relation.source_type >> relation.target_type >>
              relation.declared_edge_count)) {
            throw data_error(schema_file, "invalid relation declaration");
        }
        if (relation.source_type >= type_count || relation.target_type >= type_count) {
            throw data_error(schema_file, "relation refers to an unknown vertex type");
        }
        if (!relation_build_ms) {
            relation.forward.resize(static_cast<std::size_t>(graph.vertex_types_[relation.source_type].count));
            relation.reverse.resize(static_cast<std::size_t>(graph.vertex_types_[relation.target_type].count));
        }
        graph.relations_.push_back(std::move(relation));
    }

    for (std::uint32_t relation_id = 0; relation_id < relation_count; ++relation_id) {
        auto& relation = graph.relations_[relation_id];
        const auto edge_file =
            dataset_directory / "edge" / (std::to_string(relation_id) + ".txt");
        std::ifstream input(edge_file);
        if (!input) {
            throw data_error(edge_file, "cannot open relation file");
        }

        std::uint32_t header_source = 0;
        std::uint32_t header_target = 0;
        std::uint64_t header_edges = 0;
        if (!(input >> header_source >> header_target >> header_edges)) {
            throw data_error(edge_file, "invalid relation header");
        }
        if (header_source != relation.source_type ||
            header_target != relation.target_type) {
            throw data_error(edge_file, "relation header disagrees with base.txt");
        }
        if (header_edges != relation.declared_edge_count) {
            throw data_error(edge_file, "edge count header disagrees with base.txt");
        }

        std::uint64_t source = 0;
        std::uint64_t target = 0;
        std::vector<std::pair<VertexId,VertexId>> input_pairs;
        if (relation_build_ms) input_pairs.reserve(relation.declared_edge_count);
        while (input >> source >> target) {
            if (source >= graph.vertex_types_[relation.source_type].count || target >= graph.vertex_types_[relation.target_type].count) {
                throw data_error(edge_file, "edge endpoint is outside its type domain");
            }
            if (relation_build_ms) input_pairs.emplace_back(source,target);
            else {
                relation.forward[static_cast<std::size_t>(source)].push_back(static_cast<VertexId>(target));
                relation.reverse[static_cast<std::size_t>(target)].push_back(static_cast<VertexId>(source));
            }
            ++relation.loaded_edge_count;
        }
        if (!input.eof()) {
            throw data_error(edge_file, "malformed edge record");
        }
        if (relation.loaded_edge_count != relation.declared_edge_count) {
            throw data_error(edge_file,
                             "loaded edge count does not match the declared count");
        }
        const auto organize_begin = std::chrono::steady_clock::now();
        if (relation_build_ms) {
            relation.forward.resize(graph.vertex_types_[relation.source_type].count);
            relation.reverse.resize(graph.vertex_types_[relation.target_type].count);
            for (const auto& edge : input_pairs) {
                relation.forward[edge.first].push_back(edge.second);
                relation.reverse[edge.second].push_back(edge.first);
            }
        }
        sort_and_deduplicate(relation.forward);
        sort_and_deduplicate(relation.reverse);
        if (relation_build_ms) *relation_build_ms += std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-organize_begin).count();
    }

    return graph;
}

void HinGraph::save_binary(const std::filesystem::path& index_file) const {
    if (index_file.has_parent_path()) {
        std::filesystem::create_directories(index_file.parent_path());
    }
    std::ofstream output(index_file, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create BRI index: " +
                                 index_file.string());
    }
    constexpr char magic[8] = {'H', 'I', 'N', 'B', 'R', 'I', '\0', '\0'};
    output.write(magic, sizeof(magic));
    const bool enhanced = std::any_of(relations_.begin(), relations_.end(),
        [](const Relation& r) { return r.roundtrip_ready; });
    write_binary(output, static_cast<std::uint32_t>(enhanced ? 2 : 1));
    write_binary(output, static_cast<std::uint32_t>(0x01020304U));
    write_binary(output, static_cast<std::uint32_t>(vertex_types_.size()));
    write_binary(output, static_cast<std::uint32_t>(relations_.size()));
    for (const auto& type : vertex_types_) {
        if (type.name.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("BRI vertex type name is too long");
        }
        write_binary(output, static_cast<std::uint32_t>(type.name.size()));
        output.write(type.name.data(),
                     static_cast<std::streamsize>(type.name.size()));
        write_binary(output, type.count);
    }
    for (const auto& relation : relations_) {
        std::uint64_t forward_edges = 0;
        std::uint64_t reverse_edges = 0;
        for (const auto& list : relation.forward) {
            forward_edges += list.size();
        }
        for (const auto& list : relation.reverse) {
            reverse_edges += list.size();
        }
        if (forward_edges != reverse_edges) {
            throw std::logic_error(
                "HIN forward and reverse relation indexes disagree");
        }
        write_binary(output, relation.source_type);
        write_binary(output, relation.target_type);
        write_binary(output, forward_edges);
        write_adjacency_lists(output, relation.forward);
        write_adjacency_lists(output, relation.reverse);
        if (enhanced) {
            write_binary(output, static_cast<std::uint32_t>(relation.roundtrip_ready));
            if (relation.roundtrip_ready) {
                for (auto d : relation.source_closed_degrees) write_binary(output, d);
                for (auto d : relation.target_closed_degrees) write_binary(output, d);
                write_adjacency_lists(output, relation.source_ordered_postings);
                write_adjacency_lists(output, relation.target_ordered_postings);
            }
        }
    }
    if (!output) {
        throw std::runtime_error("failed to write BRI index payload");
    }
}

HinGraph HinGraph::load_binary(const std::filesystem::path& index_file) {
    std::ifstream input(index_file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open BRI index: " +
                                 index_file.string());
    }
    constexpr char expected[8] = {'H', 'I', 'N', 'B', 'R', 'I', '\0', '\0'};
    char magic[8]{};
    input.read(magic, sizeof(magic));
    const auto version = read_binary<std::uint32_t>(input, "version");
    const auto endian = read_binary<std::uint32_t>(input, "endian marker");
    if (std::memcmp(magic, expected, sizeof(magic)) != 0 || (version != 1 && version != 2) ||
        endian != 0x01020304U) {
        throw std::runtime_error("unsupported BRI index format");
    }
    const auto type_count = read_binary<std::uint32_t>(input, "type count");
    const auto relation_count =
        read_binary<std::uint32_t>(input, "relation count");
    if (type_count == 0) {
        throw std::runtime_error("BRI contains no vertex types");
    }

    HinGraph graph;
    graph.vertex_types_.reserve(type_count);
    for (std::uint32_t type_id = 0; type_id < type_count; ++type_id) {
        const auto name_size =
            read_binary<std::uint32_t>(input, "type name size");
        std::string name(name_size, '\0');
        if (name_size != 0) {
            input.read(name.data(), static_cast<std::streamsize>(name_size));
            if (!input) {
                throw std::runtime_error("truncated BRI type name");
            }
        }
        const auto count = read_binary<std::uint64_t>(input, "vertex count");
        if (name.empty() || count > std::numeric_limits<VertexId>::max()) {
            throw std::runtime_error("invalid BRI vertex type");
        }
        graph.vertex_types_.push_back(VertexType{std::move(name), count});
    }

    graph.relations_.reserve(relation_count);
    for (std::uint32_t relation_id = 0; relation_id < relation_count;
         ++relation_id) {
        Relation relation;
        relation.source_type =
            read_binary<std::uint32_t>(input, "source type");
        relation.target_type =
            read_binary<std::uint32_t>(input, "target type");
        relation.loaded_edge_count =
            read_binary<std::uint64_t>(input, "edge count");
        relation.declared_edge_count = relation.loaded_edge_count;
        if (relation.source_type >= graph.vertex_types_.size() ||
            relation.target_type >= graph.vertex_types_.size()) {
            throw std::runtime_error("BRI relation refers to an unknown type");
        }
        const auto source_vertices =
            graph.vertex_types_[relation.source_type].count;
        const auto target_vertices =
            graph.vertex_types_[relation.target_type].count;
        relation.forward = read_adjacency_lists(
            input, source_vertices, target_vertices,
            relation.loaded_edge_count);
        relation.reverse = read_adjacency_lists(
            input, target_vertices, source_vertices,
            relation.loaded_edge_count);
        if (version == 2) {
            const auto flag = read_binary<std::uint32_t>(input, "roundtrip flag");
            if (flag > 1) throw std::runtime_error("invalid roundtrip flag");
            relation.roundtrip_ready = flag != 0;
            if (relation.roundtrip_ready) {
                auto read_degrees = [&](std::uint64_t n) {
                    std::vector<std::uint64_t> result(n);
                    for (auto& d : result) {
                        d = read_binary<std::uint64_t>(input, "roundtrip degree");
                        if (d == 0 || d > n) throw std::runtime_error("invalid roundtrip degree");
                    }
                    return result;
                };
                relation.source_closed_degrees = read_degrees(source_vertices);
                relation.target_closed_degrees = read_degrees(target_vertices);
                relation.source_ordered_postings = read_adjacency_lists(input,
                    target_vertices, source_vertices, relation.loaded_edge_count, false);
                relation.target_ordered_postings = read_adjacency_lists(input,
                    source_vertices, target_vertices, relation.loaded_edge_count, false);
                auto validate = [](const auto& ordered, const auto& original, const auto& degrees) {
                    std::vector<std::size_t> seen(degrees.size(), 0);
                    for (std::size_t row=0; row<ordered.size(); ++row) {
                        if (ordered[row].size() != original[row].size())
                            throw std::runtime_error("roundtrip posting size mismatch");
                        VertexId previous=0; bool first=true;
                        for (auto v : ordered[row]) {
                            if (seen[v] == row+1 ||
                                !std::binary_search(original[row].begin(), original[row].end(), v) ||
                                (!first && (degrees[v] < degrees[previous] ||
                                  (degrees[v] == degrees[previous] && v <= previous))))
                                throw std::runtime_error("invalid roundtrip posting permutation");
                            seen[v]=row+1; previous=v; first=false;
                        }
                    }
                };
                validate(relation.source_ordered_postings, relation.reverse, relation.source_closed_degrees);
                validate(relation.target_ordered_postings, relation.forward, relation.target_closed_degrees);
            }
        }
        graph.relations_.push_back(std::move(relation));
    }
    if (input.peek() != std::ifstream::traits_type::eof()) {
        throw std::runtime_error("unexpected trailing data in BRI index");
    }
    return graph;
}

const std::vector<VertexType>& HinGraph::vertex_types() const noexcept {
    return vertex_types_;
}

void HinGraph::prepare_roundtrip_metadata() {
    for (auto& r : relations_) {
        // Same-type transitions have different semantics in the legacy loader.
        // Leave those on the established online path.
        if (r.source_type == r.target_type) continue;
        auto prepare = [](const auto& forward, const auto& reverse,
                          auto& degrees, auto& ordered) {
            degrees.assign(forward.size(), 1);
            std::vector<std::size_t> seen(forward.size(), 0);
            for (std::size_t u=0; u<forward.size(); ++u) {
                const auto epoch=u+1;
                seen[u]=epoch;
                for (auto w : forward[u]) for (auto v : reverse[w])
                    if (seen[v] != epoch) { seen[v]=epoch; ++degrees[u]; }
            }
            ordered=reverse;
            for (auto& row : ordered)
                std::sort(row.begin(), row.end(), [&](VertexId a, VertexId b) {
                    return degrees[a] != degrees[b] ? degrees[a] < degrees[b] : a < b;
                });
        };
        prepare(r.forward, r.reverse, r.source_closed_degrees, r.source_ordered_postings);
        prepare(r.reverse, r.forward, r.target_closed_degrees, r.target_ordered_postings);
        r.roundtrip_ready=true;
    }
}

const std::vector<Relation>& HinGraph::relations() const noexcept {
    return relations_;
}

std::uint32_t HinGraph::type_id(const std::string& token) const {
    for (std::uint32_t i = 0; i < vertex_types_.size(); ++i) {
        if (vertex_types_[i].name == token) {
            return i;
        }
    }

    std::size_t consumed = 0;
    try {
        const auto numeric = std::stoul(token, &consumed);
        if (consumed == token.size() && numeric < vertex_types_.size()) {
            return static_cast<std::uint32_t>(numeric);
        }
    } catch (const std::exception&) {
        // Report the common unknown-token error below.
    }
    throw std::invalid_argument("unknown vertex type in meta-path: " + token);
}

Transition HinGraph::transition(std::uint32_t source_type,
                                std::uint32_t target_type) const {
    const Relation* match = nullptr;
    bool use_reverse = false;
    for (const auto& relation : relations_) {
        const bool forward = relation.source_type == source_type &&
                             relation.target_type == target_type;
        const bool reverse = relation.source_type == target_type &&
                             relation.target_type == source_type;
        if (!forward && !reverse) {
            continue;
        }
        if (match != nullptr) {
            throw std::invalid_argument(
                "meta-path step is ambiguous because multiple relation types connect " +
                std::to_string(source_type) + " and " + std::to_string(target_type));
        }
        match = &relation;
        use_reverse = reverse;
    }
    if (match == nullptr) {
        throw std::invalid_argument("no relation connects type " +
                                    std::to_string(source_type) + " to type " +
                                    std::to_string(target_type));
    }
    return Transition{match, use_reverse};
}

std::vector<std::uint32_t> parse_meta_path(const HinGraph& graph,
                                           const std::string& specification) {
    const auto tokens = split_meta_path(specification);
    std::vector<std::uint32_t> result;
    result.reserve(tokens.size());
    for (const auto& token : tokens) {
        result.push_back(graph.type_id(token));
    }
    if (result.size() < 3) {
        throw std::invalid_argument("meta-path must contain at least two relation steps");
    }
    if (result.front() != result.back()) {
        throw std::invalid_argument(
            "pSCAN projection requires a symmetric meta-path with equal endpoint types");
    }
    for (std::size_t i = 1; i < result.size(); ++i) {
        (void)graph.transition(result[i - 1], result[i]);
    }
    return result;
}

std::vector<std::vector<VertexId>> materialize_symmetric_meta_path(
    const HinGraph& graph,
    const std::vector<std::uint32_t>& meta_path,
    MaterializationStats* stats) {
    if (meta_path.size() < 3 || meta_path.front() != meta_path.back()) {
        throw std::invalid_argument("materialization requires a symmetric meta-path");
    }
    for (std::size_t i = 0, j = meta_path.size() - 1; i < j; ++i, --j) {
        if (meta_path[i] != meta_path[j]) {
            throw std::invalid_argument("meta-path type sequence is not symmetric");
        }
    }

    const auto begin = std::chrono::steady_clock::now();
    const auto vertex_count =
        graph.vertex_types()[meta_path.front()].count;
    if (vertex_count > std::numeric_limits<VertexId>::max()) {
        throw std::overflow_error("projection has more than 32-bit vertices");
    }

    std::vector<Transition> transitions;
    transitions.reserve(meta_path.size() - 1);
    for (std::size_t i = 1; i < meta_path.size(); ++i) {
        transitions.push_back(graph.transition(meta_path[i - 1], meta_path[i]));
    }

    std::vector<std::pair<VertexId, VertexId>> undirected_edges;
    std::vector<VertexId> frontier;
    std::vector<VertexId> next;
    std::uint64_t generated_path_endpoints = 0;

    for (std::uint64_t source64 = 0; source64 < vertex_count; ++source64) {
        const auto source = static_cast<VertexId>(source64);
        frontier.assign(1, source);
        for (const auto& transition : transitions) {
            next.clear();
            for (const auto vertex : frontier) {
                const auto& neighbors = transition.neighbors(vertex);
                next.insert(next.end(), neighbors.begin(), neighbors.end());
            }
            std::sort(next.begin(), next.end());
            next.erase(std::unique(next.begin(), next.end()), next.end());
            frontier.swap(next);
            if (frontier.empty()) {
                break;
            }
        }

        generated_path_endpoints += frontier.size();
        for (const auto target : frontier) {
            if (source == target) {
                continue;
            }
            undirected_edges.emplace_back(std::min(source, target),
                                          std::max(source, target));
        }
    }

    std::sort(undirected_edges.begin(), undirected_edges.end());
    undirected_edges.erase(
        std::unique(undirected_edges.begin(), undirected_edges.end()),
        undirected_edges.end());

    std::vector<std::vector<VertexId>> adjacency(
        static_cast<std::size_t>(vertex_count));
    for (const auto& [source, target] : undirected_edges) {
        adjacency[source].push_back(target);
        adjacency[target].push_back(source);
    }

    const auto end = std::chrono::steady_clock::now();
    if (stats != nullptr) {
        stats->vertices = vertex_count;
        stats->undirected_edges = undirected_edges.size();
        stats->generated_path_endpoints = generated_path_endpoints;
        stats->materialization_milliseconds =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(end - begin)
                    .count());
    }
    return adjacency;
}

void write_pscan_binary(const std::filesystem::path& output_directory,
                        const std::vector<std::vector<VertexId>>& adjacency) {
    if (adjacency.size() > static_cast<std::size_t>(
                               std::numeric_limits<std::int32_t>::max())) {
        throw std::overflow_error("pSCAN binary format supports at most INT_MAX vertices");
    }

    std::uint64_t directed_edges64 = 0;
    for (const auto& neighbors : adjacency) {
        directed_edges64 += neighbors.size();
    }
    if (directed_edges64 > static_cast<std::uint64_t>(
                               std::numeric_limits<std::int32_t>::max())) {
        throw std::overflow_error(
            "pSCAN binary format supports at most INT_MAX adjacency entries");
    }

    std::filesystem::create_directories(output_directory);
    const auto degree_file = output_directory / "b_degree.bin";
    const auto adjacency_file = output_directory / "b_adj.bin";
    std::ofstream degrees(degree_file, std::ios::binary | std::ios::trunc);
    std::ofstream edges(adjacency_file, std::ios::binary | std::ios::trunc);
    if (!degrees) {
        throw data_error(degree_file, "cannot create output");
    }
    if (!edges) {
        throw data_error(adjacency_file, "cannot create output");
    }

    const std::int32_t int_size = sizeof(std::int32_t);
    const auto vertex_count = static_cast<std::int32_t>(adjacency.size());
    const auto directed_edges = static_cast<std::int32_t>(directed_edges64);
    write_binary(degrees, int_size);
    write_binary(degrees, vertex_count);
    write_binary(degrees, directed_edges);

    for (const auto& neighbors : adjacency) {
        const auto degree = static_cast<std::int32_t>(neighbors.size());
        write_binary(degrees, degree);
        for (const auto neighbor : neighbors) {
            const auto value = static_cast<std::int32_t>(neighbor);
            write_binary(edges, value);
        }
    }
}

}  // namespace hinscan
