#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
testbin="$(mktemp /tmp/opticraft-vkcatalog-XXXXXX)"
trap 'rm -f "$testbin"' EXIT
"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Werror \
  -I"$repo/tools/tests/vulkan_catalog_smoke" -I"$repo/src" \
  "$repo/src/rendering/vulkan/VulkanBlockCatalog.cpp" \
  "$repo/tools/tests/test_vulkan_block_catalog.cpp" -o "$testbin"
"$testbin"
