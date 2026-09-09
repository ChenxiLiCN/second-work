#include "index/FactorIndex.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace hinscan {
namespace {

std::uint64_t greatest_common_divisor(std::uint64_t left,
                                      std::uint64_t right) {
    while (right != 0) {
        const auto remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

std::uint64_t square(std::uint64_t value) {
    if (value != 0 && value > std::numeric_limits<std::uint64_t>::max() / value) {
        throw std::overflow_error("similarity threshold square exceeds uint64");
    }
    return value * value;
}

template <typename T>
void write_value(std::ofstream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if (!output) {
        throw std::runtime_error("failed to write FLI index");
    }
}

template <typename T>
T read_value(std::ifstream& input, const char* description) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!input) {
        throw std::runtime_error(std::string("failed to read FLI index ") +
                                 description);
    }
    return value;
}

void write_lists(std::ofstream& output,
                 const std::vector<std::vector<VertexId>>& lists) {
    for (const auto& list : lists) {
        write_value(output, static_cast<std::uint64_t>(list.size()));
        if (!list.empty()) {
            output.write(reinterpret_cast<const char*>(list.data()),
                         static_cast<std::streamsize>(list.size() *
                                                      sizeof(VertexId)));
            if (!output) {
                throw std::runtime_error("failed to write FLI index list");
            }
        }
    }
}

std::vector<std::vector<VertexId>> read_lists(std::ifstream& input,
                                              std::uint64_t list_count,
                                              std::uint64_t value_limit,
                                              std::uint64_t* value_count,
                                              bool require_vertex_order = true) {
    if (list_count > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("FLI index list count exceeds this machine");
    }
    std::vector<std::vector<VertexId>> lists(
        static_cast<std::size_t>(list_count));
    *value_count = 0;
    for (auto& list : lists) {
        const auto size = read_value<std::uint64_t>(input, "list size");
        if (size > std::numeric_limits<std::size_t>::max() ||
            size > value_limit) {
            throw std::runtime_error("invalid FLI index list size");
        }
        list.resize(static_cast<std::size_t>(size));
        if (!list.empty()) {
            input.read(reinterpret_cast<char*>(list.data()),
                       static_cast<std::streamsize>(list.size() *
                                                    sizeof(VertexId)));
            if (!input) {
                throw std::runtime_error("failed to read FLI index list");
            }
        }
        if (require_vertex_order &&
            (!std::is_sorted(list.begin(), list.end()) ||
             std::adjacent_find(list.begin(), list.end()) != list.end())) {
            throw std::runtime_error("FLI index list is not strictly sorted");
        }
        for (const auto value : list) {
            if (value >= value_limit) {
                throw std::runtime_error("FLI index list value is out of range");
            }
        }
        *value_count += size;
    }
    return lists;
}

}  // namespace

LazyUnionCursor::LazyUnionCursor(
    VertexId self,
    const std::vector<VertexId>& witnesses,
    const std::vector<std::vector<VertexId>>& postings)
    : self_(self), postings_(&postings) {
    for (const auto witness : witnesses) {
        if (witness >= postings.size()) {
            throw std::out_of_range("factor witness exceeds posting table");
        }
        const auto& list = postings[witness];
        if (!list.empty()) {
            heap_.push(HeapEntry{list.front(), witness, 0});
        }
    }
}

bool LazyUnionCursor::Greater::operator()(const HeapEntry& left,
                                          const HeapEntry& right) const {
    if (left.value != right.value) {
        return left.value > right.value;
    }
    return left.witness > right.witness;
}

bool LazyUnionCursor::next(VertexId* value) {
    if (value == nullptr) {
        throw std::invalid_argument("LazyUnionCursor::next requires an output pointer");
    }
    if (!self_pending_ && heap_.empty()) {
        return false;
    }

    VertexId candidate = self_;
    if (!self_pending_ || (!heap_.empty() && heap_.top().value < candidate)) {
        candidate = heap_.top().value;
    }
    if (self_pending_ && candidate == self_) {
        self_pending_ = false;
    }

    while (!heap_.empty() && heap_.top().value == candidate) {
        auto entry = heap_.top();
        heap_.pop();
        ++posting_entries_read_;
        const auto& list = (*postings_)[entry.witness];
        ++entry.offset;
        if (entry.offset < list.size()) {
            entry.value = list[entry.offset];
            heap_.push(entry);
        }
    }

    *value = candidate;
    return true;
}

std::uint64_t LazyUnionCursor::posting_entries_read() const noexcept {
    return posting_entries_read_;
}

SimilarityThreshold SimilarityThreshold::parse(const std::string& value) {
    if (value.empty()) {
        throw std::invalid_argument("epsilon is empty");
    }

    const auto dot = value.find('.');
    if (dot != std::string::npos && value.find('.', dot + 1) != std::string::npos) {
        throw std::invalid_argument("epsilon has more than one decimal point");
    }

    std::uint64_t numerator = 0;
    std::uint64_t denominator = 1;
    bool saw_digit = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char character = value[i];
        if (character == '.') {
            continue;
        }
        if (character < '0' || character > '9') {
            throw std::invalid_argument("epsilon must be a decimal number in (0, 1]");
        }
        saw_digit = true;
        numerator = numerator * 10 + static_cast<std::uint64_t>(character - '0');
        if (dot != std::string::npos && i > dot) {
            denominator *= 10;
        }
    }
    if (!saw_digit || numerator == 0 || numerator > denominator) {
        throw std::invalid_argument("epsilon must be a decimal number in (0, 1]");
    }

    const auto divisor = greatest_common_divisor(numerator, denominator);
    return SimilarityThreshold{numerator / divisor, denominator / divisor};
}

std::uint64_t SimilarityThreshold::required_common_neighbors(
    std::uint64_t left_degree,
    std::uint64_t right_degree) const {
    const long double ratio =
        (static_cast<long double>(left_degree) *
         static_cast<long double>(right_degree) *
         static_cast<long double>(numerator) *
         static_cast<long double>(numerator)) /
        (static_cast<long double>(denominator) *
         static_cast<long double>(denominator));
    auto required = static_cast<std::uint64_t>(std::sqrt(ratio));
    const long double required_squared =
        static_cast<long double>(required) * static_cast<long double>(required);
    if (required_squared + 1e-18L < ratio) {
        ++required;
    }
    return required;
}

bool SimilarityThreshold::fails_degree_ratio(std::uint64_t left_degree,
                                             std::uint64_t right_degree) const {
    const auto smaller = std::min(left_degree, right_degree);
    const auto larger = std::max(left_degree, right_degree);
    const long double left = static_cast<long double>(smaller) *
                             static_cast<long double>(square(denominator));
    const long double right = static_cast<long double>(larger) *
                              static_cast<long double>(square(numerator));
    return left < right;
}

FactorIndex FactorIndex::build(const HinGraph& graph,
                               const std::vector<std::uint32_t>& meta_path) {
    if (meta_path.size() < 3 || meta_path.size() % 2 == 0) {
        throw std::invalid_argument(
            "factor index requires an odd-length symmetric type sequence");
    }
    for (std::size_t i = 0, j = meta_path.size() - 1; i < j; ++i, --j) {
        if (meta_path[i] != meta_path[j]) {
            throw std::invalid_argument("factor index requires a symmetric meta-path");
        }
    }

    const auto begin = std::chrono::steady_clock::now();
    FactorIndex index;
    if (meta_path.size() == 3) {
        const auto t = graph.transition(meta_path[0], meta_path[1]);
        const auto& r = *t.relation;
        if (r.roundtrip_ready && r.source_type != r.target_type) {
            // First implementation owns query arrays; no expansion, union-count,
            // or posting sort is repeated. Copy time is included online.
            index.witnesses_ = t.use_reverse ? r.reverse : r.forward;
            index.postings_ = t.use_reverse ? r.forward : r.reverse;
            index.degrees_ = t.use_reverse ? r.target_closed_degrees : r.source_closed_degrees;
            index.degree_ordered_postings_ = t.use_reverse ? r.target_ordered_postings : r.source_ordered_postings;
            auto& st = index.stats_;
            st.used_roundtrip_metadata = true;
            st.target_vertices = index.witnesses_.size();
            st.center_vertices = index.postings_.size();
            st.half_path_incidences = r.loaded_edge_count;
            std::uint64_t total=0;
            for (auto d : index.degrees_) total += d;
            if (total < st.target_vertices || (total-st.target_vertices)%2)
                throw std::runtime_error("invalid roundtrip degree sum");
            st.exact_projected_edges = (total-st.target_vertices)/2;
            st.estimated_index_bytes = 3*st.half_path_incidences*sizeof(VertexId)
                + (3*st.target_vertices + st.center_vertices + 2)*sizeof(std::uint64_t);
            st.build_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-begin).count();
            return index;
        }
    }
    const auto target_count64 = graph.vertex_types()[meta_path.front()].count;
    const auto center_position = meta_path.size() / 2;
    const auto center_count64 = graph.vertex_types()[meta_path[center_position]].count;
    if (target_count64 > std::numeric_limits<VertexId>::max() ||
        center_count64 > std::numeric_limits<VertexId>::max()) {
        throw std::overflow_error("FLI-0 uses 32-bit local vertex ids");
    }

    const auto target_count = static_cast<std::size_t>(target_count64);
    const auto center_count = static_cast<std::size_t>(center_count64);
    index.witnesses_.resize(target_count);
    index.postings_.resize(center_count);

    std::vector<Transition> half_transitions;
    half_transitions.reserve(center_position);
    for (std::size_t step = 1; step <= center_position; ++step) {
        half_transitions.push_back(
            graph.transition(meta_path[step - 1], meta_path[step]));
    }

    std::vector<VertexId> frontier;
    std::vector<VertexId> next;
    std::uint64_t maximum_type_count = 0;
    for (const auto type : meta_path)
        maximum_type_count = std::max(maximum_type_count, graph.vertex_types()[type].count);
    std::vector<std::uint32_t> expansion_seen(static_cast<std::size_t>(maximum_type_count), 0);
    std::uint32_t expansion_epoch = 0;
    for (std::size_t source = 0; source < target_count; ++source) {
        frontier.assign(1, static_cast<VertexId>(source));
        for (const auto& transition : half_transitions) {
            next.clear();
            if (++expansion_epoch == 0) {
                std::fill(expansion_seen.begin(), expansion_seen.end(), 0);
                expansion_epoch = 1;
            }
            for (const auto vertex : frontier) {
                const auto& neighbors = transition.neighbors(vertex);
                index.stats_.half_expansion_entries += neighbors.size();
                for (const auto neighbor : neighbors) {
                    if (expansion_seen[neighbor] != expansion_epoch) {
                        expansion_seen[neighbor] = expansion_epoch;
                        next.push_back(neighbor);
                    }
                }
            }
            frontier.swap(next);
            if (frontier.empty()) {
                break;
            }
        }
        std::sort(frontier.begin(), frontier.end());
        index.witnesses_[source] = frontier;
        for (const auto center : frontier) {
            index.postings_[center].push_back(static_cast<VertexId>(source));
        }
        index.stats_.half_path_incidences += frontier.size();
    }

    const auto half_end = std::chrono::steady_clock::now();

    index.degrees_.resize(target_count);
    std::uint64_t closed_degree_sum = 0;
    std::vector<std::uint32_t> degree_seen(target_count, 0);
    for (std::size_t vertex = 0; vertex < target_count; ++vertex) {
        const auto epoch = static_cast<std::uint32_t>(vertex + 1);
        degree_seen[vertex] = epoch;
        std::uint64_t degree = 1;
        for (const auto witness : index.witnesses_[vertex]) {
            const auto& posting = index.postings_[witness];
            index.stats_.degree_merge_entries_read += posting.size();
            for (const auto neighbor : posting) {
                if (degree_seen[neighbor] != epoch) {
                    degree_seen[neighbor] = epoch;
                    ++degree;
                }
            }
        }
        index.degrees_[vertex] = degree;
        closed_degree_sum += degree;
    }

    const auto degree_end = std::chrono::steady_clock::now();

    index.degree_ordered_postings_ = index.postings_;
    for (auto& posting : index.degree_ordered_postings_) {
        std::sort(posting.begin(), posting.end(), [&](VertexId left, VertexId right) {
            if (index.degrees_[left] != index.degrees_[right]) {
                return index.degrees_[left] < index.degrees_[right];
            }
            return left < right;
        });
    }

    if (closed_degree_sum < target_count ||
        (closed_degree_sum - target_count) % 2 != 0) {
        throw std::logic_error("factor index produced an asymmetric projected graph");
    }
    index.stats_.target_vertices = target_count;
    index.stats_.center_vertices = center_count;
    index.stats_.exact_projected_edges =
        (closed_degree_sum - target_count) / 2;
    index.stats_.estimated_index_bytes =
        3 * index.stats_.half_path_incidences * sizeof(VertexId) +
        (target_count + center_count + 2) * sizeof(std::uint64_t) +
        target_count * sizeof(std::uint64_t);
    const auto end = std::chrono::steady_clock::now();
    index.stats_.half_expansion_ms = std::chrono::duration<double, std::milli>(half_end - begin).count();
    index.stats_.degree_compute_ms = std::chrono::duration<double, std::milli>(degree_end - half_end).count();
    index.stats_.posting_order_ms = std::chrono::duration<double, std::milli>(end - degree_end).count();
    index.stats_.build_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
    return index;
}

void FactorIndex::save(const std::filesystem::path& index_file) const {
    if (index_file.has_parent_path()) {
        std::filesystem::create_directories(index_file.parent_path());
    }
    std::ofstream output(index_file, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create FLI index: " +
                                 index_file.string());
    }

    constexpr char magic[8] = {'F', 'L', 'I', '0', 'I', 'D', 'X', '\0'};
    constexpr std::uint32_t version = 2;
    constexpr std::uint32_t endian_marker = 0x01020304U;
    output.write(magic, sizeof(magic));
    write_value(output, version);
    write_value(output, endian_marker);
    write_value(output, static_cast<std::uint32_t>(sizeof(VertexId)));
    write_value(output, static_cast<std::uint32_t>(sizeof(std::uint64_t)));
    write_value(output, static_cast<std::uint64_t>(witnesses_.size()));
    write_value(output, static_cast<std::uint64_t>(postings_.size()));
    write_value(output, stats_.half_path_incidences);
    write_value(output, stats_.exact_projected_edges);
    write_value(output, stats_.degree_merge_entries_read);
    write_value(output, stats_.estimated_index_bytes);
    write_lists(output, witnesses_);
    write_lists(output, postings_);
    write_lists(output, degree_ordered_postings_);
    for (const auto degree : degrees_) {
        write_value(output, degree);
    }
}

FactorIndex FactorIndex::load(const std::filesystem::path& index_file) {
    std::ifstream input(index_file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open FLI index: " + index_file.string());
    }

    constexpr char expected_magic[8] = {'F', 'L', 'I', '0', 'I', 'D', 'X', '\0'};
    char magic[8]{};
    input.read(magic, sizeof(magic));
    if (!input || std::memcmp(magic, expected_magic, sizeof(magic)) != 0) {
        throw std::runtime_error("invalid FLI index magic");
    }
    const auto version = read_value<std::uint32_t>(input, "version");
    const auto endian = read_value<std::uint32_t>(input, "endian marker");
    const auto vertex_id_size =
        read_value<std::uint32_t>(input, "vertex id size");
    const auto degree_size = read_value<std::uint32_t>(input, "degree size");
    if ((version != 1 && version != 2) || endian != 0x01020304U ||
        vertex_id_size != sizeof(VertexId) ||
        degree_size != sizeof(std::uint64_t)) {
        throw std::runtime_error("unsupported FLI index format");
    }

    FactorIndex index;
    const auto target_count = read_value<std::uint64_t>(input, "target count");
    const auto center_count = read_value<std::uint64_t>(input, "center count");
    index.stats_.half_path_incidences =
        read_value<std::uint64_t>(input, "half-path incidence count");
    index.stats_.exact_projected_edges =
        read_value<std::uint64_t>(input, "projected edge count");
    index.stats_.degree_merge_entries_read =
        read_value<std::uint64_t>(input, "degree merge count");
    index.stats_.estimated_index_bytes =
        read_value<std::uint64_t>(input, "estimated index bytes");
    if (target_count > std::numeric_limits<VertexId>::max() ||
        center_count > std::numeric_limits<VertexId>::max()) {
        throw std::runtime_error("FLI index vertex count exceeds 32-bit ids");
    }

    std::uint64_t witness_values = 0;
    std::uint64_t posting_values = 0;
    index.witnesses_ =
        read_lists(input, target_count, center_count, &witness_values);
    index.postings_ =
        read_lists(input, center_count, target_count, &posting_values);
    if (witness_values != index.stats_.half_path_incidences ||
        posting_values != index.stats_.half_path_incidences) {
        throw std::runtime_error("FLI index incidence counts are inconsistent");
    }

    std::uint64_t degree_ordered_values = 0;
    if (version >= 2) {
        index.degree_ordered_postings_ =
            read_lists(input,
                       center_count,
                       target_count,
                       &degree_ordered_values,
                       false);
        if (degree_ordered_values != index.stats_.half_path_incidences) {
            throw std::runtime_error(
                "FLI degree-ordered posting counts are inconsistent");
        }
    }

    index.degrees_.resize(static_cast<std::size_t>(target_count));
    for (auto& degree : index.degrees_) {
        degree = read_value<std::uint64_t>(input, "degree");
        if (degree == 0 || degree > target_count) {
            throw std::runtime_error("invalid degree in FLI index");
        }
    }
    if (version == 1) {
        index.degree_ordered_postings_ = index.postings_;
        for (auto& posting : index.degree_ordered_postings_) {
            std::sort(posting.begin(), posting.end(),
                      [&](VertexId left, VertexId right) {
                          if (index.degrees_[left] != index.degrees_[right]) {
                              return index.degrees_[left] < index.degrees_[right];
                          }
                          return left < right;
                      });
        }
    } else {
        for (const auto& posting : index.degree_ordered_postings_) {
            if (!std::is_sorted(
                    posting.begin(), posting.end(),
                    [&](VertexId left, VertexId right) {
                        if (index.degrees_[left] != index.degrees_[right]) {
                            return index.degrees_[left] < index.degrees_[right];
                        }
                        return left < right;
                    })) {
                throw std::runtime_error(
                    "FLI degree-ordered posting is not sorted by degree");
            }
        }
    }
    if (input.peek() != std::ifstream::traits_type::eof()) {
        throw std::runtime_error("unexpected trailing data in FLI index");
    }
    index.stats_.target_vertices = target_count;
    index.stats_.center_vertices = center_count;
    return index;
}

std::uint64_t FactorIndex::vertex_count() const noexcept {
    return witnesses_.size();
}

bool SimilarityThreshold::certifies_common(std::uint64_t common,
    std::uint64_t left, std::uint64_t right) const {
    // Exact four-factor comparison in 256 bits; portable to MSVC and MinGW.
    auto product = [](std::array<std::uint64_t,4> factors) {
        std::array<std::uint32_t,8> value{}; value[0]=1;
        for (auto factor : factors) {
            std::array<std::uint32_t,8> next{};
            const std::uint32_t words[2]={static_cast<std::uint32_t>(factor),
                                         static_cast<std::uint32_t>(factor >> 32)};
            for (std::size_t i=0; i<8; ++i) {
                std::uint64_t carry=0;
                std::size_t j=0;
                for (; j<2 && i+j<8; ++j) {
                    const auto x=static_cast<std::uint64_t>(value[i])*words[j]+next[i+j]+carry;
                    next[i+j]=static_cast<std::uint32_t>(x); carry=x>>32;
                }
                for (auto k=i+j; carry && k<8; ++k) {
                    const auto x=static_cast<std::uint64_t>(next[k])+carry;
                    next[k]=static_cast<std::uint32_t>(x); carry=x>>32;
                }
            }
            value=next;
        }
        return value;
    };
    const auto a=product({common,common,denominator,denominator});
    const auto b=product({left,right,numerator,numerator});
    for (int i=7; i>=0; --i) if (a[i]!=b[i]) return a[i]>b[i];
    return true;
}

std::uint64_t FactorIndex::center_count() const noexcept {
    return postings_.size();
}

std::uint64_t FactorIndex::degree(VertexId vertex) const {
    if (vertex >= degrees_.size()) {
        throw std::out_of_range("factor vertex id exceeds degree table");
    }
    return degrees_[vertex];
}

const std::vector<VertexId>& FactorIndex::witnesses(VertexId vertex) const {
    if (vertex >= witnesses_.size()) {
        throw std::out_of_range("factor vertex id exceeds witness table");
    }
    return witnesses_[vertex];
}

const std::vector<VertexId>& FactorIndex::posting(VertexId center) const {
    if (center >= postings_.size()) {
        throw std::out_of_range("factor center id exceeds posting table");
    }
    return postings_[center];
}

const std::vector<VertexId>& FactorIndex::degree_ordered_posting(
    VertexId center) const {
    if (center >= degree_ordered_postings_.size()) {
        throw std::out_of_range("factor center exceeds degree posting table");
    }
    return degree_ordered_postings_[center];
}

LazyUnionCursor FactorIndex::closed_neighborhood(VertexId vertex) const {
    return LazyUnionCursor(vertex, witnesses(vertex), postings_);
}

std::vector<VertexId> FactorIndex::collect_closed_neighborhood(
    VertexId vertex,
    std::uint64_t* posting_entries_read) const {
    std::vector<VertexId> neighborhood;
    neighborhood.reserve(static_cast<std::size_t>(degree(vertex)));
    auto cursor = closed_neighborhood(vertex);
    VertexId neighbor = 0;
    while (cursor.next(&neighbor)) {
        neighborhood.push_back(neighbor);
    }
    if (posting_entries_read != nullptr) {
        *posting_entries_read = cursor.posting_entries_read();
    }
    if (neighborhood.size() != degree(vertex)) {
        throw std::logic_error("collected neighborhood disagrees with stored degree");
    }
    return neighborhood;
}

SimilarityCheck FactorIndex::check_similarity(
    VertexId left,
    VertexId right,
    const SimilarityThreshold& threshold) const {
    SimilarityCheck result;
    const auto left_degree = degree(left);
    const auto right_degree = degree(right);
    result.required_common_neighbors =
        threshold.required_common_neighbors(left_degree, right_degree);
    if (threshold.fails_degree_ratio(left_degree, right_degree)) {
        result.degree_pruned = true;
        return result;
    }

    auto left_cursor = closed_neighborhood(left);
    auto right_cursor = closed_neighborhood(right);
    VertexId left_value = 0;
    VertexId right_value = 0;
    bool has_left = left_cursor.next(&left_value);
    bool has_right = right_cursor.next(&right_value);
    while (has_left && has_right) {
        if (left_value == right_value) {
            ++result.common_neighbors;
            has_left = left_cursor.next(&left_value);
            has_right = right_cursor.next(&right_value);
        } else if (left_value < right_value) {
            has_left = left_cursor.next(&left_value);
        } else {
            has_right = right_cursor.next(&right_value);
        }
    }
    result.posting_entries_read = left_cursor.posting_entries_read() +
                                  right_cursor.posting_entries_read();
    result.similar =
        result.common_neighbors >= result.required_common_neighbors;
    return result;
}

SimilarityCheck FactorIndex::check_similarity_with_left_neighborhood(
    VertexId left,
    const std::vector<VertexId>& left_neighborhood,
    VertexId right,
    const SimilarityThreshold& threshold) const {
    SimilarityCheck result;
    const auto left_degree = degree(left);
    const auto right_degree = degree(right);
    if (left_neighborhood.size() != left_degree ||
        !std::is_sorted(left_neighborhood.begin(), left_neighborhood.end())) {
        throw std::invalid_argument("invalid precomputed left neighborhood");
    }
    result.required_common_neighbors =
        threshold.required_common_neighbors(left_degree, right_degree);
    if (threshold.fails_degree_ratio(left_degree, right_degree)) {
        result.degree_pruned = true;
        return result;
    }

    auto right_cursor = closed_neighborhood(right);
    auto left_iterator = left_neighborhood.begin();
    VertexId right_value = 0;
    bool has_right = right_cursor.next(&right_value);
    while (left_iterator != left_neighborhood.end() && has_right) {
        if (*left_iterator == right_value) {
            ++result.common_neighbors;
            ++left_iterator;
            has_right = right_cursor.next(&right_value);
        } else if (*left_iterator < right_value) {
            ++left_iterator;
        } else {
            has_right = right_cursor.next(&right_value);
        }
    }
    result.posting_entries_read = right_cursor.posting_entries_read();
    result.similar =
        result.common_neighbors >= result.required_common_neighbors;
    return result;
}

const FactorIndexStats& FactorIndex::stats() const noexcept {
    return stats_;
}

}  // namespace hinscan
