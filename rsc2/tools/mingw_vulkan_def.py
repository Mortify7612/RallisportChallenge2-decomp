#!/usr/bin/env python3
"""Write a vulkan-1.def for the MinGW import library (see rsc2/RUNNING.md).

Lists what every vulkan-1.dll exports: core Vulkan 1.0-1.3 and the Win32
surface and swapchain functions, read from the Vulkan headers. Extension
functions are left out on purpose -- the loader does not export them, so
calling one directly must fail at link time, not when the .exe loads.

    mingw_vulkan_def.py <vulkan include dir> <out.def>
"""
import re
import sys

KEEP = {'VK_VERSION_1_0', 'VK_VERSION_1_1', 'VK_VERSION_1_2', 'VK_VERSION_1_3',
        'VK_KHR_surface', 'VK_KHR_swapchain', 'VK_KHR_win32_surface'}

inc, out = sys.argv[1], sys.argv[2]
names = set()
for h in ('vulkan_core.h', 'vulkan_win32.h'):
    section = None
    for line in open('%s/vulkan/%s' % (inc, h)):
        m = re.match(r'#define (VK_\w+) 1$', line.strip())
        # A feature block opens with "#define VK_VERSION_1_2 1" or an
        # extension name (which has lower case); other "... 1" defines don't.
        if m and (re.match(r'VK_VERSION_\d_\d$', m.group(1)) or re.search('[a-z]', m.group(1))):
            section = m.group(1)
        m = re.search(r'VKAPI_CALL (vk\w+)\(', line)
        if m and section in KEEP:
            names.add(m.group(1))
with open(out, 'w') as f:
    f.write('LIBRARY vulkan-1.dll\nEXPORTS\n')
    f.writelines('    %s\n' % n for n in sorted(names))
print('%d functions -> %s' % (len(names), out))
