#!/usr/bin/env python3
# File: ko/test-image.py
# Created by Andrea "Nemesi" Cocito on 14/10/2026
# Specification-derived image fixtures and hostile-input decoder regressions.
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest

BUILDER, CHECK, WORK = [str(Path(p).resolve()) for p in sys.argv[1:4]]
del sys.argv[1:4]

def u32(b, off):
    return struct.unpack_from('<I', b, off)[0]

def put(b, off, value):
    struct.pack_into('<I', b, off, value)

def rehash(b):
    h = 0x811c9dc5
    for byte in b[128:128 + u32(b, 48)]:
        h = ((h * 0x01000193) & 0xffffffff) ^ byte
    put(b, 12, h)

class Images(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(dir=WORK, prefix='ko-image.')
        cls.base = Path(cls.temp.name)
        src = cls.base / 'source'
        src.mkdir(mode=0o755)
        (src / 'hidden').mkdir(mode=0o700)
        (src / 'empty-dir').mkdir(mode=0o755)
        for size in (0, 1, 15, 4095, 4096, 4097, 8192, 10000):
            p = src / f'file-{size}'
            p.write_bytes(bytes((i % 251 + 1 for i in range(size))))
            p.chmod(0o644)
        (src / 'private').write_bytes(b'PRIVATE' * 1200)
        (src / 'private').chmod(0o600)
        (src / 'hidden' / 'promoted').write_bytes(b'promoted payload')
        (src / 'hidden' / 'promoted').chmod(0o644)
        os.link(src / 'hidden' / 'promoted', src / 'visible-link')
        os.link(src / 'file-4097', src / 'hardlink')
        (src / 'symlink').symlink_to('file-4097')
        os.mkfifo(src / 'fifo', 0o644)
        for name in Path('units/dual-hash-names.txt').read_text().splitlines():
            (src / name).write_bytes(b'deduplicated')
        image = cls.base / 'valid.img'
        subprocess.run([BUILDER, '--temp-dir', str(cls.base), str(src), str(image)], check=True, capture_output=True)
        cls.good = image.read_bytes()
        cls.root = 128 + 16 * u32(cls.good, 52)
        cls.bucket = next(cls.root + 16*i for i in range(u32(cls.good, 56))
                          if 1 <= cls.good[cls.root+16*i] <= 5)
        cls.objects = {}
        # Only root objects are needed for the targeted corruptions.
        for i in range(u32(cls.good, 56)):
            b = cls.root + 16*i
            if 1 <= cls.good[b] <= 5:
                off = u32(cls.good, b+8)
                name = cls.good[off:cls.good.index(b'\0', off)].decode()
                cls.objects[name] = 128 + 16*u32(cls.good, b+4)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check(self, b, valid=False, checksum=True):
        b = bytearray(b)
        if checksum and len(b) >= 128:
            rehash(b)
        p = self.base / 'candidate.img'
        p.write_bytes(b)
        result = subprocess.run([CHECK, str(p)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0 if valid else 1, result.stdout + result.stderr)

    def test_valid_tree_and_pages(self):
        self.check(self.good, valid=True)

    def test_future_revision(self):
        b = bytearray(self.good)
        b[6] = 7
        self.check(b, valid=True)

    def test_headers(self):
        for off, value in ((0, 1), (4, 1), (5, 3), (7, 2), (67, 1), (72, 0), (127, 1)):
            with self.subTest(off=off):
                b = bytearray(self.good); b[off] = value; self.check(b)
        for off, value in ((32, 0xffffffff), (36, 65536), (40, 128), (44, 1),
                           (48, 0), (52, 65536), (56, 65522), (60, 32768)):
            with self.subTest(off=off):
                b = bytearray(self.good); put(b, off, value); self.check(b)

    def test_truncations(self):
        for length in (0, 1, 127, 128, 4096, len(self.good)-1):
            self.check(self.good[:length], checksum=False)

    def test_metadata_hash(self):
        b = bytearray(self.good); b[12] ^= 1
        self.check(b, checksum=False)

    def test_bucket_and_chains(self):
        for delta, value in ((4, 65536), (4, u32(self.good, 52)), (8, 0),
                             (12, u32(self.good, 56)+1), (12, (self.bucket-self.root)//16)):
            with self.subTest(delta=delta, value=value):
                b = bytearray(self.good); put(b, self.bucket+delta, value); self.check(b)
        b = bytearray(self.good); b[self.bucket+1] = 1; self.check(b)

    def test_payload_bounds_and_terminator(self):
        obj = self.objects['file-4097']
        for delta, value in ((4, 0), (4, 0xfffffffc), (8, 0xffffffff), (12, 1)):
            b = bytearray(self.good); put(b, obj+delta, value); self.check(b)
        b = bytearray(self.good); b[u32(b, obj+4)+4097] = 1; self.check(b)

    def test_parent_and_slice(self):
        obj = self.objects['hidden']
        for delta, value in ((4, u32(self.good, 52)), (12, 0x8000ffff), (12, 0)):
            b = bytearray(self.good); put(b, obj+delta, value); self.check(b)

    def test_hardlink(self):
        obj = self.objects['hardlink']
        # Depending on lexical canonical selection, choose the actual link cell.
        if self.good[obj] != 4:
            obj = self.objects['file-4097']
        self.assertEqual(self.good[obj], 4)
        for value in ((obj-128)//16, (self.objects['hidden']-128)//16, 65535):
            b = bytearray(self.good); put(b, obj+4, value); self.check(b)

    def test_symlink_and_visibility(self):
        obj = self.objects['symlink']
        b = bytearray(self.good); b[u32(b, obj+4)] = 0; self.check(b)
        # Changing the private split cannot promote private symlink/regular data.
        b = bytearray(self.good); put(b, 44, len(b)); self.check(b)

    def test_attribute_mode_and_extension(self):
        obj = self.objects['private']
        attr = 128 + 16 * struct.unpack_from('<H', self.good, obj+2)[0]
        b = bytearray(self.good); b[attr+3] = 0x40; self.check(b)
        b = bytearray(self.good); put(b, attr+12, 4); self.check(b)
        b[6] = 1; self.check(b, valid=True)

    def test_deterministic_mutations_do_not_crash(self):
        rng = random.Random(20261014)
        p = self.base / 'mutation.img'
        for _ in range(250):
            b = bytearray(self.good)
            for _ in range(rng.randrange(1, 5)):
                b[rng.randrange(len(b))] ^= 1 << rng.randrange(8)
            rehash(b)
            p.write_bytes(b)
            result = subprocess.run([CHECK, str(p)], capture_output=True)
            self.assertIn(result.returncode, (0, 1), result.stderr)

if __name__ == '__main__':
    unittest.main()
# END File: ko/test-image.py
