#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
testbin="$(mktemp /tmp/opticraft-vkcore-XXXXXX)"
trap 'rm -f "$testbin"' EXIT
"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Werror -pthread -I"$repo/src" \
  "$repo/src/rendering/vulkan/GpuWorldCore.cpp" \
  "$repo/src/rendering/vulkan/GpuFrustum.cpp" \
  "$repo/tools/tests/test_vulkan_world_core.cpp" \
  -o "$testbin"
"$testbin"
"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Werror -pthread -I"$repo/src" \
  "$repo/src/rendering/vulkan/GpuWorldCore.cpp" \
  "$repo/src/rendering/vulkan/VulkanChunkStreamPlan.cpp" \
  "$repo/tools/tests/test_vulkan_chunk_stream.cpp" \
  -o "$testbin"
"$testbin"
