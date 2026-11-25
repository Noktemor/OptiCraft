# Standalone core tests: no GLFW/OpenGL/Vulkan SDK required.
cmake_minimum_required(VERSION 3.16)
project(OptiCraftCoreTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Threads REQUIRED)
enable_testing()
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
add_library(vkcore STATIC ${ROOT}/src/rendering/vulkan/GpuWorldCore.cpp
    ${ROOT}/src/rendering/vulkan/GpuFrustum.cpp ${ROOT}/src/rendering/vulkan/VulkanChunkStreamPlan.cpp
    ${ROOT}/src/rendering/vulkan/GpuMeshTransactions.cpp)
target_include_directories(vkcore PUBLIC ${ROOT}/src ${ROOT}/external/opensimplex)
function(oc_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE vkcore Threads::Threads)
    # These tests use assert for both setup and verification. Keep them active in Release.
    if(MSVC)
        target_compile_options(${name} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${name} PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()
oc_test(world_core test_vulkan_world_core.cpp)
oc_test(chunk_stream test_vulkan_chunk_stream.cpp)
oc_test(mesh_transactions test_vulkan_mesh_transactions.cpp)
oc_test(world_policies test_vulkan_stage75_77.cpp)
oc_test(entity_batch test_vulkan_stage78_79.cpp)
oc_test(column_batch test_vulkan_stage83_85.cpp)
oc_test(material_plan test_vulkan_stage122_125.cpp)

oc_test(owned_defaults_overlay test_vulkan_stage133.cpp)

oc_test(memory_ledger test_vulkan_stage139_memory.cpp)
target_sources(memory_ledger PRIVATE ${ROOT}/src/rendering/vulkan/VulkanDeviceBuffer.cpp)
target_include_directories(memory_ledger PRIVATE ${CMAKE_CURRENT_LIST_DIR}/fake_vulkan)

oc_test(preview_invalidation test_vulkan_stage140_preview.cpp)
