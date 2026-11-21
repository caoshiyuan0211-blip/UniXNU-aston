#!/usr/bin/env python3
"""Bring UniXNU's scripts/check-turnip-darwin-host.py (UniXNU 00f47f68) up to its driver: build NativeAdrenoPreempt.cpp (newer than the
script's list), link hostsim-stubs.cpp (two weak kernel hooks) and move the host client GPU range above the CP preemption state. Usage: patch-hostsim.py UNIXNU_DIR"""
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
# The host device info still gave clients GPU addresses from 0x20000000, where NativeAdrenoPreempt (0x10000000 records, 0x20000000 and
# 0x20001000 SMMU info pages) now maps the CP's preemption state: Turnip's first BoMap collided (kIOReturnBusy). The phone driver
# starts clients at GpuClientVaStart = 0x100000000 (NativeAdrenoGpuContext.h, msm ADRENO_VM_START); the host does the same.
q = Path(sys.argv[1]) / 'drivers/Adreno740/tests/hostsim/host_gpu_service.cpp'
t = q.read_text()
b = '0x20000000ull,0x60000000ull'
assert t.count(b) == 1, 'host device info changed'
q.write_text(t.replace(b, '0x100000000ull,0x60000000ull'))
print('patched', q)
print('patched', p)
