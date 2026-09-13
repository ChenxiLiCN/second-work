#include "index/FactorIndex.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
}
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::invalid_argument("provide a new tiny fixture directory");
        const std::filesystem::path dir=argv[1];
        if(!std::filesystem::create_directory(dir)) throw std::runtime_error("fixture already exists");
        std::filesystem::create_directory(dir/"edge");
        { std::ofstream f(dir/"base.txt"); f << "2\nA 5\nB 3\n1\n0 1 6\n"; }
        { std::ofstream f(dir/"edge/0.txt"); f << "0 1 6\n0 0\n1 0\n1 1\n2 1\n3 2\n4 2\n"; }
        auto graph=hinscan::HinGraph::load(dir);
        graph.prepare_roundtrip_metadata();
        const std::vector<hinscan::VertexId> chosen{0,2,4};
        for(const auto& path: {std::vector<std::uint32_t>{0,1,0},std::vector<std::uint32_t>{0,1,0,1,0}}) {
            const auto full=hinscan::FactorIndex::build(graph,path);
            const auto part=hinscan::FactorIndex::build_selected(graph,path,chosen);
            check(part.vertex_count()==3,"subset source count");
            for(std::size_t i=0;i<chosen.size();++i) {
                const auto old=full.collect_closed_neighborhood(chosen[i]);
                std::vector<hinscan::VertexId> expected;
                for(std::size_t j=0;j<chosen.size();++j)
                    if(std::find(old.begin(),old.end(),chosen[j])!=old.end()) expected.push_back(j);
                check(part.collect_closed_neighborhood(i)==expected,"subset changes intermediate reachability");
            }
            const auto empty=hinscan::FactorIndex::build_selected(graph,path,{});
            check(empty.vertex_count()==0 && empty.stats().half_expansion_entries==0,"empty expands sources");
            if(path.size()==5) {
                check(part.stats().half_expansion_entries<full.stats().half_expansion_entries,"did not skip removed sources");
                check(part.collect_closed_neighborhood(0)==std::vector<hinscan::VertexId>({0,1}),"removed intermediate was pruned");
            }
        }
        for(const auto& bad:{std::vector<hinscan::VertexId>{2,0},std::vector<hinscan::VertexId>{0,0},std::vector<hinscan::VertexId>{5}}) {
            bool threw=false;
            try { hinscan::FactorIndex::build_selected(graph,{0,1,0},bad); }
            catch(const std::invalid_argument&) { threw=true; }
            check(threw,"invalid source selection accepted");
        }
        const auto all=hinscan::FactorIndex::build_selected(graph,{0,1,0},{0,1,2,3,4});
        check(all.stats().used_roundtrip_metadata,"full selection lost metadata fast path");
        std::cout << "all_passed=1\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
