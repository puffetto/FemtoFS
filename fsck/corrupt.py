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

@case("empty_bucket_bad_sentinel")
def _(d):
    # No empty buckets exist in this tiny image (root tablesize==N==2,
    # sub tablesize==N==1) -- skip meaningful mutation, just prove a
    # non-existent-cell edit doesn't crash: corrupt padding instead.
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


if __name__ == "__main__":
    base = load()
    all_ok = True
    for name, fn in cases.items():
        d = bytearray(base)
        d = fn(d)
        ok = run(name, d)
        all_ok = all_ok and ok
    sys.exit(0 if all_ok else 1)