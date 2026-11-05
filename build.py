#!/usr/bin/env python3
"""Build Switchlens without make (Windows, Linux, macOS).

usage: python build.py [sysmodule|overlay|all] [--libtesla PATH] [--out DIR]

Needs devkitPro (devkitA64 + libnx + switch-tools) and, for the overlay, libtesla
(https://github.com/WerWolv/libtesla). Set DEVKITPRO, or install to the default location.
Output (default ./dist) is laid out like the SD card root:
  dist/atmosphere/contents/420000000007E51C/{exefs.nsp,toolbox.json,flags/boot2.flag}
  dist/switch/.overlays/switchlens.ovl
"""
import argparse, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = '.exe' if os.name == 'nt' else ''
TID = '420000000007E51C'
ARCH = ['-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-fPIE']

ap = argparse.ArgumentParser()
ap.add_argument('target', nargs='?', default='all', choices=['sysmodule', 'overlay', 'all'])
ap.add_argument('--libtesla', help='libtesla checkout (directory containing include/tesla.hpp); default $DEVKITPRO/libtesla')
ap.add_argument('--out', default=os.path.join(HERE, 'dist'))
a = ap.parse_args()

DK = os.environ.get('DEVKITPRO') or (r'C:\devkitPro' if os.name == 'nt' else '/opt/devkitpro')
if not os.path.isdir(DK):
    sys.exit('devkitPro not found at %s (set DEVKITPRO)' % DK)
GXX = os.path.join(DK, 'devkitA64', 'bin', 'aarch64-none-elf-g++' + EXE)
TOOLS = os.path.join(DK, 'tools', 'bin')
LIBNX = os.path.join(DK, 'libnx')
env = os.environ.copy()
env['DEVKITPRO'] = DK
env['PATH'] = os.path.join(DK, 'devkitA64', 'bin') + os.pathsep + TOOLS + os.pathsep + env['PATH']
BUILD = os.path.join(HERE, 'build')
os.makedirs(BUILD, exist_ok=True)


def run(cmd, cwd=HERE):
    print('>', ' '.join(cmd))
    subprocess.run(cmd, check=True, cwd=cwd, env=env)


def build_sysmodule():
    src = os.path.join(HERE, 'sysmodule')
    obj, elf = os.path.join(BUILD, 'sys.o'), os.path.join(BUILD, 'sys.elf')
    nso, npdm = os.path.join(BUILD, 'main'), os.path.join(BUILD, 'main.npdm')
    run([GXX, '-std=c++17', '-O2', '-Wall', *ARCH, '-ffunction-sections', '-fdata-sections',
         '-I' + os.path.join(LIBNX, 'include'), '-c', os.path.join(src, 'source', 'main.cpp'), '-o', obj])
    # -lnx must come after the objects or archive members (i2c, psc, ...) are not pulled in.
    run([GXX, *ARCH, '-specs=' + os.path.join(LIBNX, 'switch.specs'), '-Wl,--gc-sections',
         '-o', elf, obj, '-L' + os.path.join(LIBNX, 'lib'), '-lnx'])
    run([os.path.join(TOOLS, 'elf2nso' + EXE), elf, nso])
    run([os.path.join(TOOLS, 'npdmtool' + EXE), os.path.join(src, 'config.json'), npdm])
    stage = os.path.join(BUILD, 'exefs')
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    shutil.copy(nso, os.path.join(stage, 'main'))
    shutil.copy(npdm, os.path.join(stage, 'main.npdm'))
    dst = os.path.join(a.out, 'atmosphere', 'contents', TID)
    os.makedirs(os.path.join(dst, 'flags'), exist_ok=True)
    run([os.path.join(TOOLS, 'build_pfs0' + EXE), stage, os.path.join(dst, 'exefs.nsp')])
    shutil.copy(os.path.join(src, 'toolbox.json'), dst)
    open(os.path.join(dst, 'flags', 'boot2.flag'), 'wb').close()


def build_overlay():
    tesla = a.libtesla or os.path.join(DK, 'libtesla')
    if not os.path.isfile(os.path.join(tesla, 'include', 'tesla.hpp')):
        sys.exit('libtesla not found at %s (use --libtesla)' % tesla)
    src = os.path.join(HERE, 'overlay', 'source', 'main.cpp')
    obj, elf, nacp = (os.path.join(BUILD, n) for n in ('ovl.o', 'ovl.elf', 'ovl.nacp'))
    ovl = os.path.join(a.out, 'switch', '.overlays', 'switchlens.ovl')
    os.makedirs(os.path.dirname(ovl), exist_ok=True)
    # The embedded NACP is what makes Tesla/Ultrahand list the overlay.
    run([os.path.join(TOOLS, 'nacptool' + EXE), '--create', 'Switchlens', 'Switchlens', '1.0.0', nacp])
    run([GXX, '-std=gnu++20', '-Os', '-Wall', *ARCH, '-ffunction-sections', '-fdata-sections', '-D__SWITCH__',
         '-I' + os.path.join(LIBNX, 'include'), '-I' + os.path.join(tesla, 'include'), '-c', src, '-o', obj])
    run([GXX, '-specs=' + os.path.join(LIBNX, 'switch.specs'), *ARCH, '-Wl,--gc-sections',
         '-L' + os.path.join(LIBNX, 'lib'), '-o', elf, obj, '-lnx'])
    run([os.path.join(TOOLS, 'elf2nro' + EXE), elf, ovl, '--nacp=' + nacp])


if a.target in ('sysmodule', 'all'):
    build_sysmodule()
if a.target in ('overlay', 'all'):
    build_overlay()
print('done ->', a.out)
