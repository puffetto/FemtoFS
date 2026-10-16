#!/usr/bin/env python3
# File: ko/test-boot-guest.py
# Created by Andrea "Nemesi" Cocito on 16/10/2026
# Runs only inside the disposable FemtoFS root-boot guest.
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys


def run(*args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def counter(name):
    return int(subprocess.check_output(['sysctl', '-n', 'vfs.femtofs.'+name]))


def refuse(*args):
    result = subprocess.run([str(a) for a in args], capture_output=True)
    assert result.returncode != 0, args


assert Path('/identity').read_text() == 'FemtoFS root\n'
with open('/dev/md0', 'rb') as f:
    root_size = struct.unpack_from('<I', f.read(128), 32)[0]
assert counter('borrowed_bytes') == root_size and counter('snapshot_bytes') == 0
print(f'FEMTOFS_BOOT root borrowed={root_size} snapshot=0', flush=True)
mount = Path('/tmp/data'); mount.mkdir()
raw = Path('/runtime/boot/data.img').read_bytes()
u32 = lambda off: struct.unpack_from('<I', raw, off)[0]
files = {}
for i in range(u32(56)):
    bucket = 128 + (u32(52)+i)*16
    if raw[bucket] not in (1, 4):
        continue
    nameoff = u32(bucket+8)
    name = raw[nameoff:raw.index(b'\0', nameoff)].decode()
    obj = 128 + u32(bucket+4)*16
    if raw[obj] == 4:
        obj = 128 + u32(obj+4)*16
    files[name] = u32(obj+4), u32(obj+8)

# A supported but busy preload must fail, never create a snapshot.
writer = os.open('/dev/md1', os.O_RDWR)
try:
    refuse('/mount_femtofs', '/dev/md1', mount)
finally:
    os.close(writer)
assert counter('borrowed_bytes') == root_size and counter('snapshot_bytes') == 0
# Header/metadata rejection must release the borrowed hold and accounting.
for unit in (2, 3):
    refuse('/mount_femtofs', f'/dev/md{unit}', mount)
    assert counter('borrowed_bytes') == root_size and counter('snapshot_bytes') == 0
    run('mdconfig', '-d', '-o', 'force', '-u', unit)

for mode in ('clean', 'leaking'):
    clean_before = counter('clean_bytes')
    faults_before = counter('borrowed_faults')
    run('/mount_femtofs', '-o', mode, '/dev/md1', mount)
    assert counter('borrowed_bytes') == root_size + len(raw)
    assert counter('snapshot_bytes') == 0
    assert (mount/'identity').read_text() == 'FemtoFS data\n'
    run('/test-directory', mount)
    assert (mount/'file-4097').stat().st_ino == (mount/'hardlink').stat().st_ino
    assert (mount/'symlink').read_bytes() == (mount/'file-4097').read_bytes()
    fd = os.open(mount/'fifo', os.O_RDWR | os.O_NONBLOCK)
    assert os.write(fd, b'fifo') == 4 and os.read(fd, 4) == b'fifo'
    os.close(fd)
    refuse('mdconfig', '-d', '-o', 'force', '-u', 1)
    for name, (off, size) in files.items():
        assert (mount/name).read_bytes() == raw[off:off+size]
        rounded = (size+4095)//4096*4096
        leaking = mode == 'leaking' and size >= 4096 and off % 4096 == 0 and off+rounded <= u32(44)
        expected = raw[off:off+rounded] if leaking else raw[off:off+size] + bytes(rounded-size)
        Path('/tmp/expected').write_bytes(expected)
        run('/test-mmap', mount/name, '/tmp/expected', mount)
    with (mount/'file-10000').open('rb') as f:
        sender, receiver = socket.socketpair()
        try:
            assert os.sendfile(sender.fileno(), f.fileno(), 0, 10000) == 10000
            assert receiver.recv(10001) == (mount/'file-10000').read_bytes()
        finally:
            sender.close(); receiver.close()
    assert counter('borrowed_faults') > faults_before
    assert counter('snapshot_bytes') == 0
    run('umount', mount)
    assert counter('borrowed_bytes') == root_size
    assert counter('clean_bytes') == clean_before
    print(f'FEMTOFS_BOOT {mode}: original backing, mmap, lifetime, sendfile, cleanup passed', flush=True)

# A file-backed md retains the old snapshot behavior, including existing writers.
Path('/tmp/backing.img').write_bytes(raw)
unit = subprocess.check_output(['mdconfig', '-a', '-t', 'vnode', '-f', '/tmp/backing.img']).decode().strip()
writer = os.open('/dev/'+unit, os.O_RDWR)
try:
    run('/mount_femtofs', '/dev/'+unit, mount)
    assert counter('snapshot_bytes') == len(raw) and counter('borrowed_bytes') == root_size
    assert (mount/'identity').read_text() == 'FemtoFS data\n'
    run('umount', mount)
finally:
    os.close(writer)
run('mdconfig', '-d', '-u', unit)
assert counter('snapshot_bytes') == 0
run(sys.executable, '/test-mount.py', '/makefemtofs', '/mount_femtofs', '/test-mmap', '/tmp')
assert counter('snapshot_bytes') == 0 and counter('borrowed_bytes') == root_size
run('mdconfig', '-d', '-u', 1)
print('FEMTOFS_BOOT ALL TESTS PASSED', flush=True)
# END File: ko/test-boot-guest.py
