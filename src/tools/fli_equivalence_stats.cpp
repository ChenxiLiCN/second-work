#include "index/FactorIndex.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {

struct Signature {
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::uint64_t size = 0;

    bool operator==(const Signature& other) const noexcept {
        return first == other.first && second == other.second &&
               size == other.size;
    }
};

struct SignatureHash {
    std::size_t operator()(const Signature& value) const noexcept {
        return static_cast<std::size_t>(
            value.first ^ (value.second + 0x9e3779b97f4a7c15ULL +
                           (value.first << 6U) + (value.first >> 2U)) ^
            value.size);
    }
};

Signature signature(const std::vector<hinscan::VertexId>& neighborhood) {
    std::uint64_t first = 1469598103934665603ULL;
    std::uint64_t second = 0x9e3779b97f4a7c15ULL;
    for (const auto vertex : neighborhood) {
        first ^= static_cast<std::uint64_t>(vertex) + 1;
        first *= 1099511628211ULL;
        second ^= static_cast<std::uint64_t>(vertex) +
                  0x9e3779b97f4a7c15ULL + (second << 6U) + (second >> 2U);
    }
    return Signature{first, second, neighborhood.size()};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <index.fli>\n";
        return 2;
    }
    try {
        const auto index = hinscan::FactorIndex::load(argv[1]);
        std::unordered_map<Signature, std::uint64_t, SignatureHash> classes;
        std::uint64_t total_neighborhood_entries = 0;
        std::uint64_t representative_neighborhood_entries = 0;
        std::uint64_t posting_entries_read = 0;
        for (std::uint64_t vertex = 0; vertex < index.vertex_count(); ++vertex) {
            std::uint64_t read = 0;
            const auto neighborhood = index.collect_closed_neighborhood(
                static_cast<hinscan::VertexId>(vertex), &read);
            const auto inserted = classes.emplace(signature(neighborhood), 0);
            if (inserted.second) {
                representative_neighborhood_entries += neighborhood.size();
            }
            ++inserted.first->second;
            total_neighborhood_entries += neighborhood.size();
            posting_entries_read += read;
        }
        std::uint64_t singleton_classes = 0;
        std::uint64_t largest_class = 0;
        std::vector<std::uint64_t> sizes;
        sizes.reserve(classes.size());
        for (const auto& item : classes) {
            singleton_classes += item.second == 1 ? 1 : 0;
            largest_class = std::max(largest_class, item.second);
            sizes.push_back(item.second);
        }
        std::sort(sizes.begin(), sizes.end(), std::greater<std::uint64_t>());
        std::cout << "vertices=" << index.vertex_count() << '\n'
                  << "projected_edges=" << index.stats().exact_projected_edges
                  << '\n'
                  << "equivalence_classes=" << classes.size() << '\n'
                  << "singleton_classes=" << singleton_classes << '\n'
                  << "largest_class=" << largest_class << '\n'
                  << "total_closed_neighborhood_entries="
                  << total_neighborhood_entries << '\n'
                  << "representative_neighborhood_entries="
                  << representative_neighborhood_entries << '\n'
                  << "posting_entries_read=" << posting_entries_read << '\n'
                  << "top_class_sizes=";
        for (std::size_t i = 0; i < std::min<std::size_t>(10, sizes.size()); ++i) {
            if (i != 0) { std::cout << ','; }
            std::cout << sizes[i];
        }
        std::cout << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fli_equivalence_stats: " << error.what() << '\n';
        return 1;
    }
}
