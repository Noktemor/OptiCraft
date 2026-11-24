#!/usr/bin/env python3
"""Regression checks for GPU submitted-triangle statistics.

The counter measures submitted primitives (NOT visible pixels). Meshlet mode must
count meshlets that survive culling, not entire sections. Static safety checks
plus headless GL shader compilation when EGL/Mesa is available.
"""
import ctypes as C
from pathlib import Path

root = Path(__file__).resolve().parents[2]
legacy = (root / 'assets/shaders/gpu_cull_commands.comp').read_text()
meshlet = (root / 'assets/shaders/gpu_cull_meshlets.comp').read_text()
renderer = (root / 'src/rendering/Renderer.cpp').read_text()
hud = (root / 'src/hud/Hud.cpp').read_text()

assert 'Submitted tris:' in hud, 'HUD should not call submitted triangles visible primitives'
assert 'sections_occluded' in hud
assert 'if (u_meshlet_enabled == 0 || kind == 2u)' in legacy
assert 'if (visible) atomicAdd(stats[u_stats_offset + 1u], quad_count * 2u);' in meshlet
assert 'glUniform1ui(m_gpu_loc_meshlet_stats_offset, use_hiz ? 5u : 0u)' in renderer
assert renderer.index('const uint32_t zero_stats[10] = {};') < renderer.index('prepare_gpu_meshlet_frame(opaque_order, view_projection, use_hiz, spectator);')

# Compile both modified shaders with actual OpenGL 4.3 drivers, not regex alone.
try:
    E=C.CDLL('libEGL.so.1'); G=C.CDLL('libGL.so.1')
    E.eglGetProcAddress.restype=C.c_void_p
    E.eglGetProcAddress.argtypes=[C.c_char_p]
    E.eglInitialize.restype=C.c_uint
    E.eglInitialize.argtypes=[C.c_void_p,C.POINTER(C.c_int),C.POINTER(C.c_int)]
    E.eglBindAPI.restype=C.c_uint
    E.eglBindAPI.argtypes=[C.c_uint]
    E.eglChooseConfig.restype=C.c_uint
    E.eglChooseConfig.argtypes=[C.c_void_p,C.POINTER(C.c_int),C.POINTER(C.c_void_p),C.c_int,C.POINTER(C.c_int)]
    E.eglCreatePbufferSurface.restype=C.c_void_p
    E.eglCreatePbufferSurface.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(C.c_int)]
    E.eglCreateContext.restype=C.c_void_p
    E.eglCreateContext.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.POINTER(C.c_int)]
    E.eglMakeCurrent.restype=C.c_uint
    E.eglMakeCurrent.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p]
    ptr=E.eglGetProcAddress(b'eglGetPlatformDisplayEXT')
    display=C.CFUNCTYPE(C.c_void_p,C.c_uint,C.c_void_p,C.POINTER(C.c_int))(ptr)(0x31DD,None,None)
    major=C.c_int(); minor=C.c_int()
    assert E.eglInitialize(display,C.byref(major),C.byref(minor))
    assert E.eglBindAPI(0x30A2)
    attrs=(C.c_int*7)(0x3033,1,0x3040,8,0x3024,8,0x3038)
    cfg=C.c_void_p(); num=C.c_int()
    assert E.eglChooseConfig(display,attrs,C.byref(cfg),1,C.byref(num)) and num.value
    surf=E.eglCreatePbufferSurface(display,cfg,(C.c_int*5)(0x3057,1,0x3056,1,0x3038))
    ctx=E.eglCreateContext(display,cfg,None,(C.c_int*7)(0x3098,4,0x30FB,3,0x30FD,1,0x3038))
    assert ctx and E.eglMakeCurrent(display,surf,surf,ctx)
    u=C.c_uint; i=C.c_int
    def gl(name,restype,argtypes):
        fn=getattr(G,name);fn.restype=restype;fn.argtypes=argtypes;return fn
    create=gl('glCreateShader',u,[u]);source=gl('glShaderSource',None,[u,i,C.POINTER(C.c_char_p),C.POINTER(i)])
    compile_=gl('glCompileShader',None,[u]);status=gl('glGetShaderiv',None,[u,u,C.POINTER(i)])
    getlog=gl('glGetShaderInfoLog',None,[u,i,C.POINTER(i),C.c_void_p]);free=gl('glDeleteShader',None,[u])
    for name in ['gpu_cull_commands.comp','gpu_cull_meshlets.comp']:
        code=(root/'assets/shaders'/name).read_bytes();p=C.c_char_p(code)
        s=create(0x91B9);source(s,1,C.byref(p),None);compile_(s);ok=i();status(s,0x8B81,C.byref(ok))
        if not ok.value:
            buf=C.create_string_buffer(4096); getlog(s,len(buf),None,buf)
            raise AssertionError(f'{name} failed to compile: {buf.value.decode(errors="replace")}')
        free(s)
    print('Triangle stats contracts + OpenGL 4.3 shader compilation: PASS')
except OSError as exc:
    print('Triangle stats source contracts: PASS (headless GL unavailable:',exc,')')
