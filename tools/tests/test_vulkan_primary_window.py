"""Source contracts only, not a GLFW or Vulkan-driver test."""
from pathlib import Path
root=Path(__file__).resolve().parents[2]
read=lambda n:(root/n).read_text()
w=read('src/core/app/Window.cpp');g=read('src/core/game/Game.cpp');m=read('src/rendering/vulkan/VulkanGameMirror.cpp');r=read('src/rendering/vulkan/VulkanLiveWorldRenderer.cpp')
assert w.index('GLFW_NO_API')<w.index('GLFW_OPENGL_CORE_PROFILE')
assert 'glfwVulkanSupported()' in w and 'glfwHideWindow(m_window_ptr)' in w
assert 'glfwShowWindow(next)' in w and 'if (!enabled && m_vk_window)' in w
assert 'OPTICRAFT_RENDERER' in g and 'select_vulkan(selected_vulkan)' in g
assert 'switch_to_opengl();' in g and 'm_input_ptr->rebind_window' in g
assert 'm_vulkan_game_mirror->present();' in g
assert 'glfwCreateWindow' not in m and 'glfwDestroyWindow' not in m
assert r.index('uiPipeline_.prepare')<r.index('runtime.beginColorPass')<r.index('uiPipeline_.draw')<r.rindex('runtime.endColorPass')
assert 'chunk->get_dirty_epoch()' in read('src/rendering/vulkan/VulkanGameChunkFeed.cpp')
assert 'get_vulkan_resident_chunks()' in g
print('Vulkan primary window and UI integration source contracts: PASS')
