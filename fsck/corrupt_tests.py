#!/usr/bin/env python3
"""Corrupts test_subdir.0fs in specific, targeted ways and checks that
fsck_femtofs (a) flags an error and (b) exits nonzero, i.e. doesn't
silently accept broken images. Each case is a single-byte or small edit
chosen to hit one specific normative rule.
"""
import struct
import subprocess
import sys

FSCK = "./fsck_femtofs"
SRC = "test_subdir.0fs"

def load():
    with open(SRC, "rb") as f:
        return bytearray(f.read())

def run(name, data):
    path = f"/tmp/corrupt_{name}.0fs"
    with open(path, "wb") as f:
        f.write(data)
    r = subprocess.run([FSCK, path], capture_output=True, text=True)
    ok = r.returncode != 0
    status = "PASS (correctly rejected)" if ok else "FAIL (accepted bad image!)"
    print(f"[{status}] {name}: exit={r.returncode}")
    if not ok:
        print("  --- fsck output ---")
        print(r.stdout, r.stderr)
    return ok

cases = {}

def case(name):
    def deco(f):
        cases[name] = f
        return f
    return deco

@case("bad_magic")
def _(d):
    d[0] = ord('X')
    return d

@case("bad_root_pad")
def _(d):
    d[67] = 1
    return d

@case("cell_count_zero")
def _(d):
    struct.pack_into("<I", d, 36, 0)
    return d

@case("meta_hash_corrupt")
def _(d):
    struct.pack_into("<I", d, 12, 0xdeadbeef)
    return d

@case("image_hash_corrupt")
def _(d):
    struct.pack_into("<I", d, 8, 0xdeadbeef)
    return d

@case("root_p_bad_p1_index")
def _(d):
    d[66] = 0x3F  # p1_index=63 >= SMALL_PRIMES_COUNT(53), mode SINGLE
    return d

@case("unknown_cell_type")
def _(d):
    d[128] = 0x42  # cell 0's type byte -> unknown, non-aux, not in {0..5,0x81}
    return d

@case("hardlink_target_wrong_type")
def _(d):
    # Repurpose cell 2 (currently a root bucket entry) is risky; instead
    # flip the DIR cell (index 6, offset 128+6*16=224) to look like it's
    # a HARDLINK targeting cell 0 (an ATTR cell) by directly editing a
    # *copy* of a hardlink-shaped cell. Simpler: corrupt cell 6's type
    # from DIR to HARDLINK while leaving w0 pointing at cell 0 (ATTR) --
    # this exercises "hardlink target must be FEMTOFS_TYPE_FILE".
    off = 128 + 6 * 16
    d[off] = 4  # FEMTOFS_TYPE_HARDLINK
    struct.pack_into("<H", d, off + 2, 0)       # attr_index = 0 (required)
    struct.pack_into("<III", d, off + 4, 0, 0, 0)  # target=cell 0 (ATTR), size=0, realsize=0
    return d

@case("file_realsize_nonzero")
def _(d):
    # cell 4 is hello.txt's FILE object at offset 128+4*16=192; word2 (realsize) at +12
    off = 128 + 4 * 16
    struct.pack_into("<I", d, off + 12, 1)
    return d

@case("filename_has_slash")
def _(d):
    # "hello.txt" bytes live in the public part; find and corrupt one byte to '/'
    idx = d.find(b"hello.txt")
    assert idx != -1
    d[idx + 2] = ord('/')  # "he/lo.txt"
    return d

@case("duplicate_name_in_root")
def _(d):
    # Point root bucket slot 1's name_off (cell 3, "sub") at the same
    # offset as slot 0's name ("hello.txt"), creating a duplicate-name
    # situation is awkward without recomputing hashes; instead just
    # duplicate the literal bytes: overwrite the "sub" blob region with
    # "hello.txt\0" truncated -- simpler: point cell 3's name_off directly
    # at hello.txt's name offset.
    hello_off = d.find(b"hello.txt")
    off = 128 + 3 * 16  # cell 3 = root bucket slot 1 ("sub" -> DIR)
    struct.pack_into("<I", d, off + 8, hello_off)  # w1 = name_off
    return d

@case("nonzero_hole_byte")
def _(d):
    # header-to-content gap is [128+meta_size, public_off); corrupt one
    # byte there if it exists.
    meta_size = struct.unpack_from("<I", d, 48)[0]
    public_off = struct.unpack_from("<I", d, 40)[0]
    gap_start = 128 + meta_size
    if gap_start < public_off:
        d[gap_start] = 0xFF
    else:
        print("  (no header-to-content gap in this image; skipping mutation)")
    return d

def _recompute_hashes(d):
    FNV1_32_INIT=0x811c9dc5; FNV_32_PRIME=0x01000193
    def fnv1_32(data, hval=FNV1_32_INIT):
        for b in data:
            hval=(hval*FNV_32_PRIME)&0xFFFFFFFF; hval^=b
        return hval
    meta_size = struct.unpack_from("<I", d, 48)[0]
    meta_hash = fnv1_32(bytes(d[128:128+meta_size]))
    struct.pack_into("<I", d, 12, meta_hash)
    image_hash = fnv1_32(bytes(d[16:]))
    struct.pack_into("<I", d, 8, image_hash)
    return d

@case("author_field_wrong")
def _(d):
    d[72] = ord('X')  # corrupt first byte of the required author string
    return _recompute_hashes(d)

@case("bucket_type_mismatch")
def _(d):
    # cell 4 = hello.txt FILE object. Corrupt whichever root bucket cell
    # (2 or 3) targets it so its own type byte no longer matches cell 4's
    # actual type (FILE), without touching the target itself.
    for bucket_off in (128 + 2*16, 128 + 3*16):
        target = struct.unpack_from("<I", d, bucket_off+4)[0]
        if target == 4:
            d[bucket_off] = 5  # FEMTOFS_TYPE_FIFO, doesn't match FILE
            break
    return _recompute_hashes(d)

@case("double_bucket_target")
def _(d):
    # Force BOTH root bucket cells (2 and 3) to target the same object
    # (cell 4, hello.txt), violating "exactly one occupied bucket".
    struct.pack_into("<I", d, 128 + 3*16 + 4, 4)
    d[128 + 3*16] = 1  # FEMTOFS_TYPE_FILE, so type still matches target
    return _recompute_hashes(d)

@case("hash_anchor_wrong")
def _(d):
    # Swap the two root bucket cells' *slot contents* (but keep them at
    # the same physical cells 2/3) so each entry no longer sits on its
    # own correct hash-computed anchor/chain -- the structural rules
    # (sentinels, N, chain shape) all still hold, only hash-reachability
    # breaks.
    off2, off3 = 128 + 2*16, 128 + 3*16
    cell2 = bytes(d[off2:off2+16])
    cell3 = bytes(d[off3:off3+16])
    d[off2:off2+16] = cell3
    d[off3:off3+16] = cell2
    return _recompute_hashes(d)


if __name__ == "__main__":
    base = load()
    all_ok = True
    for name, fn in cases.items():
        d = bytearray(base)
        d = fn(d)
        ok = run(name, d)
        all_ok = all_ok and ok
    sys.exit(0 if all_ok else 1)
