#!/usr/bin/env python3
# File: ko/test-boot.py
# Created by Andrea "Nemesi" Cocito on 16/10/2026
# Reuse the md regression's boot disk to test a FemtoFS root and both backing paths.
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


if platform.system() != 'FreeBSD' or len(sys.argv) != 5:
    sys.exit('usage on FreeBSD: test-boot.py md-guest-directory builder freebsd-src new-work-directory')
base, builder, src, work = [Path(p).resolve() for p in sys.argv[1:]]
repo = Path(__file__).resolve().parent.parent
work.mkdir()
build, root, stage, data = (work / n for n in ('module', 'root', 'stage', 'data'))
build.mkdir(); root.mkdir(); data.mkdir()
# The existing md regression supplies the tested kernel, loader and disk layout.
stage.mkdir()
shutil.copytree(base/'root/boot', stage/'boot', copy_function=shutil.copy)
env = dict(os.environ, MAKEOBJDIR=str(build))
run('make', '-C', repo/'ko', f'SYSDIR={src}/sys', env=env)
for name in ('mount_femtofs', 'test-mmap', 'test-directory'):
    run('cc', '-static', '-Wall', '-Wextra', '-Werror', '-g', repo/f'ko/{name}.c', '-o', root/name)
shutil.copy(build/'femtofs.ko', stage/'boot/kernel/femtofs.ko')

# Dynamic test tools live on UFS: writable private library mappings are outside
# FemtoFS's read-only mmap contract. Init and the filesystem probes are static.
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

for name in ('dev', 'etc', 'bin', 'sbin', 'rescue', 'runtime', 'tmp', 'var/run'):
    (root/name).mkdir(parents=True, exist_ok=True)
for name in ('usr', 'lib', 'libexec'):
    (root/name).symlink_to('/runtime/'+name)
(root/'makefemtofs').symlink_to('/runtime/makefemtofs')
shutil.copy('/rescue/rescue', root/'rescue/rescue')
for name in ('sh', 'init', 'mount', 'umount', 'sysctl', 'mdconfig', 'dd', 'md5',
             'echo', 'reboot', 'sync'):
    os.link(root/'rescue/rescue', root/'rescue'/name)
os.link(root/'rescue/rescue', root/'bin/sh')
os.link(root/'rescue/rescue', root/'bin/echo')
os.link(root/'rescue/rescue', root/'sbin/init')
os.link(root/'rescue/rescue', root/'sbin/mount')
shutil.copy('/etc/login.conf', root/'etc/login.conf')
shutil.copy('/var/run/ld-elf.so.hints', root/'var/run/ld-elf.so.hints')
(root/'etc/fstab').write_text('/dev/md0 / femtofs ro 0 0\n')
(root/'etc/master.passwd').write_text(
    'root:*:0:0::0:0:Root:/root:/rescue/sh\n'
    'nobody:*:65534:65534::0:0:Unprivileged:/nonexistent:/rescue/sh\n')
run('pwd_mkdb', '-p', '-d', root/'etc', root/'etc/master.passwd')
for name in ('test-mount.py', 'test-boot-guest.py'):
    ast.parse((repo/'ko'/name).read_text())
    shutil.copy(repo/'ko'/name, root/name)
(root/'identity').write_text('FemtoFS root\n')
(root/'etc/rc').write_text('''#!/rescue/sh
export PATH=/rescue:/bin:/sbin:/usr/local/bin:/usr/bin:/usr/sbin
mount -t ufs -o ro /dev/ada0p2 /runtime || exit 1
mount -t tmpfs -o mode=1777 tmpfs /tmp || exit 1
export PYTHONDONTWRITEBYTECODE=1
/usr/local/bin/python3.12 /test-boot-guest.py
status=$?
echo "FEMTOFS_BOOT GUEST_STATUS=$status"
sync
reboot -p
''')

(data/'identity').write_text('FemtoFS data\n')
(data/'hidden').mkdir(mode=0o700)
for size in (0, 1, 4095, 4096, 4097, 8192, 10000):
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
raw = bytearray((stage/'boot/data.img').read_bytes())
raw[0] ^= 128
(stage/'boot/bad-header.img').write_bytes(raw)
raw[0] ^= 128; raw[12] ^= 128
(stage/'boot/bad-metadata.img').write_bytes(raw)
# Explicit loading order is required for a named root device. Lua module-table
# iteration from loader.conf does not guarantee a filename-to-md-unit order.
(stage/'boot/lua/loader.lua').chmod(0o644)
(stage/'boot/lua/loader.lua').write_text('''require("cli")
loader.perform("set console=comconsole")
loader.perform("set comconsole_speed=115200")
loader.perform("load /boot/kernel/kernel")
loader.perform("load /boot/kernel/femtofs.ko")
loader.perform("load -t md_image /boot/root.img")
loader.perform("load -t md_image /boot/data.img")
loader.perform("load -t md_image /boot/bad-header.img")
loader.perform("load -t md_image /boot/bad-metadata.img")
loader.perform("set vfs.root.mountfrom=femtofs:/dev/md0")
loader.perform("boot -v")
''')
run('makefs', '-t', 'ffs', '-s', '512m', '-o', 'version=2', work/'root.ufs', stage)
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
preloads = {unit: address for unit, address in re.findall(
    r'(md\d+): Preloaded image <[^>]+> \d+ bytes at (0x[0-9a-f]+)', output)}
borrowed = re.findall(r'femtofs: /dev/(md\d+) borrowed \d+ bytes at (0x[0-9a-f]+)', output)
assert {'md0', 'md1'} <= {unit for unit, _ in borrowed}, borrowed
assert all(address == preloads[unit] for unit, address in borrowed)
print(f'PASS: FemtoFS root boot, borrowed/fallback regressions and original backing identity; {work}/console.log')
# END File: ko/test-boot.py
