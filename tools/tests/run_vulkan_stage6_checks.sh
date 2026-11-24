#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Werror \
 -fsanitize=undefined -fno-sanitize-recover=all -I"$repo/src" \
 "$repo/src/rendering/vulkan/GpuWorldCore.cpp" \
 "$repo/src/rendering/vulkan/GpuMeshTransactions.cpp" \
 "$repo/tools/tests/test_vulkan_mesh_transactions.cpp" \
 -o /tmp/opticraft-vk-stage6-mesh-$$
trap 'rm -f /tmp/opticraft-vk-stage6-mesh-$$' EXIT
/tmp/opticraft-vk-stage6-mesh-$$
"${CXX:-g++}" -std=c++20 -fsyntax-only -Wall -Wextra -Werror \
 -I"$repo/tools/tests/fake_vulkan" -I"$repo/src" \
 "$repo/src/rendering/vulkan/VulkanChunkMeshing.cpp"
python3 "$repo/tools/tests/test_vulkan_013_stage6_integration.py"
