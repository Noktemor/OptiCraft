#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
testbin="$(mktemp /tmp/opticraft-vkstage5-XXXXXX)"
trap 'rm -f "$testbin"' EXIT
"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Werror -Wno-unused-function -pthread \
  -I"$repo/tools/tests/fake_vulkan" -I"$repo/src" -I"$repo/external/stb" -I"$repo/external/opensimplex" \
  "$repo/src/rendering/vulkan/GpuWorldCore.cpp" \
  "$repo/src/rendering/vulkan/VulkanChunkStreamPlan.cpp" \
  "$repo/src/rendering/vulkan/VulkanDeviceBuffer.cpp" \
  "$repo/src/rendering/vulkan/GpuFrustum.cpp" \
  "$repo/src/rendering/vulkan/VulkanColumnMaterialRuntime.cpp" \
  "$repo/src/rendering/vulkan/VulkanEntityPipeline.cpp" \
  "$repo/src/rendering/vulkan/VulkanItemPipeline.cpp" \
  "$repo/src/rendering/vulkan/VulkanChunkStreamGpu.cpp" \
  "$repo/src/rendering/vulkan/GpuMeshTransactions.cpp" \
  "$repo/src/rendering/vulkan/VulkanChunkMeshing.cpp" \
  "$repo/src/rendering/vulkan/VulkanGpuCuller.cpp" \
  "$repo/src/rendering/vulkan/VulkanGpuMesher.cpp" \
  "$repo/src/rendering/vulkan/VulkanResidentLight.cpp" \
  "$repo/src/rendering/vulkan/VulkanMiscMesher.cpp" \
  "$repo/src/rendering/vulkan/VulkanOpaqueMaterialPipeline.cpp" \
  "$repo/src/rendering/vulkan/VulkanMaterialTables.cpp" \
  "$repo/src/rendering/vulkan/VulkanBlockAtlas.cpp" \
  "$repo/tools/tests/test_vulkan_stage5_mock.cpp" -o "$testbin"
"$testbin" "$repo/assets/texture/texture_atlas.png"
