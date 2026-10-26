#!/usr/bin/env python3
"""
Builds a tiny, spec-conformant femtoFS image for exercising fsck_femtofs:

  /            (root dir, tablesize=1)
    hello.txt  (a small public FILE)

Deliberately minimal: single-hash mode, no dedup edge cases, no hardlinks,
no symlinks, no metaext. Good enough to prove the checker accepts a clean
image and correctly rejects targeted corruptions of it.
"""
import struct

PAGE = 4096
FNV1_32_INIT = 0x811c9dc5
FNV_32_PRIME = 0x01000193

AUTHOR_FIELD = 'Andrea "Nemesi" Cocito - blackye at gmail dot com'.encode("ascii").ljust(56, b"\x00")

def bucket_word2(name, next_index):
    # bucket bytes 12..15: reserved (u8 = 0), name_len (u8), next_index (u16)
    return (len(name) << 8) | (next_index << 16)

def fnv1_32(data, hval=FNV1_32_INIT):
    for b in data:
        hval = (hval * FNV_32_PRIME) & 0xFFFFFFFF
        hval ^= b
    return hval

def align_up(n, a):
    return (n + a - 1) // a * a

def build():
    # ---- content blobs -------------------------------------------------
    filename = b"hello.txt"
    payload = b"hi femtoFS\n"

    # filename blob: bytes + NUL + pad to 4
    fn_blob = filename + b"\x00"
    fn_blob += b"\x00" * ((-len(fn_blob)) % 4)

    pl_blob = payload + b"\x00"
    pl_blob += b"\x00" * ((-len(pl_blob)) % 4)

    # public part layout: filename blob then payload blob, packed from 0
    public_part = bytearray()
    name_off_rel = len(public_part)
    public_part += fn_blob
    data_off_rel = len(public_part)
    public_part += pl_blob
    # pad the whole public part up to one page
    public_part += b"\x00" * ((-len(public_part)) % PAGE)
    if len(public_part) == 0:
        public_part = bytearray(PAGE)

    private_part = bytearray()  # nothing private in this minimal image

    # ---- metadata table --------------------------------------------------
    # cell 0: ATTR for root dir
    # cell 1: ATTR for the file
    # cell 2: bucket slice for root (tablesize=1) -> occupied bucket referencing cell 3
    # cell 3: FILE object

    TYPE_FILE = 1
    TYPE_DIR = 2
    TYPE_ATTR = 0x81

    S_IFDIR = 0o040000
    S_IFREG = 0o100000

    cells = []

    def obj_cell(typ, b1, f2, w0, w1, w2):
        return struct.pack("<BBHIII", typ, b1, f2, w0, w1, w2)

    # cell 0: root ATTR (mode=DIR|0755, uid=0,gid=0,ext_off=0)
    root_attr_idx = 0
    cells.append(obj_cell(TYPE_ATTR, 0, S_IFDIR | 0o755, 0, 0, 0))

    # cell 1: file ATTR (mode=REG|0644)
    file_attr_idx = 1
    cells.append(obj_cell(TYPE_ATTR, 0, S_IFREG | 0o644, 0, 0, 0))

    # cell 2: root bucket slot (tablesize=1), occupied, points at object cell 3
    bucket_first = 2
    tablesize = 1
    # size/name_off filled in after we know public_off
    cells.append(None)  # placeholder, fixed up below

    # cell 3: FILE object
    file_cell_idx = 3
    cells.append(None)  # placeholder, data_off filled in below

    cell_count = len(cells)
    meta_size = cell_count * 16

    header_size = 128
    public_off = align_up(header_size + meta_size, PAGE)
    private_off = public_off + len(public_part)
    image_size = private_off + len(private_part)

    name_off = public_off + name_off_rel
    data_off = public_off + data_off_rel

    cells[2] = obj_cell(TYPE_FILE, 0, 0, file_cell_idx, name_off, bucket_word2(filename, tablesize))  # next_index==tablesize sentinel
    cells[3] = obj_cell(TYPE_FILE, 0, file_attr_idx, data_off, len(payload), 0)

    meta_bytes = b"".join(cells)
    assert len(meta_bytes) == meta_size

    # ---- header ---------------------------------------------------------
    root_first = bucket_first
    root_size = tablesize
    root_real = 1
    root_p = 0  # single-hash mode, p1_index=0
    root_pad = 0
    hash2_base = 0

    # header layout per spec (128 bytes total)
    header_wo_hashes_tail = struct.pack(
        "<4s2sH",  # magic, format, version
        b"0FS\x00", bytes([0, 4]), 0x0100,
    )
    # placeholders for image_hash, meta_hash computed after
    uuid = tuple((0x1000 + i) for i in range(4))

    def build_header(image_hash, meta_hash):
        return struct.pack(
            "<4s2sHII16sIIIIIIIIHBBI56s",
            b"0FS\x00", bytes([0, 4]), 0x0100,
            image_hash, meta_hash,
            struct.pack("<4I", *uuid),
            image_size,
            cell_count,
            public_off,
            private_off,
            meta_size,
            root_first,
            root_size,
            root_real,
            root_attr_idx,
            root_p,
            root_pad,
            hash2_base,
            AUTHOR_FIELD,
        )

    meta_hash = fnv1_32(meta_bytes)

    # image_hash covers from uuid (offset 16) to EOF; build once with hash=0
    # then recompute properly.
    header0 = build_header(0, meta_hash)
    assert len(header0) == 128

    image = bytearray()
    image += header0
    image += meta_bytes
    assert len(image) == header_size + meta_size
    image += b"\x00" * (public_off - len(image))
    image += public_part
    image += private_part
    assert len(image) == image_size

    image_hash = fnv1_32(bytes(image[16:]))
    header = build_header(image_hash, meta_hash)
    image[0:128] = header

    return bytes(image)

def build_with_subdir():
    """
    root/
      hello.txt
      sub/            (subdirectory -- exercises DIR-typed bucket entries)
        inner.txt
    """
    TYPE_FILE = 1
    TYPE_DIR = 2
    TYPE_ATTR = 0x81
    S_IFDIR = 0o040000
    S_IFREG = 0o100000

    def obj_cell(typ, b1, f2, w0, w1, w2):
        return struct.pack("<BBHIII", typ, b1, f2, w0, w1, w2)

    # ---- cell plan ------------------------------------------------------
    # 0: ATTR (dir mode)              -- shared by root and sub
    # 1: ATTR (file mode)             -- shared by hello.txt and inner.txt
    # 2: root bucket slot 0 -> object 4 (hello.txt FILE)
    # 3: root bucket slot 1 -> object 6 (sub DIR)
    # 4: FILE object (hello.txt)
    # 5: sub's bucket slot 0 -> object 7 (inner.txt FILE)
    # 6: DIR object (sub)
    # 7: FILE object (inner.txt)
    root_attr_idx = 0
    file_attr_idx = 1

    cells = [None] * 8
    cells[0] = obj_cell(TYPE_ATTR, 0, S_IFDIR | 0o755, 0, 0, 0)
    cells[1] = obj_cell(TYPE_ATTR, 0, S_IFREG | 0o644, 0, 0, 0)
    # cells[2], [3] (root buckets) filled after we know name offsets
    # cells[4] (hello.txt FILE) filled after we know data offsets
    cells[5] = None  # sub bucket, filled later
    # cells[6] (sub DIR object): bucket_first=5, tablesize=1, realsize=pack(parent=ROOT,N=1)
    FEMTOFS_PARENT_ROOT = 0xFFFF
    def pack_dir_realsize(parent, n):
        return (parent & 0xFFFF) | ((n & 0x7FFF) << 16)
    cells[6] = obj_cell(TYPE_DIR, 0, root_attr_idx, 5, 1, pack_dir_realsize(FEMTOFS_PARENT_ROOT, 1))
    # cells[7] (inner.txt FILE) filled later

    cell_count = len(cells)
    meta_size = cell_count * 16
    header_size = 128
    public_off = align_up(header_size + meta_size, PAGE)

    # ---- content blobs ----------------------------------------------------
    def blob(b):
        b = b + b"\x00"
        b += b"\x00" * ((-len(b)) % 4)
        return b

    names = [b"hello.txt", b"sub", b"inner.txt"]
    payloads = {b"hello.txt": b"hi femtoFS\n", b"inner.txt": b"nested!\n"}

    public_part = bytearray()
    offs = {}
    for n in names:
        offs[("name", n)] = public_off + len(public_part)
        public_part += blob(n)
    for n, p in payloads.items():
        offs[("data", n)] = public_off + len(public_part)
        public_part += blob(p)
    public_part += b"\x00" * ((-len(public_part)) % PAGE)
    if len(public_part) == 0:
        public_part = bytearray(PAGE)

    private_part = bytearray()
    private_off = public_off + len(public_part)
    image_size = private_off + len(private_part)

    # With p1=3 and tablesize=2 both "hello.txt" and "sub" hash to anchor 0,
    # so "sub" sits in the free slot 1 chained from slot 0.
    cells[2] = obj_cell(TYPE_FILE, 0, 0, 4, offs[("name", b"hello.txt")], bucket_word2(b"hello.txt", 1))
    cells[3] = obj_cell(TYPE_DIR, 0, 0, 6, offs[("name", b"sub")], bucket_word2(b"sub", 2))  # sentinel=tablesize(2)
    cells[4] = obj_cell(TYPE_FILE, 0, file_attr_idx, offs[("data", b"hello.txt")], len(payloads[b"hello.txt"]), 0)
    cells[5] = obj_cell(TYPE_FILE, 0, 0, 7, offs[("name", b"inner.txt")], bucket_word2(b"inner.txt", 1))  # sentinel=tablesize(1)
    cells[7] = obj_cell(TYPE_FILE, 0, file_attr_idx, offs[("data", b"inner.txt")], len(payloads[b"inner.txt"]), 0)

    meta_bytes = b"".join(cells)
    assert len(meta_bytes) == meta_size

    root_first, root_size, root_real = 2, 2, 2
    root_p = 0
    hash2_base = 0
    uuid = tuple((0x2000 + i) for i in range(4))

    def build_header(image_hash, meta_hash):
        return struct.pack(
            "<4s2sHII16sIIIIIIIIHBBI56s",
            b"0FS\x00", bytes([0, 4]), 0x0100,
            image_hash, meta_hash,
            struct.pack("<4I", *uuid),
            image_size, cell_count, public_off, private_off, meta_size,
            root_first, root_size, root_real,
            root_attr_idx, root_p, 0, hash2_base,
            AUTHOR_FIELD,
        )

    meta_hash = fnv1_32(meta_bytes)
    header0 = build_header(0, meta_hash)
    image = bytearray()
    image += header0
    image += meta_bytes
    image += b"\x00" * (public_off - len(image))
    image += public_part
    image += private_part
    assert len(image) == image_size
    image_hash = fnv1_32(bytes(image[16:]))
    image[0:128] = build_header(image_hash, meta_hash)
    return bytes(image)


if __name__ == "__main__":
    import sys
    which = sys.argv[1] if len(sys.argv) > 1 else "flat"
    out = sys.argv[2] if len(sys.argv) > 2 else f"test_{which}.0fs"
    data = build_with_subdir() if which == "subdir" else build()
    with open(out, "wb") as f:
        f.write(data)
    print(f"wrote {out}: {len(data)} bytes")
