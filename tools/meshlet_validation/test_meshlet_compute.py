import ctypes as C, math
# Create a surfaceless EGL 4.3+ context (Mesa llvmpipe in this container).
E=C.CDLL('libEGL.so.1')
E.eglGetProcAddress.restype=C.c_void_p; E.eglGetProcAddress.argtypes=[C.c_char_p]
E.eglInitialize.restype=C.c_uint; E.eglInitialize.argtypes=[C.c_void_p,C.POINTER(C.c_int),C.POINTER(C.c_int)]
E.eglBindAPI.restype=C.c_uint; E.eglBindAPI.argtypes=[C.c_uint]
E.eglChooseConfig.restype=C.c_uint; E.eglChooseConfig.argtypes=[C.c_void_p,C.POINTER(C.c_int),C.POINTER(C.c_void_p),C.c_int,C.POINTER(C.c_int)]
E.eglCreatePbufferSurface.restype=C.c_void_p; E.eglCreatePbufferSurface.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(C.c_int)]
E.eglCreateContext.restype=C.c_void_p; E.eglCreateContext.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.POINTER(C.c_int)]
E.eglMakeCurrent.restype=C.c_uint; E.eglMakeCurrent.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p]
getplat=E.eglGetProcAddress(b'eglGetPlatformDisplayEXT')
GETPLAT=C.CFUNCTYPE(C.c_void_p,C.c_uint,C.c_void_p,C.POINTER(C.c_int))(getplat)
display=GETPLAT(0x31DD,None,None)
maj=C.c_int(); minr=C.c_int(); assert E.eglInitialize(display,C.byref(maj),C.byref(minr))
assert E.eglBindAPI(0x30A2)
attrs=(C.c_int*7)(0x3033,1,0x3040,8,0x3024,8,0x3038)
cfg=C.c_void_p(); n=C.c_int(); assert E.eglChooseConfig(display,attrs,C.byref(cfg),1,C.byref(n)) and n.value
pattrs=(C.c_int*5)(0x3057,1,0x3056,1,0x3038); surf=E.eglCreatePbufferSurface(display,cfg,pattrs)
ctxattrs=(C.c_int*7)(0x3098,4,0x30FB,3,0x30FD,1,0x3038); ctx=E.eglCreateContext(display,cfg,None,ctxattrs)
assert ctx and E.eglMakeCurrent(display,surf,surf,ctx)
G=C.CDLL('libGL.so.1')

def setup(name, restype, args):
    f=getattr(G,name); f.restype=restype; f.argtypes=args; return f
u=C.c_uint; i=C.c_int; sz=C.c_ssize_t; ptr=C.c_void_p
getstr=setup('glGetString',C.c_char_p,[u]); print('GL',getstr(0x1F02).decode())
create_shader=setup('glCreateShader',u,[u]); shader_source=setup('glShaderSource',None,[u,i,C.POINTER(C.c_char_p),C.POINTER(i)])
compile_shader=setup('glCompileShader',None,[u]); get_shader_iv=setup('glGetShaderiv',None,[u,u,C.POINTER(i)])
get_shader_log=setup('glGetShaderInfoLog',None,[u,i,C.POINTER(i),ptr]); create_program=setup('glCreateProgram',u,[])
attach=setup('glAttachShader',None,[u,u]); link=setup('glLinkProgram',None,[u]); get_prog_iv=setup('glGetProgramiv',None,[u,u,C.POINTER(i)])
get_prog_log=setup('glGetProgramInfoLog',None,[u,i,C.POINTER(i),ptr]); use=setup('glUseProgram',None,[u]);
genbuf=setup('glGenBuffers',None,[i,C.POINTER(u)]); bindbuf=setup('glBindBuffer',None,[u,u]); bufferdata=setup('glBufferData',None,[u,sz,ptr,u]); bindbase=setup('glBindBufferBase',None,[u,u,u]);
getuniform=setup('glGetUniformLocation',i,[u,C.c_char_p]); uniform1ui=setup('glUniform1ui',None,[i,u]); uniform1i=setup('glUniform1i',None,[i,i]);
uniform1f=setup('glUniform1f',None,[i,C.c_float]); uniform4fv=setup('glUniform4fv',None,[i,i,ptr]); uniformmat=setup('glUniformMatrix4fv',None,[i,i,u,ptr]); uniform2f=setup('glUniform2f',None,[i,C.c_float,C.c_float]);
gen_tex=setup('glGenTextures',None,[i,C.POINTER(u)]); active_tex=setup('glActiveTexture',None,[u]); bind_tex=setup('glBindTexture',None,[u,u]); tex_image=setup('glTexImage2D',None,[u,i,i,i,i,i,u,u,ptr]); tex_param=setup('glTexParameteri',None,[u,u,i]);
dispatch=setup('glDispatchCompute',None,[u,u,u]); barrier=setup('glMemoryBarrier',None,[u]); getsub=setup('glGetBufferSubData',None,[u,sz,sz,ptr]); geterr=setup('glGetError',u,[])
GL_SSBO=0x90D2; GL_DYNAMIC=0x88E8; GL_STATIC=0x88E4; GL_SHADER_STORAGE_BARRIER=0x2000; GL_COMMAND_BARRIER=0x40

def program(path):
    src=open(path,'rb').read(); sh=create_shader(0x91B9); csrc=C.c_char_p(src); shader_source(sh,1,C.byref(csrc),None); compile_shader(sh)
    ok=i(); get_shader_iv(sh,0x8B81,C.byref(ok))
    if not ok.value:
        log=C.create_string_buffer(8192); ln=i(); get_shader_log(sh,8192,C.byref(ln),log); raise RuntimeError(log.value.decode())
    pr=create_program(); attach(pr,sh); link(pr); get_prog_iv(pr,0x8B82,C.byref(ok))
    if not ok.value:
        log=C.create_string_buffer(8192); ln=i(); get_prog_log(pr,8192,C.byref(ln),log); raise RuntimeError(log.value.decode())
    return pr

def mkbuf(binding, data, nbytes=None):
    b=u(); genbuf(1,C.byref(b)); bindbuf(GL_SSBO,b)
    if isinstance(data,bytes): raw=data
    else: raw=bytes(data)
    bufferdata(GL_SSBO,len(raw) if nbytes is None else nbytes,raw if raw else None,GL_DYNAMIC)
    bindbase(GL_SSBO,binding,b); return b

def uvec4(*v): return C.string_at((C.c_uint*4)(*v),16)
def vec4f(*v): return C.string_at((C.c_float*4)(*v),16)
def getwords(b,n):
    bindbuf(GL_SSBO,b); out=(C.c_uint*n)(); getsub(GL_SSBO,0,C.sizeof(out),out); return list(out)

# Build three descriptors: solid, shaped and liquid. Each candidate maps to one GPU stream.
w0_solid=(2<<15)|(1<<17)  # axis=Z, positive, width=1, height=1, extent=1
w0_shape=1 | (2<<5) | (3<<10) | (4<<15)
z_shape=(32<<21)
w_shape=32 | (32<<7)
quads=[0]*(96*4)
quads[0:4]=[w0_solid,0,0,0]
quads[32*4:32*4+4]=[w0_shape,0,z_shape,w_shape]
quads[64*4:64*4+4]=[w0_solid,0,0,0]
counts=[0]*16; counts[0]=1; counts[4]=1; counts[12]=1
origins=[0.0,0.0,0.0,0.0]
candidates=[0,0,0,0, 0,4,32,0, 0,12,64,0]
b_candidates=mkbuf(0,C.string_at((C.c_uint*12)(*candidates),48))
b_counts=mkbuf(1,C.string_at((C.c_uint*16)(*counts),64))
b_quads=mkbuf(2,C.string_at((C.c_uint*len(quads))(*quads),len(quads)*4))
b_origins=mkbuf(3,C.string_at((C.c_float*4)(*origins),16))
b_bounds=mkbuf(4,bytes(144))
pr=program('assets/shaders/gpu_build_meshlets.comp'); use(pr)
def set1ui(name,v): uniform1ui(getuniform(pr,name.encode()),v)
set1ui('u_candidate_count',3)
for base in range(3):
    set1ui('u_candidate_base',base); dispatch(1,1,1)
barrier(GL_SHADER_STORAGE_BARRIER)
assert geterr()==0
raw=(C.c_float*36)(); bindbuf(GL_SSBO,b_bounds); getsub(GL_SSBO,0,C.sizeof(raw),raw)
meta=(C.c_uint*36)(); getsub(GL_SSBO,0,C.sizeof(meta),meta)
solid_min=tuple(raw[0:3]); solid_max=tuple(raw[4:7])
shaped_min=tuple(raw[12:15]); shaped_max=tuple(raw[16:19])
water_min=tuple(raw[24:27]); water_max=tuple(raw[28:31])
assert meta[8]==1 and meta[20]==1 and meta[32]==1, (meta[8],meta[20],meta[32])
assert abs(solid_min[2]-0.998)<1e-4 and abs(solid_max[2]-1.002)<1e-4, (solid_min,solid_max)
assert tuple(round(x,4) for x in shaped_min)==(1.0,2.0,3.0),shaped_min
assert tuple(round(x,4) for x in shaped_max)==(1.5,2.5,3.5),shaped_max
assert tuple(round(x,4) for x in water_min)==(0.0,-0.08,0.0),water_min
assert tuple(round(x,4) for x in water_max)==(1.0,1.08,1.0),water_max
print('BUILD_PASS solid/shaped/liquid AABBs',solid_min,solid_max,shaped_min,shaped_max,water_min,water_max)

# Frustum culling: opaque streams use separate compact lists; two liquid streams
# receive adjacent fixed command offsets so their back-to-front order is testable.
streams=[
    0,0,0,0,  0,0,0,1,  # solid, relative fixed offset 0
    0,4,32,0, 0,1,0,1,  # shaped, relative fixed offset 0
    0,12,64,0, 0,3,0,1, # far liquid, fixed offset 0
    0,13,0,0,  0,3,1,1, # near liquid, fixed offset 1
]
b_streams=mkbuf(0,C.string_at((C.c_uint*32)(*streams),128))
b_cmd=mkbuf(2,bytes(64)); b_draw=mkbuf(3,bytes(16))
pr2=program('assets/shaders/gpu_cull_meshlets.comp'); use(pr2)
def loc2(name): return getuniform(pr2,name.encode())
uniform1ui(loc2('u_stream_count'),4); uniform1ui(loc2('u_stream_base'),0)
uniform1ui(loc2('u_compact'),0); uniform1ui(loc2('u_solid_command_base'),0)
uniform1ui(loc2('u_shaped_command_base'),1); uniform1ui(loc2('u_liquid_command_base'),2)
planes=(C.c_float*24)(1,0,0,10, -1,0,0,10, 0,1,0,10, 0,-1,0,10, 0,0,1,10, 0,0,-1,10)
uniform4fv(loc2('u_frustum_planes[0]'),6,planes)
identity=(C.c_float*16)(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)
uniformmat(loc2('u_view_projection'),1,0,identity); uniform2f(loc2('u_viewport_size'),64,64); uniform1i(loc2('u_use_hiz'),0)
bindbase(GL_SSBO,0,b_streams); bindbase(GL_SSBO,1,b_bounds); bindbase(GL_SSBO,2,b_cmd); bindbase(GL_SSBO,3,b_draw)
for base in range(4):
    uniform1ui(loc2('u_stream_base'),base); dispatch(1,1,1)
barrier(GL_SHADER_STORAGE_BARRIER|GL_COMMAND_BARRIER)
cmd=getwords(b_cmd,16)
assert cmd[0:4]==[6,1,0,0],cmd[0:4]
assert cmd[4:8]==[6,1,32*6,0],cmd[4:8]
assert cmd[8:12]==[6,1,64*6,0],cmd[8:12]
assert cmd[12:16]==[6,1,0,0],cmd[12:16]
assert geterr()==0
print('CULL_PASS solid + shaped + ordered liquid commands',cmd)

# Compact mode only compacts opaque streams. Liquid commands must stay at fixed offsets
# (the reverse-distance order assembled by Renderer::prepare_gpu_meshlet_frame).
use(pr2); uniform1ui(loc2('u_compact'),1)
bindbuf(GL_SSBO,b_draw)
bufferdata(GL_SSBO,16,C.string_at((C.c_uint*4)(0,0,0,0),16),GL_DYNAMIC)
bindbase(GL_SSBO,3,b_draw)
for base in range(4):
    uniform1ui(loc2('u_stream_base'),base); dispatch(1,1,1)
barrier(GL_SHADER_STORAGE_BARRIER|GL_COMMAND_BARRIER|0x8000)
counts_out=getwords(b_draw,4); cmd_compact=getwords(b_cmd,16)
assert counts_out==[1,1,0,0],counts_out
assert sorted([cmd_compact[0:4],cmd_compact[4:8]],key=lambda x:x[2])==[
    [6,1,0,0],[6,1,192,0]],cmd_compact
assert cmd_compact[8:12]==[6,1,384,0],cmd_compact[8:12]
assert cmd_compact[12:16]==[6,1,0,0],cmd_compact[12:16]
assert geterr()==0
print('COMPACT_PASS opaque counts; liquid fixed order',counts_out,'commands',cmd_compact)

# A forced-near 1x1 Hi-Z image would reject every ordinary opaque AABB. LOD streams
# carry range.w bit 0 and intentionally skip coarse Hi-Z rejection to avoid speckled
# coarse terrain holes; liquid always bypasses opaque Hi-Z as well.
hiz_tex=u(); gen_tex(1,C.byref(hiz_tex)); active_tex(0x84C0 + 10); bind_tex(0x0DE1,hiz_tex)
near_depth=C.c_float(0.0)
tex_image(0x0DE1,0,0x822E,1,1,0,0x1903,0x1406,C.byref(near_depth))
tex_param(0x0DE1,0x2801,0x2600); tex_param(0x0DE1,0x2800,0x2600)
streams_lod=[
    0,0,0,1, 0,0,0,1,  # solid LOD stream: skip Hi-Z, keep frustum test
    0,4,32,0, 0,1,0,1, # shaped detail stream: subject to Hi-Z
    0,12,64,0, 0,3,0,1 # liquid: subject to frustum only
]
b_streams_lod=mkbuf(0,C.string_at((C.c_uint*24)(*streams_lod),96))
b_cmd_lod=mkbuf(2,bytes(48)); b_counts_lod=mkbuf(3,C.string_at((C.c_uint*4)(0,0,0,0),16))
use(pr2); uniform1ui(loc2('u_stream_count'),3); uniform1ui(loc2('u_compact'),0)
uniform1i(loc2('u_use_hiz'),1); uniform1i(loc2('u_hiz'),10)
bindbase(GL_SSBO,0,b_streams_lod); bindbase(GL_SSBO,1,b_bounds)
bindbase(GL_SSBO,2,b_cmd_lod); bindbase(GL_SSBO,3,b_counts_lod)
for base in range(3):
    uniform1ui(loc2('u_stream_base'),base); dispatch(1,1,1)
barrier(GL_SHADER_STORAGE_BARRIER|GL_COMMAND_BARRIER)
cmd_lod=getwords(b_cmd_lod,12)
assert cmd_lod[0]==6 and cmd_lod[4]==0 and cmd_lod[8]==6,cmd_lod
assert geterr()==0
print('LOD_HIZ_PASS coarse LOD retained / detailed opaque occluded / liquid retained')
