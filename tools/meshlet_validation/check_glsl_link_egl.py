#!/usr/bin/env python3
"""Compile/link the terrain shader pair and compile the GPU compute meshers on EGL."""
import runpy

# Reuse the small EGL/OpenGL ctypes bootstrap and compute test harness. The latter also
# creates the surfaceless context and leaves its GL function table available here.
ns = runpy.run_path('tools/meshlet_validation/test_meshlet_compute.py')
C = ns['C']; G = ns['G']; i = ns['i']; u = ns['u']
create_shader = ns['create_shader']; shader_source = ns['shader_source']
compile_shader = ns['compile_shader']; get_shader_iv = ns['get_shader_iv']
get_shader_log = ns['get_shader_log']; create_program = ns['create_program']
attach = ns['attach']; link = ns['link']; get_prog_iv = ns['get_prog_iv']
get_prog_log = ns['get_prog_log']

GL_COMPILE_STATUS = 0x8B81
GL_LINK_STATUS = 0x8B82


def compile_stage(path, stage):
    source = open(path, 'rb').read()
    shader = create_shader(stage)
    csource = C.c_char_p(source)
    shader_source(shader, 1, C.byref(csource), None)
    compile_shader(shader)
    ok = i()
    get_shader_iv(shader, GL_COMPILE_STATUS, C.byref(ok))
    if not ok.value:
        log = C.create_string_buffer(8192); length = i()
        get_shader_log(shader, len(log), C.byref(length), log)
        raise RuntimeError(f'{path}: {log.value.decode(errors="replace")}')
    print('COMPILE_PASS', path)
    return shader


def link_pair(vertex_path, fragment_path):
    vertex = compile_stage(vertex_path, 0x8B31)
    fragment = compile_stage(fragment_path, 0x8B30)
    program = create_program()
    attach(program, vertex); attach(program, fragment); link(program)
    ok = i(); get_prog_iv(program, GL_LINK_STATUS, C.byref(ok))
    if not ok.value:
        log = C.create_string_buffer(8192); length = i()
        get_prog_log(program, len(log), C.byref(length), log)
        raise RuntimeError(f'link {vertex_path} + {fragment_path}: {log.value.decode(errors="replace")}')
    print('LINK_PASS', vertex_path, '+', fragment_path)


for path in (
    'assets/shaders/mesh_greedy.comp',
    'assets/shaders/mesh_misc.comp',
    'assets/shaders/gpu_build_meshlets.comp',
    'assets/shaders/gpu_cull_meshlets.comp',
    'assets/shaders/gpu_cull_commands.comp',
    'assets/shaders/gpu_hiz_copy.comp',
    'assets/shaders/gpu_hiz_reduce.comp',
):
    compile_stage(path, 0x91B9)
link_pair('assets/shaders/terrain_gpu_vertex.glsl', 'assets/shaders/terrain_gpu_fragment.glsl')
link_pair('assets/shaders/terrain_gpu_misc_vertex.glsl', 'assets/shaders/terrain_gpu_misc_fragment.glsl')
