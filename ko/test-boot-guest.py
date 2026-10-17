#!/usr/bin/env python3
# File: ko/test-boot-guest.py
# Created by Andrea "Nemesi" Cocito on 16/10/2026
# Runs only inside the disposable FemtoFS root-boot guest.
import concurrent.futures
import errno
import mmap
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys


def run(*args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def counter(name):
    value = int(subprocess.check_output(['sysctl', '-n', 'vfs.femtofs.'+name]))
    if name == 'imported_bytes':
        # On this target imported logical size is already VM-page aligned.
        assert value == counter('imported_image_bytes')
    return value


def refuse(*args):
    result = subprocess.run([str(a) for a in args], capture_output=True)
    assert result.returncode != 0, args


assert Path('/identity').read_text() == 'FemtoFS root\n'
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

# Select a physically and virtually 2 MiB-aligned window in loader memory.
metadata = subprocess.check_output(['sysctl', '-n', 'debug.dump_modinfo']).decode()
record = metadata.split('value:\t/boot/data.img', 1)[1]
address = int(re.search(r'MODINFO_ADDR.*?value:\s*(0x[0-9a-f]+)', record, re.S)[1], 16)
large_offset = (-(address + files['file-4194304'][0])) % (2 * 1024 * 1024)

if Path('/etc/guest-mode').read_text() == 'audit':
    mount = Path('/tmp/data'); mount.mkdir()
    run('/mount_femtofs', '-o', 'clean', 'data', mount)
    off, size = files['file-4194304']
    Path('/tmp/expected').write_bytes(raw[off:off+size])
    env = dict(os.environ, FEMTOFS_TEST_SUPERPAGE_OFFSET=str(large_offset))
    run('/test-mmap', mount/'file-4194304', '/tmp/expected', mount, 'audit', env=env)
    run('umount', mount)
    print('FEMTOFS_BOOT ALL TESTS PASSED', flush=True)
    sys.exit(0)

root_size = len(Path('/runtime/boot/root.img').read_bytes())
data_size = len(Path('/runtime/boot/data.img').read_bytes())
loader_total = root_size + data_size
assert counter('loader_bytes') == loader_total and counter('imported_bytes') == 0
data_image_size = struct.unpack_from('<I', Path('/runtime/boot/data.img').read_bytes(), 32)[0]
assert data_size == data_image_size + 8192
assert counter('loader_image_bytes') == root_size + data_image_size
for old in ('borrowed_bytes', 'snapshot_bytes', 'borrowed_faults', 'released_pages'):
    refuse('sysctl', '-n', 'vfs.femtofs.'+old)
print(f'FEMTOFS_BOOT banks loader_bytes={loader_total} '
      f'loader_image_bytes={root_size + data_image_size} imported_bytes=0', flush=True)
assert not subprocess.check_output(['mdconfig', '-l']).strip()
mount = Path('/tmp/data'); mount.mkdir()
ctl = '/femtoctl'
run(ctl, 'list')
refuse('kldunload', 'femtofs')
refuse('/mount_femtofs', '/dev/md0', mount)
refuse('/mount_femtofs', 'missing', mount)
refuse(ctl, 'add', 'async', 'future', '/runtime/boot/data.img')
refuse(ctl, 'add', 'data', '/runtime/boot/data.img')
for name in ('bad-header', 'bad-metadata'):
    refuse('/mount_femtofs', name, mount)
    refuse(ctl, 'add', 'invalid', '/runtime/boot/'+name+'.img')
assert counter('loader_bytes') == loader_total and counter('imported_bytes') == 0
raw = Path('/runtime/boot/data.img').read_bytes()[:data_image_size]
# Even with usermount enabled, only an administrator may claim loader memory.
user_mount = Path('/tmp/user-mount'); user_mount.mkdir()
os.chown(user_mount, 65534, 65534)
def nobody():
    os.setgroups([]); os.setgid(65534); os.setuid(65534)
old_usermount = subprocess.check_output(['sysctl', '-n', 'vfs.usermount']).strip().decode()
denied = []
try:
    run('sysctl', 'vfs.usermount=1')
    for command in (['/mount_femtofs', '-o', 'leaking', 'data', str(user_mount)],
                    ['mount', '-t', 'femtofs', '-o', 'ro', 'data', str(user_mount)]):
        result = subprocess.run(command, preexec_fn=nobody, capture_output=True, text=True)
        denied.append(result.returncode != 0 and 'Operation not permitted' in result.stderr)
        print(f'FEMTOFS_BOOT unprivileged mount denied={denied[-1]}: {result.stderr.strip()}', flush=True)
        if result.returncode == 0:
            run('umount', user_mount)
finally:
    run('sysctl', 'vfs.usermount='+old_usermount)
for mode in ('clean', 'leaking'):
    clean_before = counter('clean_bytes')
    faults_before = counter('loader_faults')
    run('/mount_femtofs', '-o', mode, 'data', mount)
    assert counter('loader_bytes') == loader_total
    assert counter('imported_bytes') == 0
    assert (mount/'identity').read_text() == 'FemtoFS data\n'
    run('/test-directory', mount)
    assert (mount/'file-4097').stat().st_ino == (mount/'hardlink').stat().st_ino
    assert (mount/'symlink').read_bytes() == (mount/'file-4097').read_bytes()
    fd = os.open(mount/'fifo', os.O_RDWR | os.O_NONBLOCK)
    assert os.write(fd, b'fifo') == 4 and os.read(fd, 4) == b'fifo'
    os.close(fd)
    refuse(ctl, 'discard', 'data')
    alias = Path('/tmp/alias'); alias.mkdir(exist_ok=True)
    refuse('/mount_femtofs', 'data', alias)
    run('mount', '-t', 'nullfs', mount, alias)
    assert (alias/'identity').read_bytes() == (mount/'identity').read_bytes()
    alias_bytes = (alias/'identity').read_bytes()
    Path('/tmp/alias-expected').write_bytes(alias_bytes + bytes((-len(alias_bytes)) % 4096))
    run('/test-mmap', alias/'identity', '/tmp/alias-expected', mount)
    print('FEMTOFS_BOOT nullfs mmap passed', flush=True)
    refuse('umount', mount)
    run('umount', alias)
    for name, (off, size) in files.items():
        assert (mount/name).read_bytes() == raw[off:off+size]
        rounded = (size+4095)//4096*4096
        leaking = mode == 'leaking' and size >= 4096 and off % 4096 == 0 and off+rounded <= u32(44)
        expected = raw[off:off+rounded] if leaking else raw[off:off+size] + bytes(rounded-size)
        Path('/tmp/expected').write_bytes(expected)
        env = dict(os.environ)
        if name == 'file-4194304':
            env['FEMTOFS_TEST_SUPERPAGE_OFFSET'] = str(large_offset)
        run('/test-mmap', mount/name, '/tmp/expected', mount, env=env)
    with (mount/'file-10000').open('rb') as f:
        sender, receiver = socket.socketpair()
        try:
            assert os.sendfile(sender.fileno(), f.fileno(), 0, 10000) == 10000
            assert receiver.recv(10001) == (mount/'file-10000').read_bytes()
        finally:
            sender.close(); receiver.close()
    assert counter('loader_faults') > faults_before
    assert counter('imported_bytes') == 0
    run('umount', mount)
    assert counter('loader_bytes') == loader_total
    assert counter('clean_bytes') == clean_before
    print(f'FEMTOFS_BOOT {mode}: original backing, mmap, lifetime, sendfile, cleanup passed', flush=True)

assert all(denied), 'unprivileged bank mount succeeded with vfs.usermount=1'
# Private writes must not bypass the separate execute restriction.
run('/mount_femtofs', '-o', 'noexec', 'data', mount)
with (mount/'file-4097').open('rb') as f:
    original = f.read()
    for flags in (mmap.MAP_PRIVATE, mmap.MAP_SHARED):
        try:
            mapping = mmap.mmap(f.fileno(), 4096, flags=flags, prot=mmap.PROT_READ | mmap.PROT_EXEC)
        except OSError as error:
            assert error.errno == errno.EACCES
        else:
            mapping.close()
            raise AssertionError('noexec mapping accepted')
    with mmap.mmap(f.fileno(), 4096, flags=mmap.MAP_PRIVATE,
                   prot=mmap.PROT_READ | mmap.PROT_WRITE) as mapping:
        mapping[0] ^= 0xff
    f.seek(0)
    assert f.read() == original
run('umount', mount)
print('FEMTOFS_BOOT noexec preserved; private writes leave read() unchanged', flush=True)
# A fresh clean mount: the tail cache outlives process A and is reused by B.
run('mount', mount)  # fstab -> standard mount helper dispatch
before = counter('clean_pages')
off, size = files['file-4097']
assert size == 4097 and off % 4096 == 0
Path('/tmp/tail').write_bytes(raw[off:off+size] + bytes(4095))
run('/test-mmap', mount/'file-4097', '/tmp/tail', mount)
assert counter('clean_pages') == before + 1
run('/test-mmap', mount/'hardlink', '/tmp/tail', mount)
assert counter('clean_pages') == before + 1
run('umount', mount)
run('/mount_femtofs', 'data', mount)
before = counter('clean_pages')
reader, writer = os.pipe()
ready_reader, ready_writer = os.pipe()
processes = [subprocess.Popen(['/test-mmap', str(mount/'file-4097'), '/tmp/tail', str(mount)],
    env=dict(os.environ, FEMTOFS_TEST_BARRIER=str(reader), FEMTOFS_TEST_READY=str(ready_writer)),
    pass_fds=(reader, ready_writer)) for _ in range(8)]
os.close(reader)
os.close(ready_writer)
ready = b''
while len(ready) < len(processes):
    chunk = os.read(ready_reader, len(processes) - len(ready))
    assert chunk, 'mmap probe exited before reaching the barrier'
    ready += chunk
os.close(ready_reader)
os.write(writer, b'x' * len(processes)); os.close(writer)
assert all(process.wait() == 0 for process in processes)
assert counter('clean_pages') == before + 1
run('umount', mount)

# File import disconnects from the source; remount retains the owned allocation.
for content in (b'', raw[:64], raw[:-4096]):
    Path('/tmp/short.img').write_bytes(content)
    refuse(ctl, 'add', 'short', '/tmp/short.img')
    assert counter('imported_bytes') == 0
Path('/tmp/short.img').unlink()
Path('/tmp/backing.img').write_bytes(raw)
run(ctl, 'add', 'copy', '/tmp/backing.img')
Path('/tmp/backing.img').unlink()
assert counter('imported_bytes') == len(raw)
for _ in range(2):
    run('/mount_femtofs', 'copy', mount)
    assert (mount/'identity').read_text() == 'FemtoFS data\n'
    refuse(ctl, 'discard', 'copy')
    run('umount', mount)
    assert counter('imported_bytes') == len(raw)
refuse('kldunload', 'femtofs')
run(ctl, 'discard', 'copy')
assert counter('imported_bytes') == 0

# Competing publications have one winner and no losing allocation leak.
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    results = list(pool.map(lambda _: subprocess.run(
        [ctl, 'add', 'race', '/runtime/boot/data.img'], capture_output=True).returncode, range(2)))
assert sorted(results) == [0, 1], results
assert counter('imported_bytes') == len(raw)
run(ctl, 'discard', 'race')
# A mount/discard race can publish a claim or discard; never both.
for _ in range(10):
    run(ctl, 'add', 'race', '/runtime/boot/data.img')
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        a = pool.submit(subprocess.run, ['/mount_femtofs', 'race', str(mount)], capture_output=True)
        b = pool.submit(subprocess.run, [ctl, 'discard', 'race'], capture_output=True)
        mounted, discarded = a.result().returncode == 0, b.result().returncode == 0
    assert mounted != discarded
    if mounted:
        assert (mount/'identity').read_text() == 'FemtoFS data\n'
        run('umount', mount)
        run(ctl, 'discard', 'race')
assert counter('imported_bytes') == 0
run('/test-bank', '/runtime/boot/data.img')
# Listing during mutation must return a coherent snapshot or an explicit retry error.
def mutate():
    for _ in range(20):
        run(ctl, 'add', 'listing', '/runtime/boot/data.img')
        run(ctl, 'discard', 'listing')
with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
    mutation = pool.submit(mutate)
    for _ in range(20):
        result = subprocess.run([ctl, 'list'], capture_output=True, text=True)
        if result.returncode:
            assert 'temporarily unavailable' in result.stderr.lower()
        else:
            rows = [line.split('\t')[0] for line in result.stdout.splitlines()[1:]]
            assert len(rows) == len(set(rows)) and {'root', 'data'} <= set(rows)
    mutation.result()
run(sys.executable, '/test-mount.py', '/makefemtofs', '/mount_femtofs', '/test-mmap', '/tmp')
assert counter('imported_bytes') == 0 and counter('loader_bytes') == loader_total
pages = counter('loader_released_pages')
# v_page_count increases when reserved boot pages are returned to VM management.
managed_before = int(subprocess.check_output(['sysctl', '-n', 'vm.stats.vm.v_page_count']))
run(ctl, 'discard', 'data')
assert counter('loader_released_pages') == pages + data_size//4096
managed_after = int(subprocess.check_output(['sysctl', '-n', 'vm.stats.vm.v_page_count']))
assert managed_after - managed_before == data_size//4096, (managed_before, managed_after)
print(f'FEMTOFS_BOOT loader discard reclaimed {managed_after - managed_before} pages '
      f'for {len(raw)} logical / {data_size} retained bytes', flush=True)
assert counter('loader_bytes') == root_size
assert counter('loader_image_bytes') == root_size
metadata = subprocess.check_output(['sysctl', '-n', 'debug.dump_modinfo']).decode()
assert '/boot/data.img' not in metadata
refuse('/mount_femtofs', 'data', mount)
# A discarded label can be reused, now with imported storage.
run(ctl, 'add', 'data', '/runtime/boot/data.img')
run('/mount_femtofs', 'data', mount)
assert (mount/'identity').read_text() == 'FemtoFS data\n'
run('umount', mount)
run(ctl, 'discard', 'data')
if Path('/etc/guest-mode').read_text() == 'control':
    run(ctl, 'discard', 'root')
    assert counter('loader_bytes') == 0 and counter('metadata_bytes') == 0
    assert counter('loader_image_bytes') == 0
    run('kldunload', 'femtofs')
    assert not Path('/dev/femtofs').exists()
    run('kldload', '/boot/kernel/femtofs.ko')
    assert len(subprocess.check_output([ctl, 'list']).splitlines()) == 1
    run('kldunload', 'femtofs')
print('FEMTOFS_BOOT ALL TESTS PASSED', flush=True)
# END File: ko/test-boot-guest.py
