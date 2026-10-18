#!/usr/bin/env python3
# File: ko/test-boot.py
# Created by Andrea "Nemesi" Cocito on 16/10/2026
# Boot stock FreeBSD with resident banks, imports and explicit reclamation.
import ast
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import sysconfig


def run(*args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


if platform.system() != 'FreeBSD' or len(sys.argv) not in (5, 6):
    sys.exit('usage on FreeBSD: test-boot.py stock-boot-directory builder stock-src new-work-directory [control|reverse|audit|root-smoke]')
base, builder, src, work = [Path(p).resolve() for p in sys.argv[1:5]]
mode = sys.argv[5] if len(sys.argv) == 6 else "root"
assert mode in ("root", "reverse", "control", "audit", "root-smoke")
repo = Path(__file__).resolve().parent.parent
work.mkdir()
build, root, stage, data = (work / n for n in ('module', 'root', 'stage', 'data'))
build.mkdir(); root.mkdir(); data.mkdir()
# Stock kernel and loader, with only the guest's required modules.
stage.mkdir()
(stage/'boot').mkdir()
for name in ('loader', 'loader_lua', 'lua', 'defaults'):
    if (base/name).is_dir():
        shutil.copytree(base/name, stage/'boot'/name, copy_function=shutil.copy)
    else:
        shutil.copy(base/name, stage/'boot'/name)
(stage/'boot/kernel').mkdir()
for name in ('kernel', 'nullfs.ko'):
    shutil.copy(base/'kernel'/name, stage/'boot/kernel'/name)
env = dict(os.environ, MAKEOBJDIR=str(build))
# A debug guest must compile the module against that kernel's option headers.
kernel_options = [f'KERNBUILDDIR={os.environ["FEMTOFS_TEST_KERNBUILDDIR"]}'] if 'FEMTOFS_TEST_KERNBUILDDIR' in os.environ else []
run('make', '-C', repo/'ko', f'SYSDIR={src}/sys', *kernel_options, env=env)
for name in ('mount_femtofs', 'femtoctl', 'test-bank', 'test-mmap', 'test-directory'):
    run('cc', '-static', '-Wall', '-Wextra', '-Werror', '-g', repo/f'ko/{name}.c', '-o', root/name)
shutil.copy(build/'femtofs.ko', stage/'boot/kernel/femtofs.ko')

# Python and the builder live on UFS; the normal shell, runtime linker and base
# libraries live in FemtoFS, so boot itself exercises private library mappings.
python = Path(sys.executable).resolve()
stdlib = Path(sysconfig.get_path('stdlib'))
shutil.copytree(stdlib, stage/stdlib.relative_to('/'), copy_function=shutil.copy,
                ignore=shutil.ignore_patterns('__pycache__', 'site-packages', 'test',
                                             'idlelib', 'tkinter', 'turtledemo', 'ensurepip'))
runtime = [python, builder, *stdlib.glob('lib-dynload/*.so')]
libraries = {Path('/libexec/ld-elf.so.1')}
for binary in runtime:
    out = run('ldd', binary, capture_output=True, text=True).stdout
    libraries.update(Path(p) for p in re.findall(r'=> (/\S+)', out))
for binary in [python, *libraries]:
    target = stage/binary.relative_to('/')
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(binary, target)
shutil.copy(builder, stage/'makefemtofs')

for name in ('dev', 'etc', 'bin', 'sbin', 'rescue', 'runtime', 'units', 'var/run'):
    (root/name).mkdir(parents=True, exist_ok=True)
(root/'tmp').mkdir() if mode in ('control', 'audit', 'root-smoke') else (root/'tmp').symlink_to('/runtime/tmp')
if mode not in ('control', 'audit', 'root-smoke'):
    (stage/'tmp').mkdir()
(root/'usr').symlink_to('/runtime/usr')
for name in ('lib', 'libexec'):
    shutil.copytree(stage/name, root/name, copy_function=shutil.copy)
(root/'makefemtofs').symlink_to('/runtime/makefemtofs')
shutil.copy('/rescue/rescue', root/'rescue/rescue')
for name in ('sh', 'init', 'mount', 'umount', 'sysctl', 'mdconfig', 'dd', 'md5',
             'cat', 'echo', 'reboot', 'sync', 'kldload', 'kldunload', 'kenv'):
    os.link(root/'rescue/rescue', root/'rescue'/name)
# Stock init is static. Its /bin/sh interpreter and these utilities are dynamic.
for name in ('/sbin/init', '/bin/sh', '/bin/echo', '/bin/cat'):
    shutil.copy(name, root/name.lstrip('/'))
for name in ('/bin/sh', '/bin/echo', '/bin/cat'):
    out = run('ldd', name, capture_output=True, text=True).stdout
    print(out, flush=True)
    for library in re.findall(r'=> (/\S+)', out):
        for destination in (root, stage):
            target = destination/Path(library).relative_to('/')
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                shutil.copy(library, target)
os.link(root/'rescue/rescue', root/'sbin/mount')
os.link(root/'rescue/rescue', root/'sbin/mount_nullfs')
shutil.copy('/etc/login.conf', root/'etc/login.conf')
shutil.copy('/var/run/ld-elf.so.hints', root/'var/run/ld-elf.so.hints')
(root/'etc/fstab').write_text('data /tmp/data femtofs ro,clean 0 0\n')
(root/'etc/guest-mode').write_text(mode)
os.link(root/'mount_femtofs', root/'sbin/mount_femtofs')
(root/'etc/shells').write_text('/rescue/sh\n')
(root/'etc/master.passwd').write_text(
    'root:*:0:0::0:0:Root:/root:/rescue/sh\n'
    'nobody:*:65534:65534::0:0:Unprivileged:/nonexistent:/rescue/sh\n')
run('pwd_mkdb', '-p', '-d', root/'etc', root/'etc/master.passwd')
for name in ('test-mount.py', 'test-boot-guest.py'):
    ast.parse((repo/'ko'/name).read_text())
    shutil.copy(repo/'ko'/name, root/name)
shutil.copy(repo/'units/dual-hash-names.txt', root/'units/dual-hash-names.txt')
(root/'identity').write_text('FemtoFS root\n')
(root/'etc/rc').write_text('''#!/bin/sh
export PATH=/rescue:/bin:/sbin
/bin/echo "FEMTOFS_BOOT dynamic shell before runtime: $(/bin/cat /identity)"
echo "FEMTOFS_BOOT ALL TESTS PASSED"
echo "FEMTOFS_BOOT GUEST_STATUS=0"
sync
reboot -p
''' if mode == 'root-smoke' else '''#!/bin/sh
export PATH=/rescue:/bin:/sbin:/usr/local/bin:/usr/bin:/usr/sbin
/bin/echo "FEMTOFS_BOOT dynamic shell before runtime: $(/bin/cat /identity)"
mode=$(cat /etc/guest-mode)
if [ "$mode" != control ] && [ "$mode" != audit ]; then
    mount -t ufs -o ro /dev/ada0p2 /runtime || exit 1
fi
mount -t tmpfs -o mode=1777 tmpfs /tmp || exit 1
export PYTHONDONTWRITEBYTECODE=1
/usr/local/bin/python3.12 /test-boot-guest.py
status=$?
if [ $status -eq 0 ]; then
    umount /tmp || status=1
    if [ "$mode" != control ] && [ "$mode" != audit ]; then
        umount /runtime || status=1
    fi
fi
echo "FEMTOFS_BOOT GUEST_STATUS=$status"
sync
reboot -p
''')

(data/'identity').write_text('FemtoFS data\n')
shutil.copy('/bin/echo', data/'dynamic-echo')
(data/'hidden').mkdir(mode=0o700)
for size in (0, 1, 4095, 4096, 4097, 8192, 10000, 4194304):
    (data/f'file-{size}').write_bytes(bytes(i % 251 + 1 for i in range(size)))
    (data/f'file-{size}').chmod(0o644)
(data/'private').write_bytes(b'secret' * 2000)
(data/'private').chmod(0o600)
os.link(data/'file-4097', data/'hardlink')
(data/'symlink').symlink_to('file-4097')
os.mkfifo(data/'fifo', 0o644)
for tree, name in ((root, 'root'), (data, 'data')):
    run(builder, '--root-uid', '0', '--root-gid', '0', '--temp-dir', work,
        tree, stage/f'boot/{name}.img')
# The loader retains the whole record, while imports retain only logical bytes.
with (stage/'boot/data.img').open('ab') as padded:
    padded.write(bytes(8192))
raw = bytearray((stage/'boot/data.img').read_bytes())
raw[0] ^= 128
(stage/'boot/bad-header.img').write_bytes(raw)
raw[0] ^= 128; raw[12] ^= 128
(stage/'boot/bad-metadata.img').write_bytes(raw)
# Use explicit labels; reverse loading order in a separate acceptance boot.
(stage/'boot/lua/loader.lua').chmod(0o644)
commands = ['set console=comconsole', 'set comconsole_speed=115200',
            'load /boot/kernel/kernel', 'load /boot/kernel/nullfs.ko',
            'load /boot/kernel/femtofs.ko']
order = ['root', 'data', 'bad-header', 'bad-metadata']
if mode == 'reverse':
    order.reverse()
for name in order:
    commands += [f'load -t femtofs_image /boot/{name}.img',
                 f'set femtofs.bank.{name}=/boot/{name}.img']
commands += ['lsmod', 'set vfs.root.mountfrom=' +
             ('ufs:/dev/ada0p2' if mode in ('control', 'audit') else 'femtofs:'),
             'set vfs.root.mountfrom.options=ro,clean,bank=root' if mode not in ('control', 'audit')
             else 'set vfs.root.mountfrom.options=ro', 'boot -v']
import json
(stage/'boot/lua/loader.lua').write_text('require("cli")\n' + ''.join(
    f'loader.perform({json.dumps(command)})\n' for command in commands))
if mode in ('control', 'audit'):
    # Keep the dynamic runtime already in stage; make /runtime refer to this root.
    for child in root.iterdir():
        if child.name in ('usr', 'lib', 'libexec', 'runtime'):
            continue
        target = stage/child.name
        if child.is_dir():
            shutil.copytree(child, target, dirs_exist_ok=True, copy_function=shutil.copy)
        elif not target.exists():
            shutil.copy(child, target, follow_symlinks=False)
    (stage/'runtime').symlink_to('/')
run('makefs', '-t', 'ffs', '-s', '1g' if mode in ('control', 'audit') else '512m',
    '-o', 'version=2', work/'root.ufs', stage)
run('mkimg', '-s', 'gpt', '-b', '/boot/pmbr', '-p', 'freebsd-boot:=/boot/gptboot',
    '-p', f'freebsd-ufs:={work}/root.ufs', '-o', work/'guest.img')
cmd = ['qemu-system-x86_64', '-accel', 'tcg', '-m', '1536', '-smp', '2',
       '-display', 'none', '-monitor', 'none', '-serial', 'stdio', '-nic', 'none',
       '-device', 'virtio-rng-pci', '-drive',
       f'file={work}/guest.img,format=raw,if=ide,snapshot=on', '-no-reboot']
with (work/'console.log').open('w') as log:
    result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=600)
output = (work/'console.log').read_text(errors='replace')
assert result.returncode == 0 and 'FEMTOFS_BOOT ALL TESTS PASSED' in output, work/'console.log'
assert 'FEMTOFS_BOOT GUEST_STATUS=0' in output and 'panic:' not in output
assert 'lock order reversal:' not in output
assert 'FEMTOFS_BOOT dynamic shell before runtime: FemtoFS root' in output
preloads = {label: address for label, address in re.findall(
    r'femtofs: preload (\S+) <[^>]+> \d+ bytes at (0x[0-9a-f]+)', output)}
mounted = re.findall(r'femtofs: bank (\S+) preload \d+ bytes at (0x[0-9a-f]+)', output)
assert 'data' in preloads and 'root' in preloads
loader_addresses = dict((name, int(address, 16)) for address, name in re.findall(
    r'(0x[0-9a-f]+): /boot/(root|data).img \(femtofs_image,', output))
assert all(int(address, 16) == 0xffffffff80000000 + loader_addresses[label]
           for label, address in preloads.items())
samples = re.findall(r'femtofs: page data offset (\d+) physical (0x[0-9a-f]+)', output)
if mode != 'root-smoke':
    assert samples and int(samples[0][1], 16) == loader_addresses['data'] + int(samples[0][0])
assert all(address == preloads[label] for label, address in mounted)
if mode != 'root-smoke':
    assert 'data' in {label for label, _ in mounted}
if mode not in ('control', 'audit'):
    assert 'root' in {label for label, _ in mounted}
coverage = 'root mount/dynamic shell/shutdown' if mode == 'root-smoke' else 'bank lifecycle/import regressions'
print(f'PASS: FemtoFS {mode} boot, {coverage}; {work}/console.log')
# END File: ko/test-boot.py
