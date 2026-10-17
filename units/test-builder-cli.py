#!/usr/bin/env python3
# File: units/test-builder-cli.py
# Created by Andrea "Nemesi" Cocito on 14/10/2026
# Public CLI fixtures with specification-derived byte and failure assertions.
import io
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

BUILDER, WORK = map(lambda value: str(Path(value).resolve()), sys.argv[1:3])
del sys.argv[1:3]
UUID = '00112233-4455-6677-8899-aabbccddeeff'
PRIMES = [n for n in range(3, 252) if all(n % d for d in range(2, int(n ** .5) + 1))]



def fnv(data):
    state = 0x811c9dc5
    for byte in data:
        state = ((state * 0x01000193) & 0xffffffff) ^ byte
    return state


class BuilderCLI(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='builder-cli.', dir=WORK)
        self.root = Path(self.temporary.name)
        self.serial = 0

    def tearDown(self):
        self.temporary.cleanup()

    def build(self, source, *args, output=None, error=None):
        self.serial += 1
        output = output or self.root / f'{self.serial}.img'
        result = subprocess.run([BUILDER, '--temp-dir', str(self.root), '--uuid', UUID,
                                 *args, str(source), str(output)], capture_output=True, text=True)
        if error:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn(error, result.stderr)
        else:
            self.assertEqual(result.returncode, 0, result.stderr)
        return output

    def archive(self, entries, mode='w', format=tarfile.PAX_FORMAT):
        self.serial += 1
        path = self.root / f'{self.serial}.tar'
        with tarfile.open(path, mode, format=format) as archive:
            for fields in entries:
                name, kind, data = fields[:3]
                entry = tarfile.TarInfo(name)
                entry.mode = 0o755 if kind == 'dir' else 0o644
                if kind == 'dir':
                    entry.type = tarfile.DIRTYPE
                elif kind == 'link':
                    entry.type = tarfile.LNKTYPE
                    entry.linkname = data
                elif kind == 'symlink':
                    entry.type = tarfile.SYMTYPE
                    entry.mode = 0o777
                    entry.linkname = data
                elif kind == 'fifo':
                    entry.type = tarfile.FIFOTYPE
                elif kind == 'device':
                    entry.type = tarfile.CHRTYPE
                else:
                    entry.size = len(data)
                if len(fields) == 4:
                    entry.mode = fields[3]
                archive.addfile(entry, io.BytesIO(data) if kind == 'file' else None)
        return path

    def image(self, path):
        image = path.read_bytes()
        self.assertEqual(image[:8], b'0FS\0\0\4\0\1')
        self.assertEqual(struct.unpack_from('<I', image, 32)[0], len(image))
        self.assertEqual(struct.unpack_from('<I', image, 8)[0], fnv(image[16:]))
        size = struct.unpack_from('<I', image, 48)[0]
        self.assertEqual(struct.unpack_from('<I', image, 12)[0], fnv(image[128:128 + size]))
        self.assert_layout(image)
        return image

    def assert_layout(self, image):
        # Assertions on producer fixtures, derived from the specification. This is
        # deliberately not exposed as a general hostile-image validator.
        u32 = lambda offset: struct.unpack_from('<I', image, offset)[0]
        count, public, private, meta = struct.unpack_from('<IIII', image, 36)
        self.assertTrue(0 < count < 65536)
        self.assertEqual(meta, count * 16)
        self.assertTrue(128 + meta <= public <= private <= len(image))
        self.assertTrue(all(value % 4096 == 0 for value in (public, private, len(image))))
        self.assertEqual(image[67], 0)
        author = b'Andrea "Nemesi" Cocito - blackye at gmail dot com'
        self.assertEqual(image[72:128], author + bytes(56 - len(author)))
        cells = [struct.unpack_from('<BBHIII', image, 128 + 16 * i) for i in range(count)]
        root_attr = struct.unpack_from('<H', image, 64)[0]
        attributes, referenced_attrs = {}, {root_attr}
        for index, cell in enumerate(cells):
            if cell[0] == 0x81:
                self.assertEqual((cell[1], cell[5]), (0, 0))
                key = cell[2:]
                self.assertNotIn(key, attributes.values())
                attributes[index] = key
        self.assertIn(root_attr, attributes)
        self.assertEqual(stat.S_IFMT(cells[root_attr][2]), stat.S_IFDIR)
        slices, objects, blobs, dual = set(), {}, {}, []
        self.lookup_comparisons = 0

        def blob(offset, size, domain, visible):
            end = offset + ((size + 4) & ~3)
            self.assertEqual(offset % 4, 0)
            self.assertTrue(public <= offset < len(image))
            self.assertLessEqual(end, private if offset < private else len(image))
            self.assertEqual(image[offset + size:end], bytes(end - offset - size))
            if end - offset >= 4096:
                self.assertEqual(offset % 4096, 0)
            else:
                self.assertEqual(offset // 4096, (end - 1) // 4096)
            key = (domain, image[offset:offset + size])
            if key in blobs:
                old_offset, old_end, old_visible = blobs[key]
                self.assertEqual((offset, end), (old_offset, old_end))
                visible |= old_visible
            blobs[key] = (offset, end, visible)

        def directory(first, size, real, control, parent, ancestors):
            self.assertTrue(real < 32768 and real <= size <= 65521)
            mode, prime = control >> 6, control & 63
            self.assertIn(mode, (0, 1))
            self.assertLess(prime, 53)
            dual.append(mode == 1)
            self.assertLessEqual(first + size, count)
            if not size:
                self.assertEqual((first, real), (0, 0))
            claimed = set(range(first, first + size))
            self.assertFalse(slices & claimed)
            slices.update(claimed)
            names, occupied = set(), 0
            for relative in range(size):
                kind, hash_p, attr, target, name_offset, next_index = cells[first + relative]
                if kind == 0:
                    self.assertEqual((hash_p, attr, target, name_offset, next_index), (0, 0, 0, 0, size))
                    continue
                if kind == 0x81:
                    continue
                self.assertIn(kind, range(1, 6))
                self.assertEqual((hash_p, attr), (0, 0))
                self.assertLessEqual(next_index, size)
                self.assertLess(target, count)
                self.assertNotIn(target, objects)
                self.assertEqual(cells[target][0], kind)
                part_end = private if name_offset < private else len(image)
                terminator = image.index(0, name_offset, part_end)
                name = image[name_offset:terminator]
                self.assertTrue(1 <= len(name) <= 255)
                self.assertNotIn(b'/', name)
                self.assertNotIn(name, (b'.', b'..'))
                self.assertNotIn(name, names)
                names.add(name)
                occupied += 1
                blob(name_offset, len(name), 'shared', all(m & 5 == 5 for m in ancestors))
                objects[target] = (ancestors, parent)
                bases = [PRIMES[prime]] + ([u32(68)] if mode else [])
                anchors = []
                for base in bases:
                    hashed = 0
                    for byte in name:
                        hashed = (hashed * base + byte) & 0xffffffff
                    if hashed % size not in anchors:
                        anchors.append(hashed % size)
                reached = False
                lookup_done = False
                for anchor in anchors:
                    visited = set()
                    if cells[first + anchor][0] in (0, 0x81):
                        continue
                    while anchor != size:
                        self.assertTrue(0 <= anchor < size)
                        self.assertNotIn(anchor, visited)
                        visited.add(anchor)
                        bucket = cells[first + anchor]
                        self.assertIn(bucket[0], range(1, 6))
                        if not lookup_done:
                            self.lookup_comparisons += 1
                            lookup_done = anchor == relative
                        reached |= anchor == relative
                        anchor = bucket[5]
                self.assertTrue(reached)
                obj = cells[target]
                if kind != 4:
                    self.assertIn(obj[2], attributes)
                    referenced_attrs.add(obj[2])
                    self.assertEqual(stat.S_IFMT(cells[obj[2]][2]),
                                     {1: stat.S_IFREG, 2: stat.S_IFDIR, 3: stat.S_IFLNK, 5: stat.S_IFIFO}[kind])
                if kind == 2:
                    self.assertEqual(obj[5] >> 31, 0)
                    self.assertEqual(obj[5] & 65535, parent)
                    directory(obj[3], obj[4], obj[5] >> 16, obj[1], target,
                              ancestors + [cells[obj[2]][2]])
                else:
                    self.assertEqual(obj[1], 0)
                    self.assertEqual(obj[5], 0)
                    if kind in (4, 5):
                        self.assertEqual(obj[4], 0)
                        if kind == 4:
                            self.assertEqual(obj[2], 0)
                        else:
                            self.assertEqual(obj[3], 0)
            self.assertEqual(occupied, real)

        directory(u32(52), u32(56), u32(60), image[66], 65535, [cells[root_attr][2]])
        self.assertLess(len(objects), 32768)
        self.assertFalse(set(objects) & slices)
        self.assertEqual(referenced_attrs, set(attributes))
        if any(dual):
            base = u32(68)
            self.assertTrue(256 < base < 2**24)
            self.assertTrue(all(base % d for d in range(2, int(base ** .5) + 1)))
        else:
            self.assertEqual(u32(68), 0)
        for index, (ancestors, _) in objects.items():
            obj = cells[index]
            if obj[0] == 4:
                self.assertIn(obj[3], objects)
                self.assertEqual(cells[obj[3]][0], 1)
                obj = cells[obj[3]]
            if obj[0] in (1, 3):
                visible = obj[0] == 1 and cells[obj[2]][2] & 4 != 0 and all(m & 1 for m in ancestors)
                blob(obj[3], obj[4], 'symlink' if obj[0] == 3 else 'shared', visible)
                if obj[0] == 3:
                    self.assertNotIn(0, image[obj[3]:obj[3] + obj[4]])
        for index, cell in enumerate(cells):
            if index not in slices and index not in objects and index not in attributes:
                self.assertEqual(cell, (0, 0, 0, 0, 0, 0))
        cursor = 128 + meta
        for start, end, visible in sorted(blobs.values()):
            self.assertLessEqual(cursor, start)
            self.assertEqual(image[cursor:start], bytes(start - cursor))
            self.assertEqual(start < private, bool(visible))
            cursor = end
        self.assertEqual(image[cursor:], bytes(len(image) - cursor))

    def test_golden_empty_image(self):
        source = self.root / 'empty'
        source.mkdir()
        actual = self.image(self.build(source, '--root-mode', '755', '--root-uid', '0', '--root-gid', '0'))
        expected = bytearray(4096)
        expected[:8] = b'0FS\0\0\4\0\1'
        expected[16:32] = bytes.fromhex(UUID.replace('-', ''))
        struct.pack_into('<IIIII', expected, 32, 4096, 1, 4096, 4096, 16)
        author = b'Andrea "Nemesi" Cocito - blackye at gmail dot com'
        expected[72:72 + len(author)] = author
        struct.pack_into('<BBHIII', expected, 128, 0x81, 0, 0o40755, 0, 0, 0)
        struct.pack_into('<I', expected, 12, fnv(expected[128:144]))
        struct.pack_into('<I', expected, 8, fnv(expected[16:]))
        self.assertEqual(actual, expected)
        self.assertNotEqual(fnv(actual[17:]), fnv(actual[16:]))

    def test_tar_formats_filters_and_order(self):
        entries = [('.', 'dir', ''), ('dir', 'dir', ''), ('dir/a', 'file', b'payload'),
                   ('empty', 'file', b''), ('alias', 'link', 'dir/a'),
                   ('sym', 'symlink', 'dir/a'), ('pipe', 'fifo', '')]
        expected = None
        for fmt in (tarfile.USTAR_FORMAT, tarfile.PAX_FORMAT, tarfile.GNU_FORMAT):
            for mode in ('w', 'w:gz', 'w:bz2', 'w:xz'):
                with self.subTest(format=fmt, filter=mode):
                    result = self.image(self.build(self.archive(entries, mode, fmt)))
                    if expected is None:
                        expected = result
                    self.assertEqual(result, expected)
        # Forward hardlink and children preceding their parents.
        self.assertEqual(self.image(self.build(self.archive(list(reversed(entries))))), expected)

    def test_cpio(self):
        source = self.root / 'cpio-tree'
        source.mkdir()
        (source / 'payload').write_bytes(b'cpio payload')
        archive = self.root / 'source.cpio'
        result = subprocess.run(['tar', '-cf', str(archive), '--format=newc',
                                 '-C', str(source), '.'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.image(self.build(archive)), self.image(self.build(source)))

    def test_builder_and_simulator_lookup_counts(self):
        names = Path(__file__).with_name('dual-hash-names.txt').read_text().splitlines()
        source = self.archive([(name, 'file', b'') for name in names])
        self.image(self.build(source))
        comparisons = self.lookup_comparisons
        self.assertEqual(comparisons, 80)
        simulator = Path(BUILDER).with_name('femtofsSim')
        listing = self.root / 'listing'
        listing.write_text('1 1 drwxr-xr-x 2 root wheel 0 Oct 14 2026 ./\n' + ''.join(
            f'{i + 2} 1 -rw-r--r-- 1 root wheel 0 Oct 14 2026 ./{name}\n'
            for i, name in enumerate(names)))
        result = subprocess.run([str(simulator), str(listing), '--no-budgeted-hash-experiment'],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        import re
        count = re.search(r'^  successful lookup comparisons: +([0-9]+)$', result.stdout, re.M)
        self.assertIsNotNone(count)
        self.assertEqual(int(count.group(1)), comparisons)
        reverse = self.archive([(name, 'file', b'') for name in reversed(names)])
        self.assertEqual(self.image(self.build(source)), self.image(self.build(reverse)))

    def test_zip(self):
        source = self.root / 'source.zip'
        with zipfile.ZipFile(source, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            entry = zipfile.ZipInfo('payload')
            entry.create_system = 3
            entry.external_attr = (stat.S_IFREG | 0o644) << 16
            archive.writestr(entry, b'zip payload')
        image = self.image(self.build(source))
        self.assertIn(b'zip payload\0', image)

    def test_malformed_sources(self):
        cases = [
            ([('a', 'file', b''), ('a', 'file', b'')], 'duplicate normalized'),
            ([('../a', 'file', b'')], 'invalid component'),
            ([('/a', 'file', b'')], 'absolute pathname'),
            ([('a/./b', 'file', b'')], 'invalid component'),
            ([('a//b', 'file', b'')], 'invalid component'),
            ([('x' * 256, 'file', b'')], 'exceeds 255'),
            ([('missing/a', 'file', b'')], 'missing parent'),
            ([('a', 'file', b''), ('a/b', 'file', b'')], 'not a directory'),
            ([('d', 'dir', ''), ('a', 'link', 'd')], 'not a regular file'),
            ([('a', 'link', 'missing')], 'target does not exist'),
            ([('device', 'device', '')], 'unsupported'),
            ([('a', 'file', b'x', 0o600), ('b', 'link', 'a', 0o644)], 'inconsistent attributes'),
        ]
        for entries, error in cases:
            with self.subTest(error=error):
                output = self.build(self.archive(entries), error=error)
                self.assertFalse(output.exists())
        normalized = self.archive([('././a', 'file', b'a')])
        plain = self.archive([('a', 'file', b'a')])
        self.assertEqual(self.image(self.build(normalized)), self.image(self.build(plain)))
        missing = self.archive([('missing/a', 'file', b'a')])
        self.image(self.build(missing, '--synthesize-dirs'))

    def test_mtree_is_rejected(self):
        payload = self.root / 'payload'
        payload.write_bytes(b'not empty')
        source = self.root / 'METALOG'
        source.write_text('./payload type=file mode=0644 contents=' + str(payload) + '\n')
        output = self.build(source, error='mtree/METALOG input is unsupported')
        self.assertFalse(output.exists())

    def test_empty_archives_are_rejected(self):
        sources = [self.root / 'zero-byte.tar', self.archive([])]
        sources[0].write_bytes(b'')
        for source in sources:
            with self.subTest(source=source.name):
                output = self.build(source, error='input archive contains no entries')
                self.assertFalse(output.exists())

    def test_empty_hardlink_group(self):
        source = self.archive([('a', 'link', 'b'), ('b', 'link', 'a')])
        self.image(self.build(source))

    def test_publication_and_aliases(self):
        source = self.archive([('a', 'file', b'payload')])
        destination = self.root / 'existing.img'
        destination.write_bytes(b'previous bytes')
        self.build(source, output=destination, error='output exists')
        self.assertEqual(destination.read_bytes(), b'previous bytes')
        self.image(self.build(source, '--force', output=destination))
        original = source.read_bytes()
        self.build(source, '--force', output=source, error='same file')
        self.assertEqual(source.read_bytes(), original)
        for dangling in (False, True):
            link = self.root / f'link-{dangling}'
            link.symlink_to(self.root / 'absent' if dangling else destination)
            self.build(source, '--force', output=link, error='symlink')
            self.assertTrue(link.is_symlink())
        directory = self.root / 'tree'
        directory.mkdir()
        self.build(directory, output=directory / 'inside.img', error='inside')
        self.build(source, '--device', error='existing block or character device')

    def test_visibility_and_page_edges(self):
        entries = [('.', 'dir', ''), ('hidden', 'dir', '', 0o700),
                   ('searchable', 'dir', '', 0o711),
                   ('hidden/a', 'file', b'shared'), ('visible', 'link', 'hidden/a'),
                   ('shared', 'file', b'different'), ('sym', 'symlink', 'shared'),
                   ('searchable/name', 'file', b'readable'),
                   ('private', 'file', b'secret', 0o600)]
        entries += [(str(size), 'file', b'x' * size) for size in
                    (0, 1, 3, 4, 4091, 4092, 4095, 4096, 4097, 8191, 8192)]
        archive = self.archive(entries)
        self.image(self.build(archive))
        self.image(self.build(archive, '--root-mode', '700'))

    def test_sparse_directory_payload(self):
        source = self.root / 'tree'
        source.mkdir()
        with (source / 'sparse').open('wb') as output:
            output.write(b'head')
            output.seek(1024 * 1024)
            output.write(b'tail')
        image = self.image(self.build(source))
        offset, size = struct.unpack_from('<II', image, 128 + 4)
        self.assertEqual(image[offset:offset + size], (source / 'sparse').read_bytes())
        archive = self.root / 'sparse.pax'
        result = subprocess.run(['tar', '-cf', str(archive), '--format=pax',
                                 '-C', str(source), '.'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.image(self.build(archive)), image)


if __name__ == '__main__':
    unittest.main(verbosity=2)
# END File: units/test-builder-cli.py
