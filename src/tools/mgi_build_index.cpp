#include "index/MajorityIndex.h"
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv) {
    if(argc!=3) {std::cerr<<"Usage: mgi_build_index <hin-directory> <new-index-directory>\n";return 2;}
    try {
        using Clock=std::chrono::steady_clock;
        auto ms=[](Clock::duration d){return std::chrono::duration<double,std::milli>(d).count();};
        const std::filesystem::path dir=argv[2];
        if(std::filesystem::exists(dir)) throw std::runtime_error("index output directory already exists");
        double adjacency_ms=0;
        const auto start=Clock::now();
        auto graph=hinscan::HinGraph::load(argv[1],&adjacency_ms);
        const auto loaded=Clock::now();
        graph.prepare_roundtrip_metadata();
        const auto degrees=Clock::now();
        const auto index=hinscan::MajorityIndex::build(graph);
        const auto built=Clock::now();
        if(!std::filesystem::create_directories(dir)) throw std::runtime_error("index directory appeared during build");
        graph.save_binary(dir/"base.bri");index.save(dir/"groups.mgi",graph);
        const auto saved=Clock::now();
        std::cout<<std::setprecision(12)
            <<"algorithm=majority_group_index_v1\n"
            <<"offline_compute_ms="<<adjacency_ms+ms(built-loaded)<<'\n'
            <<"offline_timing_scope=adjacency_build_roundtrip_metadata_majority_groups_no_input_or_output\n"
            <<"raw_load_with_adjacency_ms="<<ms(loaded-start)<<'\n'
            <<"adjacency_build_ms="<<adjacency_ms<<'\n'
            <<"roundtrip_prepare_ms="<<ms(degrees-loaded)<<'\n'
            <<"majority_prepare_ms="<<ms(built-degrees)<<'\n'
            <<"index_write_ms="<<ms(saved-built)<<'\n'
            <<"index_bytes="<<std::filesystem::file_size(dir/"base.bri")+std::filesystem::file_size(dir/"groups.mgi")<<'\n'
            <<"base_bytes="<<std::filesystem::file_size(dir/"base.bri")<<'\n'
            <<"groups_bytes="<<std::filesystem::file_size(dir/"groups.mgi")<<'\n'
            <<"group_count="<<index.group_count()<<"\nmember_count="<<index.member_count()<<'\n';
        return 0;
    } catch(const std::exception& e){std::cerr<<"mgi_build_index: "<<e.what()<<'\n';return 1;}
}
