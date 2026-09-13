#include "hin/HinGraph.h"
#include "index/MajorityIndex.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kHeaderBytes = 88;
constexpr std::size_t kChecksumOffset = 80;
constexpr std::size_t kDirectionHeaderBytes = 48;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename T>
T load_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset,
            "test binary field is out of range");
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

template <typename T>
void store_le(std::vector<std::uint8_t>* bytes, std::size_t offset, T value) {
    require(offset <= bytes->size() && sizeof(T) <= bytes->size() - offset,
            "test binary field is out of range");
    std::memcpy(bytes->data() + offset, &value, sizeof(T));
}

std::uint64_t crc64(const std::uint8_t* data, std::size_t size) {
    std::uint64_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint64_t>(data[i]) << 56U;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & (std::uint64_t{1} << 63U))
                      ? (crc << 1U) ^ 0x42F0E1EBA9EA3693ULL
                      : crc << 1U;
        }
    }
    return crc;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    require(static_cast<bool>(input), "cannot read test index");
    const auto end = input.tellg();
    require(end >= 0, "cannot size test index");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    require(static_cast<bool>(input), "cannot read complete test index");
    return bytes;
}

void write_bytes(const std::filesystem::path& file,
                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "cannot write test index");
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    require(static_cast<bool>(output), "cannot write complete test index");
}

void repair_checksum(std::vector<std::uint8_t>* bytes) {
    require(bytes->size() >= kHeaderBytes, "test index has no full header");
    store_le<std::uint64_t>(
        bytes, kChecksumOffset,
        crc64(bytes->data() + kHeaderBytes, bytes->size() - kHeaderBytes));
}

void expect_load_failure(const std::filesystem::path& file,
                         const hinscan::HinGraph& graph,
                         const std::string& message) {
    try {
        (void)hinscan::MajorityIndex::load(file, graph);
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

std::filesystem::path write_fixture(const std::filesystem::path& root,
                                    const std::string& name,
                                    bool changed = false) {
    const auto directory = root / name;
    std::filesystem::create_directories(directory / "edge");
    {
        std::ofstream schema(directory / "base.txt");
        schema << "2\nA 4\nB 4\n1\n0 1 12\n";
    }
    {
        std::ofstream relation(directory / "edge" / "0.txt");
        relation << "0 1 12\n";
        const unsigned omitted[4] = {changed ? 2U : 3U, 2U, 1U, 0U};
        for (unsigned u = 0; u < 4; ++u) {
            unsigned written = 0;
            for (unsigned v = 0; v < 4; ++v) {
                if (v != omitted[u] && written++ < 3) {
                    relation << u << ' ' << v << '\n';
                }
            }
        }
    }
    return directory;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <temporary-directory>\n";
        return 2;
    }
    try {
        const auto root = std::filesystem::path(argv[1]);
        std::filesystem::create_directories(root);
        const auto fixture = write_fixture(root, "original");
        const auto graph = hinscan::HinGraph::load(fixture);
        const auto index = hinscan::MajorityIndex::build(graph);

        const auto forward = graph.transition(0, 1);
        const auto& groups = index.groups_for(graph, forward);
        const std::vector<std::uint64_t> expected_offsets{
            0, 3, 5, 9, 12, 15, 17, 20};
        const std::vector<hinscan::VertexId> expected_members{
            0, 1, 2, 0, 1, 0, 1, 2, 3, 0, 1, 3,
            0, 2, 3, 2, 3, 1, 2, 3};
        require(groups.offsets == expected_offsets,
                "forward majority group offsets differ from hand oracle");
        require(groups.members == expected_members,
                "forward majority group members differ from hand oracle");
        require(index.group_count() == 14, "two-direction group count");
        require(index.member_count() == 40, "two-direction member count");
        require(index.member_count() <= 4 * 12,
                "linear two-direction member bound");

        const auto reverse = graph.transition(1, 0);
        const auto& reverse_groups = index.groups_for(graph, reverse);
        require(reverse_groups.offsets.size() == 8,
                "reverse relation group count");
        require(reverse_groups.members.size() == 20,
                "reverse relation member count");

        const auto file = root / "groups.mgi";
        index.save(file, graph);
        const auto loaded = hinscan::MajorityIndex::load(file, graph);
        require(loaded.groups_for(graph, forward).offsets == expected_offsets &&
                    loaded.groups_for(graph, forward).members == expected_members,
                "saved majority groups did not round-trip");

        const auto changed_fixture = write_fixture(root, "changed", true);
        const auto changed_graph = hinscan::HinGraph::load(changed_fixture);
        expect_load_failure(file, changed_graph,
                            "graph fingerprint mismatch was accepted");

        const auto pristine = read_bytes(file);
        require(pristine.size() > kHeaderBytes + kDirectionHeaderBytes,
                "test index unexpectedly small");

        auto broken = pristine;
        broken.back() ^= 0x80U;
        write_bytes(root / "checksum.mgi", broken);
        expect_load_failure(root / "checksum.mgi", graph,
                            "bad payload checksum was accepted");

        broken = pristine;
        broken.pop_back();
        write_bytes(root / "truncated.mgi", broken);
        expect_load_failure(root / "truncated.mgi", graph,
                            "truncated payload was accepted");

        broken = pristine;
        broken.push_back(0);
        write_bytes(root / "trailing.mgi", broken);
        expect_load_failure(root / "trailing.mgi", graph,
                            "trailing payload was accepted");

        const auto first_direction = kHeaderBytes;
        const auto first_group_count =
            load_le<std::uint64_t>(pristine, first_direction + 32);
        const auto first_member_count =
            load_le<std::uint64_t>(pristine, first_direction + 40);
        require(first_group_count == 7 && first_member_count == 20,
                "unexpected test payload dimensions");
        const auto first_offsets = first_direction + kDirectionHeaderBytes;
        const auto first_members =
            first_offsets + (first_group_count + 1) * sizeof(std::uint64_t);

        broken = pristine;
        store_le<std::uint64_t>(&broken, first_offsets + sizeof(std::uint64_t), 0);
        repair_checksum(&broken);
        write_bytes(root / "offset.mgi", broken);
        expect_load_failure(root / "offset.mgi", graph,
                            "non-increasing group offset was accepted");

        broken = pristine;
        store_le<hinscan::VertexId>(&broken, first_members, 4);
        repair_checksum(&broken);
        write_bytes(root / "id.mgi", broken);
        expect_load_failure(root / "id.mgi", graph,
                            "out-of-range group member was accepted");

        broken = pristine;
        const auto first = load_le<hinscan::VertexId>(broken, first_members);
        const auto second = load_le<hinscan::VertexId>(
            broken, first_members + sizeof(hinscan::VertexId));
        store_le<hinscan::VertexId>(&broken, first_members, second);
        store_le<hinscan::VertexId>(
            &broken, first_members + sizeof(hinscan::VertexId), first);
        repair_checksum(&broken);
        write_bytes(root / "order.mgi", broken);
        expect_load_failure(root / "order.mgi", graph,
                            "unsorted group members were accepted");

        broken = pristine;
        store_le<std::uint64_t>(
            &broken, first_direction + 40,
            std::numeric_limits<std::uint64_t>::max());
        repair_checksum(&broken);
        write_bytes(root / "length.mgi", broken);
        expect_load_failure(root / "length.mgi", graph,
                            "corrupt direction length was accepted");

        std::cout << "group_count=" << index.group_count() << '\n'
                  << "member_count=" << index.member_count() << '\n'
                  << "all_passed=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "verify_majority_index: " << error.what() << '\n';
        return 1;
    }
}
