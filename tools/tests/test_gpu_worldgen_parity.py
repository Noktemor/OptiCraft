#!/usr/bin/env python3
"""Headless EGL validation of OpenSimplex2S material shader against project's C noise.
Optional on developer machines: requires libEGL/libGL and C compiler. No save files touched.
"""
import ctypes as C
import math
import os
import pathlib
import random
import struct
import subprocess
import tempfile

FULL = os.environ.get('GPU_WORLDGEN_FULL') == '1'

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / 'external/opensimplex/OpenSimplex2S.c'
SHADER = ROOT / 'assets/shaders/worldgen_material.comp'

# A true GPU context (Mesa surfaceless EGL also works; on a laptop uses installed GL driver).
E=C.CDLL('libEGL.so.1'); E.eglGetProcAddress.restype=C.c_void_p; E.eglGetProcAddress.argtypes=[C.c_char_p]
E.eglInitialize.restype=C.c_uint; E.eglInitialize.argtypes=[C.c_void_p,C.POINTER(C.c_int),C.POINTER(C.c_int)]
E.eglBindAPI.restype=C.c_uint; E.eglBindAPI.argtypes=[C.c_uint]
E.eglChooseConfig.restype=C.c_uint; E.eglChooseConfig.argtypes=[C.c_void_p,C.POINTER(C.c_int),C.POINTER(C.c_void_p),C.c_int,C.POINTER(C.c_int)]
E.eglCreatePbufferSurface.restype=C.c_void_p; E.eglCreatePbufferSurface.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(C.c_int)]
E.eglCreateContext.restype=C.c_void_p; E.eglCreateContext.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.POINTER(C.c_int)]
E.eglMakeCurrent.restype=C.c_uint; E.eglMakeCurrent.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p,C.c_void_p]
f=E.eglGetProcAddress(b'eglGetPlatformDisplayEXT')
display=C.CFUNCTYPE(C.c_void_p,C.c_uint,C.c_void_p,C.POINTER(C.c_int))(f)(0x31DD,None,None)
a=C.c_int();b=C.c_int();assert E.eglInitialize(display,C.byref(a),C.byref(b))
assert E.eglBindAPI(0x30A2)
attrs=(C.c_int*7)(0x3033,1,0x3040,8,0x3024,8,0x3038)
cfg=C.c_void_p(); num=C.c_int(); assert E.eglChooseConfig(display,attrs,C.byref(cfg),1,C.byref(num)) and num.value
surf=E.eglCreatePbufferSurface(display,cfg,(C.c_int*5)(0x3057,1,0x3056,1,0x3038))
ctx=E.eglCreateContext(display,cfg,None,(C.c_int*7)(0x3098,4,0x30FB,3,0x30FD,1,0x3038))
assert ctx and E.eglMakeCurrent(display,surf,surf,ctx)
G=C.CDLL('libGL.so.1'); i=C.c_int; u=C.c_uint; ssize=C.c_ssize_t

def gl(name,ret,args):
    fn=getattr(G,name); fn.restype=ret; fn.argtypes=args; return fn
create=gl('glCreateShader',u,[u]); source=gl('glShaderSource',None,[u,i,C.POINTER(C.c_char_p),C.POINTER(i)])
compile_=gl('glCompileShader',None,[u]); getsh=gl('glGetShaderiv',None,[u,u,C.POINTER(i)])
getlog=gl('glGetShaderInfoLog',None,[u,i,C.POINTER(i),C.c_void_p])
newprog=gl('glCreateProgram',u,[]);attach=gl('glAttachShader',None,[u,u]);link=gl('glLinkProgram',None,[u]);getpr=gl('glGetProgramiv',None,[u,u,C.POINTER(i)])
use=gl('glUseProgram',None,[u]); loc=gl('glGetUniformLocation',i,[u,C.c_char_p]);u1i=gl('glUniform1i',None,[i,i]);u1v=gl('glUniform1uiv',None,[i,i,C.POINTER(u)])
genbuf=gl('glGenBuffers',None,[i,C.POINTER(u)]); bind=gl('glBindBuffer',None,[u,u]);bdata=gl('glBufferData',None,[u,ssize,C.c_void_p,u]);bbase=gl('glBindBufferBase',None,[u,u,u]);dispatch=gl('glDispatchCompute',None,[u,u,u]);barrier=gl('glMemoryBarrier',None,[u]);getsub=gl('glGetBufferSubData',None,[u,ssize,ssize,C.c_void_p]); err=gl('glGetError',u,[])
code=SHADER.read_bytes();csrc=C.c_char_p(code);shader=create(0x91B9);source(shader,1,C.byref(csrc),None);compile_(shader)
ok=i();getsh(shader,0x8B81,C.byref(ok));assert ok.value
program=newprog();attach(program,shader);link(program);getpr(program,0x8B82,C.byref(ok));assert ok.value

def buffer(slot,content):
    raw=C.create_string_buffer(content)
    b=u();genbuf(1,C.byref(b));bind(0x90D2,b);bdata(0x90D2,len(content),raw,0x88E8);bbase(0x90D2,slot,b)
    return b

class G3(C.Structure): _fields_=[('x',C.c_double),('y',C.c_double),('z',C.c_double)]
class Lattice(C.Structure):pass
Lattice._fields_=[('dxr',C.c_double),('dyr',C.c_double),('dzr',C.c_double),('xrv',i),('yrv',i),('zrv',i),('fail',C.POINTER(Lattice)),('success',C.POINTER(Lattice))]
class Env(C.Structure):_fields_=[('grad2',C.c_void_p),('grad3',C.c_void_p),('grad4',C.c_void_p),('lookup2',C.c_void_p),('lookup3',C.POINTER(C.POINTER(Lattice))),('lookup4',C.c_void_p)]
class Gradients(C.Structure):_fields_=[('perm',C.POINTER(C.c_short)),('grad2',C.c_void_p),('grad3',C.POINTER(G3)),('grad4',C.c_void_p)]
with tempfile.TemporaryDirectory() as td:
    so=pathlib.Path(td)/'simplex.so'
    subprocess.check_call(['cc','-O2','-shared','-fPIC','-o',str(so),str(SRC)])
    lib=C.CDLL(str(so));lib.initOpenSimplex.restype=C.POINTER(Env)
    lib.newOpenSimplexGradients.restype=C.POINTER(Gradients);lib.newOpenSimplexGradients.argtypes=[C.POINTER(Env),C.c_long]
    lib.noise3_XZBeforeY.restype=C.c_double;lib.noise3_XZBeforeY.argtypes=[C.POINTER(Env),C.POINTER(Gradients),C.c_double,C.c_double,C.c_double]
    env=lib.initOpenSimplex()
    for case, seed in enumerate([1,14594,287617,4202026]):
        grad=lib.newOpenSimplexGradients(env,seed)
        perm=[grad.contents.perm[x] for x in range(2048)]
        grads=[q for ix in range(2048) for q in (grad.contents.grad3[ix].x,grad.contents.grad3[ix].y,grad.contents.grad3[ix].z,0.)]
        idx={}; nodes=[]
        def add(ptr):
            if not ptr:return -1
            addr=C.addressof(ptr.contents)
            if addr in idx:return idx[addr]
            k=len(nodes);idx[addr]=k;nodes.append(None)
            n=ptr.contents
            f=add(n.fail);s=add(n.success)
            nodes[k]=struct.pack('<4d8i',n.dxr,n.dyr,n.dzr,0.,n.xrv,n.yrv,n.zrv,0,f,s,0,0)
            return k
        starts=[add(env.contents.lookup3[x]) for x in range(8)]
        assert len(nodes)<=128,len(nodes)
        bs=[buffer(0,struct.pack('<2048i',*perm)),buffer(1,struct.pack('<8192d',*grads)),buffer(2,b''.join(nodes)),buffer(3,struct.pack('<8i',*starts))]
        xbase=random.Random(seed).choice([-12,-4,0,3,12]);zbase=random.Random(seed+1).choice([-18,-7,0,4,14])
        # Match shader's biome id (Forest=4). Columns share a test height for clarity.
        height=[55.75,153.1,74.4,100.5][case] if FULL else random.Random(seed+2).choice([55.75,74.4,153.1])
        cy=[-8,-4,1,2][case] if FULL else -4
        columns=b''.join(struct.pack('<dII',height,4 if (v%3==0) else 2,0) for v in range(1024))
        bs.extend([buffer(4,columns),buffer(5,struct.pack('<i',cy)),buffer(6,bytes(32768*4))])
        use(program)
        for name,val in [('uChunkX',xbase),('uChunkZ',zbase),('uChunkCount',1),('uWorldMinY',-512),('uWorldMaxY',511)]:u1i(loc(program,name.encode()),val)
        # Block IDs as distinct and nonzero so we can compare classes without loading game catalog.
        blocks=list(range(101,111));a=(u*10)(*blocks);u1v(loc(program,b'uBlocks'),10,a)
        # Default: quick smoke test. FULL: all 32k cells and random samples across Y.
        n=32768 if FULL else 64
        dispatch((n+63)//64,1,1);barrier(0x2000|0x0200)
        assert err()==0
        result=(u*n)();bind(0x90D2,bs[-1]);getsub(0x90D2,0,C.sizeof(result),result)
        def nn(wx,wy,wz,xmul,ymul,zmul,ox=0,oy=0,oz=0):
            return max(0.,min(1.,0.5*lib.noise3_XZBeforeY(env,grad,wx*xmul+ox,wy*ymul+oy,wz*zmul+oz)+0.5))
        samples = random.Random(seed*277).sample(range(n), min(4096,n)) if FULL else list(range(n))
        for k in samples:
            x=k%32;z=(k//32)%32;wx=xbase*32+x;wz=zbase*32+z;wy=cy*32+k//1024;below=height-wy
            if wy < -256 or wy > 511 or wy >= math.floor(height) or below <= 4.0:
                assert result[k] == 0, (seed,k,wy,result[k])
                continue
            ore=nn(wx,wy,wz,.11,.11,.11)
            def vein(ox,oy,oz):return nn(wx,wy,wz,.13,.13,.13,ox,oy,oz)
            if below>=250:
                want=blocks[2] if ore>.84 else blocks[3] if ore>.76 else blocks[1]
            elif below>=90:
                want=(blocks[3] if ore>.915 else blocks[4] if ore>.86 else
                      blocks[5] if vein(31.7,-4.2,9.9)>.86 else
                      blocks[6] if vein(43.1,-12.3,7.4)>.88 else
                      blocks[7] if vein(-17.3,12.6,5.1)>.80 else
                      blocks[8] if ore>.74 else blocks[0])
            else:
                want=(blocks[5] if vein(31.7,-4.2,9.9)>.86 else
                      blocks[7] if vein(-17.3,12.6,5.1)>.80 else
                      blocks[6] if vein(43.1,-12.3,7.4)>.92 else
                      blocks[8] if ore>.82 else
                      blocks[9] if ore>.7 and ((z*32+x)%3==0) else blocks[0])
            got=result[k]
            assert got == want or got == 0, (seed,k,wx,wy,wz,got,want)
        print('GPU_WORLDGEN_PARITY_PASS',seed,len(nodes),'nodes,',len(samples),'voxels checked')
print('GPU worldgen shader compiled and noise/material test passed')
