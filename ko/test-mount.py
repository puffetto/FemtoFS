#!/usr/bin/env python3
# File: ko/test-mount.py
# Created by Andrea "Nemesi" Cocito on 14/10/2026
# Disposable FreeBSD provider and mounted VFS regression matrix.
import concurrent.futures
import errno
import os
from pathlib import Path
import platform
import pwd
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

def run(*args, **kw):
    return subprocess.run([str(x) for x in args], check=True, **kw)

if platform.system() != 'FreeBSD' or os.geteuid() != 0:
    sys.exit('This test requires FreeBSD and root; it only uses its own md providers.')
builder, helper, probe, work = [Path(p).resolve() for p in sys.argv[1:5]]
base = Path(tempfile.mkdtemp(prefix='ko-mount.', dir=work))
source, mountpoint = base / 'source', base / 'mount'
source.mkdir(mode=0o755); mountpoint.mkdir()
base.chmod(0o755)
for size in (0, 1, 4095, 4096, 4097, 8192, 10000):
    p = source / f'file-{size}'
    p.write_bytes(bytes(i % 251 + 1 for i in range(size)))
    p.chmod(0o644)
(source / 'private').write_bytes(b'secret' * 2000)
(source / 'private').chmod(0o600)
(source / 'hidden').mkdir(mode=0o700)
(source / 'hidden' / 'promoted').write_bytes(b'promoted')
(source / 'hidden' / 'promoted').chmod(0o644)
os.link(source / 'hidden' / 'promoted', source / 'visible')
os.link(source / 'file-4097', source / 'hardlink')
(source / 'symlink').symlink_to('file-4097')
os.mkfifo(source / 'fifo', 0o644)
for i in range(300):
    (source / f'entry-{i:03}').write_bytes(b'dedup')
image = base / 'image'
run(builder, '--temp-dir', base, source, image)
raw = image.read_bytes()
u32 = lambda off: struct.unpack_from('<I', raw, off)[0]
root, rootsize, private = u32(52), u32(56), u32(44)
files = {}
for i in range(rootsize):
    b = 128 + (root+i)*16
    if raw[b] not in (1, 4):
        continue
    nameoff = u32(b+8)
    name = raw[nameoff:raw.index(b'\0', nameoff)].decode()
    obj = 128 + u32(b+4)*16
    if raw[obj] == 4:
        obj = 128 + u32(obj+4)*16
    files[name] = u32(obj+4), u32(obj+8)
unit = None
mounted = False
try:
    # Real mount rejection must release GEOM and allocation state each time.
    for byteoff in (0, 5, 7, 12, 67, 72):
        corrupt = bytearray(raw)
        corrupt[byteoff] ^= 128
        damaged = base / 'damaged'
        damaged.write_bytes(corrupt)
        unit = subprocess.check_output(['mdconfig', '-a', '-t', 'vnode', '-f', str(damaged)]).decode().strip()
        assert unit.startswith('md') and unit[2:].isdigit()
        result = subprocess.run([str(helper), '/dev/'+unit, str(mountpoint)], capture_output=True)
        assert result.returncode != 0, f'corrupted header {byteoff} mounted'
        run('mdconfig', '-d', '-u', unit); unit = None
    print('six malformed-image mounts rejected and providers detached', flush=True)
    for sector in (512, 4096):
        unit = subprocess.check_output(['mdconfig', '-a', '-t', 'vnode', '-f', str(image), '-S', str(sector)]).decode().strip()
        assert unit.startswith('md') and unit[2:].isdigit()
        device = '/dev/' + unit
        bad = subprocess.run([str(helper), '-o', 'dirty', device, str(mountpoint)], capture_output=True)
        assert bad.returncode != 0, 'deferred dirty mode was accepted'
        for mode in ('clean', 'leaking'):
            run(helper, '-o', mode, device, mountpoint)
            mounted = True
            run(probe.with_name('test-directory'), mountpoint)
            seen = 0
            for original in [source, *source.rglob('*')]:
                rel = original.relative_to(source)
                target = mountpoint / rel
                a, b = original.lstat(), target.lstat()
                assert stat.S_IFMT(a.st_mode) == stat.S_IFMT(b.st_mode), rel
                assert stat.S_IMODE(a.st_mode) == stat.S_IMODE(b.st_mode), rel
                assert (a.st_uid, a.st_gid, a.st_nlink) == (b.st_uid, b.st_gid, b.st_nlink), rel
                if stat.S_ISREG(a.st_mode):
                    assert target.read_bytes() == original.read_bytes(), rel
                elif stat.S_ISLNK(a.st_mode):
                    assert os.readlink(target) == os.readlink(original)
                elif stat.S_ISDIR(a.st_mode):
                    assert sorted(p.name for p in target.iterdir()) == sorted(p.name for p in original.iterdir())
                seen += 1
            assert (mountpoint/'hardlink').stat().st_ino == (mountpoint/'file-4097').stat().st_ino
            assert (mountpoint/'entry-000').stat().st_ino != (mountpoint/'entry-001').stat().st_ino
            nobody = pwd.getpwnam('nobody')
            def unprivileged():
                os.setgroups([]); os.setgid(nobody.pw_gid); os.setuid(nobody.pw_uid)
            run(sys.executable, '-c', '''
import errno, pathlib, sys
root = pathlib.Path(sys.argv[1])
assert (root/'file-1').read_bytes()
for path in ('private', 'hidden/promoted'):
    try: (root/path).read_bytes()
    except OSError as e: assert e.errno == errno.EACCES
    else: raise AssertionError('private path readable')
''', mountpoint, preexec_fn=unprivileged)
            for action in (lambda: (mountpoint/'new').write_bytes(b'x'),
                           lambda: (mountpoint/'file-1').chmod(0o600),
                           lambda: (mountpoint/'file-1').unlink()):
                try:
                    action()
                except OSError as error:
                    assert error.errno == errno.EROFS, error
                else:
                    raise AssertionError('write operation succeeded')
            # Exercise concurrent namecache hits, misses, reads and directory iteration.
            def reader(index):
                for _ in range(20):
                    assert (mountpoint/f'entry-{index:03}').read_bytes() == b'dedup'
                    assert not (mountpoint/'missing').exists()
                    assert len(list(mountpoint.iterdir())) == len(list(source.iterdir()))
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                list(pool.map(reader, range(8)))
            # FIFO I/O must remain usable on a read-only filesystem.
            fd = os.open(mountpoint/'fifo', os.O_RDWR | os.O_NONBLOCK)
            assert os.write(fd, b'fifo') == 4 and os.read(fd, 4) == b'fifo'
            os.close(fd)
            page = os.sysconf('SC_PAGE_SIZE')
            for name in ('file-0', 'file-1', 'file-4095', 'file-4096', 'file-4097', 'file-8192', 'file-10000', 'private', 'visible'):
                off, size = files[name]
                rounded = (size+page-1)//page*page
                leaking = mode == 'leaking' and page <= 4096 and size >= page and off % page == 0 and off+rounded <= private
                expected = raw[off:off+rounded] if leaking else raw[off:off+size] + b'\0'*(rounded-size)
                expected_file = base / 'expected'
                expected_file.write_bytes(expected)
                run(probe, mountpoint/name, expected_file, mountpoint)
            run('umount', mountpoint); mounted = False
            print(f'{mode}, sector={sector}: {seen} objects; metadata, reads, names, FIFO, concurrency and mmap passed', flush=True)
        run('mdconfig', '-d', '-u', unit); unit = None
finally:
    # Preserve artifacts if cleanup fails; never detach a still-mounted provider.
    if mounted:
        run('umount', mountpoint)
    if unit:
        run('mdconfig', '-d', '-u', unit)
    shutil.rmtree(base)
# END File: ko/test-mount.py
