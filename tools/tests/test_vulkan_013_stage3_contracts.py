#!/usr/bin/env python3
"""Stage 3 source/lifecycle assertions. Does not claim driver or SPIR-V validation."""
from pathlib import Path
root = Path(__file__).resolve().parents[2]
cmake = (root / 'CMakeLists.txt').read_text()
header = (root / 'src/rendering/vulkan/VulkanFrameRuntime.h').read_text()
cpp = (root / 'src/rendering/vulkan/VulkanFrameRuntime.cpp').read_text()
probe = (root / 'src/rendering/vulkan/VulkanProbe.cpp').read_text()
assert 'project(OptiCraft VERSION 0.13)' in cmake
assert 'VulkanFrameRuntime.cpp' in cmake and 'add_executable(OptiCraft_Vulkan' in cmake
assert 'add_library(OptiCraft_VulkanWindow STATIC' in cmake
assert 'target_link_libraries(OptiCraft_Vulkan PRIVATE OptiCraft_VulkanWindow)' in cmake
assert 'GLFW_NO_API' in probe and 'renderer.beginFrame(frame)' in probe
assert 'renderer.endFrame(frame)' in probe
assert 'vkAcquireNextImageKHR' in cpp and 'vkQueueSubmit' in cpp and 'vkQueuePresentKHR' in cpp
assert 'vkWaitForFences' in cpp and 'vkResetCommandPool' in cpp
assert cpp.index('vkWaitForFences(device_, 1, &sync.completed') < cpp.index('vkResetCommandPool(device_, sync.commandPool')
assert cpp.index('vkEndCommandBuffer(frame.cmd)') < cpp.index('vkResetFences(device_, 1, &sync.completed)')
assert cpp.index('vkQueueSubmit(graphicsQueue_') < cpp.index('vkQueuePresentKHR(graphicsQueue_')
assert 'std::vector<VkSemaphore> presentReady_' in header  # WSI lifetime differs from frame fence
assert 'std::vector<VkFence> imageInFlight_' in header
assert 'VK_ERROR_OUT_OF_DATE_KHR' in cpp and 'VK_SUBOPTIMAL_KHR' in cpp
assert 'VK_IMAGE_LAYOUT_PRESENT_SRC_KHR' in cpp
assert 'VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT' in cpp
assert 'VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT' in cpp
assert 'VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS' in cpp
assert 'vkDeviceWaitIdle(device_)' in cpp and 'vkDeviceWaitIdle(resize)' in cpp
assert 'vkCmdDraw' not in probe and 'glDraw' not in probe  # no fake CPU mesh
assert '--integrated' in probe and '--validation' in probe and '--frames' in probe
assert '0.13 — Vulkan render' in (root / 'documentation/history_version.md').read_text()
print('OptiCraft 0.13 Vulkan Stage 3 static frame/WSI contracts: PASS (not GPU-tested)')
