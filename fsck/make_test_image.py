#!/usr/bin/env python3
"""
Builds tiny, spec-conformant femtoFS images for exercising fsck_femtofs.

  flat:   /hello.txt
  subdir: /hello.txt, /sub/inner.txt  (exercises DIR-typed bucket entries)

Both builders place directory entries at their real computed hash anchors
(via place_directory_entries, an independent Python reimplementation of
femtofs_hash) rather than at arbitrary sequential slots, since
fsck_femtofs now verifies hash-anchor reachability per the spec's
Directory-slice and hash-chain rules.
"""
import struct

PAGE = 4096
FNV1_32_INIT = 0x811c9dc5
FNV_32_PRIME = 0x01000193

AUTHOR_FIELD = 'Andrea "Nemesi" Cocito - blackye at gmail dot com'.encode("ascii").ljust(56, b"\x00")
assert len(AUTHOR_FIELD) == 56

TYPE_FILE = 1
TYPE_DIR = 2
TYPE_ATTR = 0x81
S_IFDIR = 0o040000
S_IFREG = 0o100000
FEMTOFS_PARENT_ROOT = 0xFFFF


def fnv1_32(data, hval=FNV1_32_INIT):
    for b in data:
        hval = (hval * FNV_32_PRIME) & 0xFFFFFFFF
        hval ^= b
    return hval


def align_up(n, a):
    return (n + a - 1) // a * a


def obj_cell(typ, b1, f2, w0, w1, w2):
    return struct.pack("<BBHIII", typ, b1, f2, w0, w1, w2)


def pack_dir_realsize(parent, n):
    return (parent & 0xFFFF) | ((n & 0x7FFF) << 16)


def pack_bucket_word2(name, next_index):
    """Bucket bytes 12..15: reserved (u8 = 0), name_len (u8), next_index (u16)."""
    return (len(name) << 8) | (next_index << 16)


def blob(b):
    b = b + b"\x00"
    b += b"\x00" * ((-len(b)) % 4)
    return b


SMALL_PRIMES = [
    3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47,
    53, 59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107,
    109, 113, 127, 131, 137, 139, 149, 151, 157, 163, 167,
    173, 179, 181, 191, 193, 197, 199, 211, 223, 227, 229,
    233, 239, 241, 251,
]


def femtofs_hash(name: bytes, p: int, tablesize: int) -> int:
    """Independent Python reimplementation of the spec's reference hash,
    used to place directory entries at their *correct* anchor (and, on
    collision, correctly chain them) -- exactly what a real builder must
    do, and what fsck_femtofs's own anchor-reachability check verifies."""
    if tablesize in (0, 1):
        return 0
    h = 0
    for b in name:
        h = (h * p + b) & 0xFFFFFFFF
    return h % tablesize


def place_directory_entries(entries, tablesize, p1):
    """entries: list of (name: bytes, target_cell_idx: int).
    Returns {rel_index: [name, target, next_index]} using single-hash
    coalesced chaining exactly as the reference lookup algorithm expects:
    an entry's anchor slot holds it directly if free; otherwise it is
    appended to the anchor's chain via the first free slot found, and the
    previous chain tail's next_index is updated to link to it."""
    slots = {}
    for name, target in entries:
        anchor = femtofs_hash(name, p1, tablesize)
        if anchor not in slots:
            slots[anchor] = [name, target, tablesize]
            continue
        rel = anchor
        while slots[rel][2] != tablesize:
            rel = slots[rel][2]
        free = next(r for r in range(tablesize) if r not in slots)
        slots[free] = [name, target, tablesize]
        slots[rel][2] = free
    return slots


def build_header(*, image_hash, meta_hash, uuid, image_size, cell_count,
                  public_off, private_off, meta_size, root_first, root_size,
                  root_real, root_attr_idx, root_p, hash2_base):
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


def build():
    """/hello.txt -- one public file, trivial tablesize=1 root."""
    root_attr_idx, file_attr_idx = 0, 1
    cells = [None, None, None, None]
    cells[0] = obj_cell(TYPE_ATTR, 0, S_IFDIR | 0o755, 0, 0, 0)
    cells[1] = obj_cell(TYPE_ATTR, 0, S_IFREG | 0o644, 0, 0, 0)

    cell_count = len(cells)
    meta_size = cell_count * 16
    public_off = align_up(128 + meta_size, PAGE)

    filename, payload = b"hello.txt", b"hi femtoFS\n"
    public_part = bytearray()
    name_off = public_off + len(public_part)
    public_part += blob(filename)
    data_off = public_off + len(public_part)
    public_part += blob(payload)
    public_part += b"\x00" * ((-len(public_part)) % PAGE)
    if not public_part:
        public_part = bytearray(PAGE)

    private_part = bytearray()
    private_off = public_off + len(public_part)
    image_size = private_off + len(private_part)

    tablesize, root_real = 1, 1
    p1 = SMALL_PRIMES[0]
    placed = place_directory_entries([(filename, 3)], tablesize, p1)
    bucket_first = 2
    cells[2] = obj_cell(TYPE_FILE, 0, 0, placed[0][1], name_off,
                        pack_bucket_word2(filename, placed[0][2]))
    cells[3] = obj_cell(TYPE_FILE, 0, file_attr_idx, data_off, len(payload), 0)

    meta_bytes = b"".join(cells)
    assert len(meta_bytes) == meta_size

    uuid = tuple(0x1000 + i for i in range(4))
    meta_hash = fnv1_32(meta_bytes)
    kw = dict(uuid=uuid, image_size=image_size, cell_count=cell_count,
              public_off=public_off, private_off=private_off, meta_size=meta_size,
              root_first=bucket_first, root_size=tablesize, root_real=root_real,
              root_attr_idx=root_attr_idx, root_p=0, hash2_base=0)
    header0 = build_header(image_hash=0, meta_hash=meta_hash, **kw)
    assert len(header0) == 128

    image = bytearray()
    image += header0
    image += meta_bytes
    image += b"\x00" * (public_off - len(image))
    image += public_part
    image += private_part
    assert len(image) == image_size

    image_hash = fnv1_32(bytes(image[16:]))
    image[0:128] = build_header(image_hash=image_hash, meta_hash=meta_hash, **kw)
    return bytes(image)


def build_with_subdir():
    """
    root/
      hello.txt
      sub/            (subdirectory -- exercises DIR-typed bucket entries)
        inner.txt
    """
    root_attr_idx, file_attr_idx = 0, 1

    # ---- cell plan --------------------------------------------------
    # 0: ATTR (dir mode)   1: ATTR (file mode)
    # 2,3: root's 2-slot bucket table (placed by hash, not sequentially)
    # 4: FILE object (hello.txt)   5: sub's 1-slot bucket table
    # 6: DIR object (sub)          7: FILE object (inner.txt)
    cells = [None] * 8
    cells[0] = obj_cell(TYPE_ATTR, 0, S_IFDIR | 0o755, 0, 0, 0)
    cells[1] = obj_cell(TYPE_ATTR, 0, S_IFREG | 0o644, 0, 0, 0)
    cells[6] = obj_cell(TYPE_DIR, 0, root_attr_idx, 5, 1,
                         pack_dir_realsize(FEMTOFS_PARENT_ROOT, 1))

    cell_count = len(cells)
    meta_size = cell_count * 16
    public_off = align_up(128 + meta_size, PAGE)

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
    if not public_part:
        public_part = bytearray(PAGE)

    private_part = bytearray()
    private_off = public_off + len(public_part)
    image_size = private_off + len(private_part)

    p1 = SMALL_PRIMES[0]
    root_tablesize = 2
    root_placed = place_directory_entries(
        [(b"hello.txt", 4), (b"sub", 6)], root_tablesize, p1)
    root_bucket_first = 2
    for rel in range(root_tablesize):
        name, target, next_index = root_placed[rel]
        cells[root_bucket_first + rel] = obj_cell(
            TYPE_FILE if target == 4 else TYPE_DIR, 0, 0,
            target, offs[("name", name)], pack_bucket_word2(name, next_index))

    sub_tablesize = 1
    sub_placed = place_directory_entries([(b"inner.txt", 7)], sub_tablesize, p1)
    cells[5] = obj_cell(TYPE_FILE, 0, 0, sub_placed[0][1],
                         offs[("name", b"inner.txt")],
                         pack_bucket_word2(b"inner.txt", sub_placed[0][2]))

    cells[4] = obj_cell(TYPE_FILE, 0, file_attr_idx,
                         offs[("data", b"hello.txt")], len(payloads[b"hello.txt"]), 0)
    cells[7] = obj_cell(TYPE_FILE, 0, file_attr_idx,
                         offs[("data", b"inner.txt")], len(payloads[b"inner.txt"]), 0)

    meta_bytes = b"".join(cells)
    assert len(meta_bytes) == meta_size

    uuid = tuple(0x2000 + i for i in range(4))
    meta_hash = fnv1_32(meta_bytes)
    kw = dict(uuid=uuid, image_size=image_size, cell_count=cell_count,
              public_off=public_off, private_off=private_off, meta_size=meta_size,
              root_first=root_bucket_first, root_size=root_tablesize, root_real=2,
              root_attr_idx=root_attr_idx, root_p=0, hash2_base=0)
    header0 = build_header(image_hash=0, meta_hash=meta_hash, **kw)
    image = bytearray()
    image += header0
    image += meta_bytes
    image += b"\x00" * (public_off - len(image))
    image += public_part
    image += private_part
    assert len(image) == image_size
    image_hash = fnv1_32(bytes(image[16:]))
    image[0:128] = build_header(image_hash=image_hash, meta_hash=meta_hash, **kw)
    return bytes(image)


if __name__ == "__main__":
    import sys
    which = sys.argv[1] if len(sys.argv) > 1 else "flat"
    out = sys.argv[2] if len(sys.argv) > 2 else f"test_{which}.0fs"
    data = build_with_subdir() if which == "subdir" else build()
    with open(out, "wb") as f:
        f.write(data)
    print(f"wrote {out}: {len(data)} bytes")
