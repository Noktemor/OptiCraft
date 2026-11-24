#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
testbin="$(mktemp /tmp/opticraft-vkstage5-XXXXXX)"
trap 'rm -f "$testbin"' EXIT
"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Werror -Wno-unused-function -pthread \
  -I"$repo/tools/tests/fake_vulkan" -I"$repo/src" -I"$repo/external/stb" \
  "$repo/src/rendering/vulkan/GpuWorldCore.cpp" \
  "$repo/src/rendering/vulkan/VulkanChunkStreamPlan.cpp" \
  "$repo/src/rendering/vulkan/VulkanDeviceBuffer.cpp" \
  "$repo/src/rendering/vulkan/VulkanChunkStreamGpu.cpp" \
  "$repo/src/rendering/vulkan/VulkanMaterialTables.cpp" \
  "$repo/src/rendering/vulkan/VulkanBlockAtlas.cpp" \
  "$repo/tools/tests/test_vulkan_stage5_mock.cpp" -o "$testbin"
"$testbin" "$repo/assets/texture/texture_atlas.png"
