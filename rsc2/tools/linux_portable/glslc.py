#!/usr/bin/env python3
"""glslc stand-in for the portable Linux build (see build.sh).

Ubuntu 22.04 has no glslc package, so the shaders are compiled with
glslangValidator, built from the same glslang source as the game's runtime
compiler. This takes the glslc arguments xboxrecomp's CMake passes
(--target-env=, -O, -mfmt=c, -D, -fshader-stage=, -I, -o) and writes the
same C array (-mfmt=c). -O is dropped: that glslang has no optimizer, and
the driver optimises the SPIR-V anyway.
"""
import os
import struct
import subprocess
import sys
import tempfile

validator = os.environ.get('GLSLANG_VALIDATOR', 'glslangValidator')
env, stage, out, src, extra = 'vulkan1.3', None, None, None, []
args = iter(sys.argv[1:])
for a in args:
    if a.startswith('--target-env='):
        env = a.split('=', 1)[1]
    elif a.startswith('-fshader-stage='):
        stage = a.split('=', 1)[1]
    elif a in ('-O', '-mfmt=c'):
        pass
    elif a == '-o':
        out = next(args)
    elif a == '-I':
        extra.append('-I' + next(args))
    elif a.startswith(('-D', '-I')):
        extra.append(a)
    elif a.startswith('-'):
        sys.exit('glslc.py: unsupported option ' + a)
    else:
        src = a
if not src or not out:
    sys.exit('usage: glslc.py [options] <source> -o <out>')
stage = stage or os.path.splitext(src)[1].lstrip('.')

with tempfile.TemporaryDirectory() as tmp:
    spv = os.path.join(tmp, 'out.spv')
    r = subprocess.run([validator, '-V', '--target-env', env, '-S', stage, *extra,
                        '-o', spv, src], stdout=subprocess.PIPE, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout)
        sys.exit(r.returncode)
    data = open(spv, 'rb').read()
words = struct.unpack('<%dI' % (len(data) // 4), data)
lines = [','.join('0x%08x' % w for w in words[i:i + 4]) for i in range(0, len(words), 4)]
with open(out, 'w') as f:
    f.write('{' + ',\n'.join(lines) + '}\n')
