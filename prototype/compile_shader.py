#!/usr/bin/env python3
"""Compile the toy shader using the installed shaderc runtime, with no package changes."""
import ctypes as c
from pathlib import Path
lib=c.CDLL('libshaderc_shared.so.1')
def api(name,restype,args):
 f=getattr(lib,name);f.restype=restype;f.argtypes=args;return f
init=api('shaderc_compiler_initialize',c.c_void_p,[])
compile_=api('shaderc_compile_into_spv',c.c_void_p,[c.c_void_p,c.c_char_p,c.c_size_t,c.c_int,c.c_char_p,c.c_char_p,c.c_void_p])
status=api('shaderc_result_get_compilation_status',c.c_int,[c.c_void_p])
error=api('shaderc_result_get_error_message',c.c_char_p,[c.c_void_p])
length=api('shaderc_result_get_length',c.c_size_t,[c.c_void_p])
bytes_=api('shaderc_result_get_bytes',c.c_void_p,[c.c_void_p])
release=api('shaderc_result_release',None,[c.c_void_p])
finish=api('shaderc_compiler_release',None,[c.c_void_p])
p=Path(__file__).resolve().parent;src=(p/'cube.comp').read_bytes();compiler=init()
if not compiler:raise SystemExit('shaderc initialization failed')
result=compile_(compiler,src,len(src),2,b'cube.comp',b'main',None)
if not result:raise SystemExit('shaderc returned no result')
try:
 if status(result):raise SystemExit(error(result).decode())
 (p/'build').mkdir(exist_ok=True);(p/'build/cube.spv').write_bytes(c.string_at(bytes_(result),length(result)))
 print('Compiled cube.comp')
finally:release(result);finish(compiler)
