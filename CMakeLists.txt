cmake_minimum_required(VERSION 3.20)

project(hinscan_index_research LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_PSCAN_BASELINE "Build the unmodified upstream pSCAN baseline" ON)

add_library(hin_io
    src/hin/HinGraph.cpp
)
target_include_directories(hin_io PUBLIC src)

if(MSVC)
    target_compile_options(hin_io PRIVATE /O2 /W4)
else()
    target_compile_options(hin_io PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()

add_library(fli_index
    src/index/BudgetedEquivalenceIndex.cpp
    src/index/BudgetedSimilarityIndex.cpp
    src/index/FactorIndex.cpp
    src/index/FingerprintNeighborhoodIndex.cpp
    src/index/UniversalPathIndex.cpp
    src/index/EquivalenceIndex.cpp
    src/index/SimilarityCertificateIndex.cpp
    src/index/SelectiveEquivalenceIndex.cpp
)
target_include_directories(fli_index PUBLIC src)
target_link_libraries(fli_index PUBLIC hin_io)

if(MSVC)
    target_compile_options(fli_index PRIVATE /O2 /W4)
else()
    target_compile_options(fli_index PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()

add_library(fli_scan_core
    src/scan/FliScan.cpp
)
target_include_directories(fli_scan_core PUBLIC src)
target_link_libraries(fli_scan_core PUBLIC fli_index)

if(MSVC)
    target_compile_options(fli_scan_core PRIVATE /O2 /W4)
else()
    target_compile_options(fli_scan_core PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()

add_library(pscan_on_fli_core
    src/scan/PscanOnFli.cpp
    src/scan/ExactNeighborhoodCache.cpp
    src/scan/RegionCompletion.cpp
)
target_include_directories(pscan_on_fli_core PUBLIC src)
target_link_libraries(pscan_on_fli_core PUBLIC fli_index)

if(MSVC)
    target_compile_options(pscan_on_fli_core PRIVATE /O2 /W4)
else()
    target_compile_options(pscan_on_fli_core PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()

add_executable(hin_materialize
    src/tools/hin_materialize.cpp
)

# Diagnostic-only build. Release query targets do not compile the timers in.
add_library(pscan_profile_core src/scan/PscanOnFli.cpp src/scan/ExactNeighborhoodCache.cpp)
target_include_directories(pscan_profile_core PUBLIC src)
target_link_libraries(pscan_profile_core PUBLIC fli_index)
target_compile_definitions(pscan_profile_core PUBLIC HINSCAN_QUERY_PROFILE=1)
if(MSVC)
    target_compile_options(pscan_profile_core PRIVATE /O2 /W4)
else()
    target_compile_options(pscan_profile_core PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()
add_executable(bri_profile_v14 src/tools/bri_query_index.cpp)
target_compile_definitions(bri_profile_v14 PRIVATE HINSCAN_BLOCK_MODE=6)
target_link_libraries(bri_profile_v14 PRIVATE pscan_profile_core)
set_target_properties(bri_profile_v14 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_library(pscan_profile_control_core src/scan/PscanOnFli.cpp src/scan/ExactNeighborhoodCache.cpp)
target_include_directories(pscan_profile_control_core PUBLIC src)
target_link_libraries(pscan_profile_control_core PUBLIC fli_index)
target_compile_definitions(pscan_profile_control_core PUBLIC HINSCAN_QUERY_PROFILE=2)
if(MSVC)
    target_compile_options(pscan_profile_control_core PRIVATE /O2 /W4)
else()
    target_compile_options(pscan_profile_control_core PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()
add_executable(bri_profile_control_v14 src/tools/bri_query_index.cpp)
target_compile_definitions(bri_profile_control_v14 PRIVATE HINSCAN_BLOCK_MODE=6)
target_link_libraries(bri_profile_control_v14 PRIVATE pscan_profile_control_core)
set_target_properties(bri_profile_control_v14 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries(hin_materialize PRIVATE hin_io)
set_target_properties(hin_materialize PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(bri_build_index
    src/tools/bri_build_index.cpp
)
add_executable(bri_query_index src/tools/bri_query_index.cpp)
add_executable(bri_build_measured src/tools/bri_build_index.cpp)
target_compile_definitions(bri_build_measured PRIVATE HINSCAN_MEASURE_BUILD=1)
target_link_libraries(bri_build_measured PRIVATE hin_io)
set_target_properties(bri_build_measured PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_executable(bri_build_core_index src/tools/bri_build_index.cpp)
target_compile_definitions(bri_build_core_index PRIVATE HINSCAN_BUILD_ROUNDTRIP=1)
target_link_libraries(bri_build_core_index PRIVATE hin_io)
add_executable(bri_query_core_connectivity src/tools/bri_query_index.cpp)
target_compile_definitions(bri_query_core_connectivity PRIVATE HINSCAN_BLOCK_MODE=9)
target_link_libraries(bri_query_core_connectivity PRIVATE pscan_on_fli_core)
add_executable(bri_query_regions src/tools/bri_query_regions.cpp)
add_executable(bri_query_core_single_pass src/tools/bri_query_index.cpp)
add_executable(bri_query_core_lean src/tools/bri_query_index.cpp)
target_compile_definitions(bri_query_core_lean PRIVATE HINSCAN_BLOCK_MODE=11)
target_link_libraries(bri_query_core_lean PRIVATE pscan_on_fli_core)
set_target_properties(bri_query_core_lean PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_compile_definitions(bri_query_core_single_pass PRIVATE HINSCAN_BLOCK_MODE=10)
target_link_libraries(bri_query_core_single_pass PRIVATE pscan_on_fli_core)
set_target_properties(bri_query_core_single_pass PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries(bri_query_regions PRIVATE pscan_on_fli_core)
set_target_properties(bri_query_regions PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(group IN ITEMS 1 2)
    add_executable(bri_profile_core_p${group} src/tools/bri_query_index.cpp)
    target_compile_definitions(bri_profile_core_p${group} PRIVATE HINSCAN_BLOCK_MODE=9)
    if(group EQUAL 1)
        target_link_libraries(bri_profile_core_p${group} PRIVATE pscan_profile_core)
    else()
        target_link_libraries(bri_profile_core_p${group} PRIVATE pscan_profile_control_core)
    endif()
    set_target_properties(bri_profile_core_p${group} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()
set_target_properties(bri_build_core_index bri_query_core_connectivity
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(block_variant IN ITEMS bri_query_block_seed bri_query_blocks bri_query_witness_bounds bri_query_adaptive_bounds bri_query_witness_bitmaps bri_query_witness_exclusion)
    add_executable(${block_variant} src/tools/bri_query_index.cpp)
    target_link_libraries(${block_variant} PRIVATE pscan_on_fli_core)
    set_target_properties(${block_variant} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endforeach()
target_compile_definitions(bri_query_block_seed PRIVATE HINSCAN_BLOCK_MODE=1)
target_compile_definitions(bri_query_blocks PRIVATE HINSCAN_BLOCK_MODE=2)
target_compile_definitions(bri_query_witness_bounds PRIVATE HINSCAN_BLOCK_MODE=3)
target_compile_definitions(bri_query_adaptive_bounds PRIVATE HINSCAN_BLOCK_MODE=4)
target_compile_definitions(bri_query_witness_bitmaps PRIVATE HINSCAN_BLOCK_MODE=5)
target_compile_definitions(bri_query_witness_exclusion PRIVATE HINSCAN_BLOCK_MODE=6)
# Corrected experiments: all three policies retain lazy neighborhood activation.
# The old bri_ablation_* targets below are retained only for reproducibility.
foreach(cache_mib IN ITEMS 32 256 1024)
    foreach(policy IN ITEMS pressure off always)
        if(policy STREQUAL "pressure")
            set(policy_mode 6)
        elseif(policy STREQUAL "off")
            set(policy_mode 7)
        else()
            set(policy_mode 8)
        endif()
        foreach(profile_group IN ITEMS 0 1 2)
            set(target bri_isolated_${policy}_${cache_mib}_p${profile_group})
            add_executable(${target} src/tools/bri_query_index.cpp)
            target_compile_definitions(${target} PRIVATE HINSCAN_BLOCK_MODE=${policy_mode} HINSCAN_CACHE_MIB=${cache_mib})
            if(profile_group EQUAL 0)
                target_link_libraries(${target} PRIVATE pscan_on_fli_core)
            elseif(profile_group EQUAL 1)
                target_link_libraries(${target} PRIVATE pscan_profile_core)
            else()
                target_link_libraries(${target} PRIVATE pscan_profile_control_core)
            endif()
            set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        endforeach()
    endforeach()
endforeach()
# Experiment-only executables; normal query inputs and defaults stay unchanged.
foreach(cache_mib IN ITEMS 32 256 1024)
    foreach(bounds IN ITEMS on off)
        set(ablation_target bri_ablation_${bounds}_${cache_mib})
        add_executable(${ablation_target} src/tools/bri_query_index.cpp)
        if(bounds STREQUAL "on")
            target_compile_definitions(${ablation_target} PRIVATE HINSCAN_BLOCK_MODE=6)
        else()
            target_compile_definitions(${ablation_target} PRIVATE HINSCAN_BLOCK_MODE=2)
        endif()
        target_compile_definitions(${ablation_target} PRIVATE HINSCAN_CACHE_MIB=${cache_mib})
        target_link_libraries(${ablation_target} PRIVATE pscan_on_fli_core)
        set_target_properties(${ablation_target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    endforeach()
endforeach()
target_link_libraries(bri_query_index PRIVATE pscan_on_fli_core)
set_target_properties(bri_query_index PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
target_link_libraries(bri_build_index PRIVATE hin_io)
set_target_properties(bri_build_index PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(fli_verify
    src/tools/fli_verify.cpp
)
target_link_libraries(fli_verify PRIVATE fli_index)
set_target_properties(fli_verify PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(fli_scan
    src/tools/fli_scan.cpp
)
target_link_libraries(fli_scan PRIVATE fli_scan_core)
set_target_properties(fli_scan PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(fli_build_index
    src/tools/fli_build_index.cpp
)
target_link_libraries(fli_build_index PRIVATE fli_index)
set_target_properties(fli_build_index PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(fni_build_index
    src/tools/fni_build_index.cpp
)

add_executable(bsi_build_index
    src/tools/bsi_build_index.cpp
)

add_executable(beqi_build_index
    src/tools/beqi_build_index.cpp
)
target_link_libraries(beqi_build_index PRIVATE pscan_on_fli_core)
set_target_properties(beqi_build_index PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(pscan_on_beqi
    src/tools/pscan_on_beqi.cpp
)
target_link_libraries(pscan_on_beqi PRIVATE pscan_on_fli_core)
set_target_properties(pscan_on_beqi PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
target_link_libraries(bsi_build_index PRIVATE fli_index)
set_target_properties(bsi_build_index PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
target_link_libraries(fni_build_index PRIVATE fli_index)
set_target_properties(fni_build_index PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(fli_query_index
    src/tools/fli_query_index.cpp
)

add_executable(pscan_on_fli
    src/tools/pscan_on_fli.cpp
)
target_link_libraries(pscan_on_fli PRIVATE pscan_on_fli_core)
set_target_properties(pscan_on_fli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(verify_adaptive_fli src/tools/verify_adaptive_fli.cpp)
target_link_libraries(verify_adaptive_fli PRIVATE pscan_on_fli_core)
set_target_properties(verify_adaptive_fli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
target_link_libraries(fli_query_index PRIVATE fli_scan_core)
set_target_properties(fli_query_index PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(upi_build_index
    src/tools/upi_build_index.cpp
)
target_link_libraries(upi_build_index PRIVATE fli_index)
set_target_properties(upi_build_index PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(upi_query_index
    src/tools/upi_query_index.cpp
)
target_link_libraries(upi_query_index PRIVATE pscan_on_fli_core)
set_target_properties(upi_query_index PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(upi_promote_path
    src/tools/upi_promote_path.cpp
)
target_link_libraries(upi_promote_path PRIVATE pscan_on_fli_core)
set_target_properties(upi_promote_path PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(fli_equivalence_stats
    src/tools/fli_equivalence_stats.cpp
)

add_executable(eqi_build_index
    src/tools/eqi_build_index.cpp
)
target_link_libraries(eqi_build_index PRIVATE pscan_on_fli_core)
set_target_properties(eqi_build_index PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(pscan_on_eqi
    src/tools/pscan_on_eqi.cpp
)

add_executable(eqi_verify_vertex
    src/tools/eqi_verify_vertex.cpp
)

add_executable(sci_build_index
    src/tools/sci_build_index.cpp
)
target_link_libraries(sci_build_index PRIVATE pscan_on_fli_core)
set_target_properties(sci_build_index PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(pscan_on_sci
    src/tools/pscan_on_sci.cpp
)
target_link_libraries(pscan_on_sci PRIVATE pscan_on_fli_core)
set_target_properties(pscan_on_sci PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(sci_cost_probe
    src/tools/sci_cost_probe.cpp
)

add_executable(seci_build_index
    src/tools/seci_build_index.cpp
)
target_link_libraries(seci_build_index PRIVATE pscan_on_fli_core)
set_target_properties(seci_build_index PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(pscan_on_seci
    src/tools/pscan_on_seci.cpp
)
target_link_libraries(pscan_on_seci PRIVATE pscan_on_fli_core)
set_target_properties(pscan_on_seci PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
set_target_properties(sci_cost_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
target_link_libraries(eqi_verify_vertex PRIVATE pscan_on_fli_core)
set_target_properties(eqi_verify_vertex PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
target_link_libraries(pscan_on_eqi PRIVATE pscan_on_fli_core)
set_target_properties(pscan_on_eqi PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
target_link_libraries(fli_equivalence_stats PRIVATE fli_index)
set_target_properties(fli_equivalence_stats PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(BUILD_PSCAN_BASELINE)
    add_executable(pscan_baseline
        third_party/pscan/main.cpp
        third_party/pscan/Graph.cpp
        third_party/pscan/Utility.cpp
    )

    target_include_directories(pscan_baseline PRIVATE third_party/pscan)

    if(MSVC)
        target_compile_options(pscan_baseline PRIVATE /O2 /W3)
    else()
        target_compile_options(pscan_baseline PRIVATE -O3 -Wall -Wextra)
    endif()

    set_target_properties(pscan_baseline PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()
