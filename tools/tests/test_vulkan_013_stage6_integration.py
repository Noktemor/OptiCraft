#!/usr/bin/env python3
"""Stage 6 source/binding contracts. NOT a Vulkan-driver or visual test."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
read = lambda rel: (root / rel).read_text()
cmake = read('CMakeLists.txt')
game = read('src/core/game/Game.cpp')
mirror = read('src/rendering/vulkan/VulkanGameMirror.cpp')
frame = read('src/rendering/vulkan/VulkanLiveWorldRenderer.cpp')
mesh = read('src/rendering/vulkan/VulkanChunkMeshing.cpp')
feed = read('src/rendering/vulkan/VulkanGameChunkFeed.cpp')
assert 'OPTICRAFT_VULKAN_GAME_MIRROR=1' in cmake
assert 'OptiCraft_VulkanBackend' in cmake
assert 'VulkanGameMirror.cpp' in cmake
assert 'm_vulkan_game_mirror->initialize(m_window_ptr->vulkan_candidate(), Block_Registry::get_instance())' in game
assert 'm_vulkan_chunk_feed.acceptFrame(renderable, *m_chunk_manager_ptr)' in game
assert 'm_vulkan_game_mirror->setWorld(projection * view,sky_color)' in game
assert 'set_vulkan_chunk_sink({}, {});' in game
window = read('src/core/app/Window.cpp')
assert 'glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API)' in window
assert 'glfwDestroyWindow' not in mirror
assert 'fromVulkanViewProjection' in mirror
assert 'glfwPollEvents(' not in mirror  # Game already polls events
assert 'vkDeviceWaitIdle(' not in frame and 'vkQueueWaitIdle(' not in frame
for part in ('meshing_.beginFrame', 'stream_.record', 'meshing_.record',
             'f.opaqueCuller.record', 'f.miscCuller.record',
             'runtime.beginColorPass', 'opaquePipeline_.pipeline()',
             'miscPipeline_.recordDraws'):
    assert part in frame, part
assert frame.index('stream_.record') < frame.index('meshing_.record')
assert frame.index('meshing_.record') < frame.index('f.opaqueCuller.record')
assert frame.index('f.miscCuller.record') < frame.index('runtime.beginColorPass')
assert frame.index('runtime.beginColorPass') < frame.index('miscPipeline_.recordDraws')
assert 'vkCmdCopyBuffer' in mesh and 'recordPublish' in mesh
assert 'std::nth_element' in feed and 'trackedBudget_' in feed
assert 'std::sort(selected.begin()' in feed
assert 'void VulkanGameChunkFeed::clear' in feed
for s in ('voxel_count.comp', 'voxel_emit.comp', 'voxel_finalize.comp',
          'voxel_publish.comp', 'voxel_misc_count.comp', 'voxel_misc_emit.comp',
          'voxel_textured.vert', 'voxel_textured.frag', 'voxel_misc.vert',
          'voxel_misc.frag', 'voxel_cull.comp', 'voxel_misc_cull.comp'):
    assert (root / 'assets/vulkan_shaders' / s).is_file(), s
    assert s in cmake, f'{s} absent from shader compilation'
assert 'VisualBlended' in read('src/rendering/vulkan/VulkanBlockCatalog.h')
assert 'toBlockFace' in read('assets/vulkan_shaders/voxel_textured.vert')
assert 'ALPHA_PASS' in read('assets/vulkan_shaders/voxel_misc.frag')
assert 'depth.depthWriteEnable = VK_FALSE' in read('src/rendering/vulkan/VulkanMiscPipeline.cpp')
print('OptiCraft 0.13 Stage 6 source/frame integration contracts: PASS (not GPU-tested)')
