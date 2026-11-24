#!/usr/bin/env python3
"""Static assembly/ABI sanity checks. NOT a Vulkan driver or shader compiler test."""
from pathlib import Path
p = Path(__file__).resolve().parents[2]
assert (p/'VERSION').read_text().strip() == '0.13'
assert 'project(OptiCraft VERSION 0.13)' in (p/'CMakeLists.txt').read_text()
assert '## 0.13 — Vulkan render' in (p/'documentation/history_version.md').read_text()
common = (p/'assets/vulkan_shaders/mesher_common.glsl').read_text()
finalize = (p/'assets/vulkan_shaders/voxel_finalize.comp').read_text()
publish = (p/'assets/vulkan_shaders/voxel_publish.comp').read_text()
cull = (p/'assets/vulkan_shaders/voxel_cull.comp').read_text()
mesher = (p/'src/rendering/vulkan/VulkanGpuMesher.cpp').read_text()
parallel = (p/'src/rendering/vulkan/VulkanParallelDraw.cpp').read_text()
assert 'binding=7' in common and 'candidateDraws[]' in common
assert 'candidateDraws[batchIdx]' in finalize and 'draws[slotIndex] =' not in finalize
assert 'draws[slotIndex] = candidateDraws[batchIdx]' in publish
assert 'recordPublish' in mesher and 'maxJobs_' in mesher
assert 'visibleDraws[batch] = raw;' in cull
assert 'if (raw.y == 0u || !intersects(mn, mx)) raw.y = 0u;' in cull
assert 'VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT' in (p/'src/rendering/vulkan/VulkanGpuCuller.cpp').read_text()
assert 'vkCmdSetViewport' in parallel and 'vkCmdSetScissor' in parallel
assert 'vkCmdDrawIndirect' in parallel
assert 'vkCreateGraphicsPipelines' in (p/'src/rendering/vulkan/VulkanVoxelPipeline.cpp').read_text()
for shader in ['voxel_count.comp','voxel_emit.comp','voxel_finalize.comp','voxel_publish.comp','voxel_cull.comp','voxel_debug.vert','voxel_debug.frag']:
    assert shader in (p/'CMakeLists.txt').read_text()
print('OptiCraft 0.13 Vulkan Stage 2 static ABI/source contracts: PASS (not GPU-tested)')
