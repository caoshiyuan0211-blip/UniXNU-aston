#!/usr/bin/env python3
"""Bring UniXNU's scripts/check-turnip-darwin-host.py (UniXNU 00f47f68) up to its driver: build NativeAdrenoPreempt.cpp (newer than the
script's list) and link hostsim-stubs.cpp (two weak kernel hooks). Usage: patch-hostsim.py UNIXNU_DIR"""
import sys
from pathlib import Path
here = Path(__file__).resolve().parent
p = Path(sys.argv[1]) / 'scripts/check-turnip-darwin-host.py'
s = p.read_text()
a = "'NativeAdrenoSmmuContext', 'NativeAdrenoGpuContext']:"
assert s.count(a) == 1, 'core list changed'
s = s.replace(a, "'NativeAdrenoSmmuContext', 'NativeAdrenoGpuContext', 'NativeAdrenoPreempt']:")
a = "shim_obj = out / 'host_iokit_shim.o'\n"
assert s.count(a) == 1, 'shim line changed'
s = s.replace(a, "stub_obj = out / 'hostsim-stubs.o'\nsubprocess.run(['xcrun', 'clang++', '-std=c++17', '-O1', '-fPIC', '-c', '%s', '-o', stub_obj], check=True)\nobjects.append(stub_obj)\n" % (here / 'hostsim-stubs.cpp') + a)
p.write_text(s)
print('patched', p)
