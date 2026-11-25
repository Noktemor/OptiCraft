#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
testbin="$(mktemp /tmp/opticraft-vkfeed-XXXXXX)"
trap 'rm -f "$testbin"' EXIT
"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Werror -pthread \
  -I"$repo/tools/tests/vulkan_feed_mock" -I"$repo/src" \
  "$repo/src/rendering/vulkan/GpuWorldCore.cpp" \
  "$repo/src/rendering/vulkan/VulkanGameChunkFeed.cpp" \
  "$repo/tools/tests/test_vulkan_chunk_feed.cpp" -o "$testbin"
OPTICRAFT_VULKAN_COMPACT_UPLOAD=1 "$testbin"
OPTICRAFT_VULKAN_COMPACT_UPLOAD=0 "$testbin"
