#!/usr/bin/env bash
# Stage 7: GPU run-mesh reference and previous stable ABI contracts.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
python3 "$repo/tools/tests/test_vulkan_gpu_rows.py"
bash "$repo/tools/tests/run_vulkan_stage6_checks.sh"
bash "$repo/tools/tests/run_vulkan_stage5_mock_checks.sh"
bash "$repo/tools/tests/run_vulkan_catalog_checks.sh"
python3 "$repo/tools/tests/test_vulkan_013_contracts.py"
python3 "$repo/tools/tests/test_vulkan_013_stage3_contracts.py"
python3 "$repo/tools/tests/test_vulkan_013_stage4_asset_coverage.py"
