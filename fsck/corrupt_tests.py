#!/usr/bin/env python3
"""Mutates the test images in specific, targeted ways and checks fsck_femtofs's
verdict. A rejection case passes only if the checker exits 1 and reports the
expected diagnostic, and hashes are recomputed after each mutation, so a case
cannot pass merely because the image hash no longer matches. An acceptance case
(expect=None) passes only if the checker exits 0.
"""
import os
import struct
import subprocess
import sys

import make_test_image as img

FSCK = "./fsck_femtofs"
SUBDIR = "test_subdir.0fs"
LEGACY = "test_legacy.0fs"
RICH = "test_rich.0fs"

FNV1_32_INIT = 0x811c9dc5
FNV_32_PRIME = 0x01000193

def load(path):
    with open(path, "rb") as f:
        return bytearray(f.read())

def fnv1_32(data, hval=FNV1_32_INIT):
    for b in data:
        hval = (hval * FNV_32_PRIME) & 0xFFFFFFFF
        hval ^= b
    return hval

def recompute_hashes(d):
    meta_size = struct.unpack_from("<I", d, 48)[0]
    struct.pack_into("<I", d, 12, fnv1_32(bytes(d[128:128 + meta_size])))
    struct.pack_into("<I", d, 8, fnv1_32(bytes(d[16:])))
    return d

def run(name, data, expect):
    path = f"corrupt_{name}.0fs"
    with open(path, "wb") as f:
        f.write(data)
    r = subprocess.run([FSCK, path], capture_output=True, text=True)
    os.unlink(path)
    if expect is None:
        ok = r.returncode == 0
        verdict = "correctly accepted" if ok else "rejected valid image!"
    else:
        ok = r.returncode == 1 and expect in r.stderr
        verdict = "correctly rejected" if ok else f"expected exit 1 with {expect!r}"
    print(f"[{'PASS' if ok else 'FAIL'} ({verdict})] {name}: exit={r.returncode}")
    if not ok:
        print("  --- fsck output ---")
        print(r.stdout, r.stderr)
    return ok

cases = {}

def case(name, expect, src=SUBDIR, rehash=True):
    def deco(f):
        cases[name] = (f, expect, src, rehash)
        return f
    return deco

def cell(i):
    return 128 + i * 16

def set_version(d, version):
    struct.pack_into("<H", d, 6, version)

# ---- header identity and bounds -------------------------------------------

@case("bad_magic", "magic is not")
def _(d):
    d[0] = ord('X')

@case("bad_format", "format field is not")
def _(d):
    d[5] = 5

@case("major_version_2", "unsupported major version")
def _(d):
    set_version(d, 0x0201)

@case("bad_root_pad", "root_pad must be 0")
def _(d):
    d[67] = 1

@case("cell_count_zero", "cell_count must be > 0")
def _(d):
    struct.pack_into("<I", d, 36, 0)

@case("meta_hash_corrupt", "meta_hash mismatch", rehash=False)
def _(d):
    struct.pack_into("<I", d, 12, 0xdeadbeef)

@case("image_hash_corrupt", "image_hash mismatch", rehash=False)
def _(d):
    struct.pack_into("<I", d, 8, 0xdeadbeef)

@case("root_p_bad_p1_index", "root_p (0x3f)")
def _(d):
    d[66] = 0x3F  # p1_index=63 >= SMALL_PRIMES_COUNT(53), mode SINGLE

# ---- revision 0x0101 header layout ----------------------------------------

@case("author_field_wrong", "author field")
def _(d):
    d[78] = ord('X')

@case("author_not_terminated", "author field")
def _(d):
    d[127] = ord('X')

@case("header_padding_nonzero", "header padding")
def _(d):
    d[76] = 1

@case("creation_time_epoch", None)
def _(d):
    struct.pack_into("<I", d, 72, 0)  # zero is valid and denotes the epoch itself

@case("creation_time_max", None)
def _(d):
    struct.pack_into("<I", d, 72, 0xFFFFFFFF)  # every u32 value is representable

@case("legacy_layout_stamped_0x0101", "author field", src=LEGACY)
def _(d):
    set_version(d, 0x0101)

# ---- revision 0x0100 header layout ----------------------------------------

@case("legacy_author_wrong", "author field", src=LEGACY)
def _(d):
    d[72] = ord('X')

@case("legacy_author_padding", "legacy author padding", src=LEGACY)
def _(d):
    d[122] = 1

@case("new_layout_stamped_0x0100", "author field")
def _(d):
    set_version(d, 0x0100)

# ---- cells and object graph -----------------------------------------------

@case("unknown_cell_type", "unknown non-aux type 0x42")
def _(d):
    d[128] = 0x42  # cell 0's type byte -> unknown, non-aux, not in {0..5,0x81}

@case("hardlink_target_wrong_type", "is not a discovered FILE")
def _(d):
    # Flip the DIR cell (index 6) to a HARDLINK targeting cell 0 (an ATTR cell).
    off = cell(6)
    d[off] = img.TYPE_HARDLINK
    struct.pack_into("<H", d, off + 2, 0)
    struct.pack_into("<III", d, off + 4, 0, 0, 0)

@case("file_realsize_nonzero", "realsize must be 0")
def _(d):
    # cell 4 is hello.txt's FILE object; word 2 (realsize) is at +12
    struct.pack_into("<I", d, cell(4) + 12, 1)

@case("filename_has_slash", "contains '/'")
def _(d):
    # "hello.txt" bytes live in the public part; corrupt one byte to '/'
    idx = d.find(b"hello.txt")
    assert idx != -1
    d[idx + 2] = ord('/')

@case("duplicate_name_in_root", "duplicate directory-entry name")
def _(d):
    # Point cell 3's name_off at the other root bucket's filename.
    other = struct.unpack_from("<I", d, cell(2) + 8)[0]
    struct.pack_into("<I", d, cell(3) + 8, other)

@case("nonzero_hole_byte", "alignment gap")
def _(d):
    # header-to-content gap is [128+meta_size, public_off)
    meta_size = struct.unpack_from("<I", d, 48)[0]
    public_off = struct.unpack_from("<I", d, 40)[0]
    assert 128 + meta_size < public_off
    d[128 + meta_size] = 0xFF

@case("nonzero_content_hole", "unaccounted-for non-zero byte")
def _(d):
    private_off = struct.unpack_from("<I", d, 44)[0]
    d[private_off - 1] = 0xFF

@case("bucket_type_mismatch", "does not exactly equal")
def _(d):
    # Corrupt whichever root bucket targets cell 4 (FILE) to claim FIFO.
    for bucket_off in (cell(2), cell(3)):
        if struct.unpack_from("<I", d, bucket_off + 4)[0] == 4:
            d[bucket_off] = img.TYPE_FIFO
            break

@case("double_bucket_target", "already targeted")
def _(d):
    # Force both root buckets to target cell 4, violating "exactly one".
    struct.pack_into("<I", d, cell(3) + 4, 4)
    d[cell(3)] = img.TYPE_FILE

@case("hash_anchor_wrong", "not reachable from")
def _(d):
    # Swap the two root buckets so an entry leaves its own hash chain; the
    # shape rules (sentinels, N) still hold, only hash reachability breaks.
    off2, off3 = cell(2), cell(3)
    cell2, cell3 = bytes(d[off2:off2 + 16]), bytes(d[off3:off3 + 16])
    d[off2:off2 + 16] = cell3
    d[off3:off3 + 16] = cell2

# ---- the rich image: rules no smaller fixture can reach --------------------

def rich_bucket(placed_name):
    placed, _free = img.rich_root_layout()
    for rel, (name, _target, _next) in placed.items():
        if name == placed_name:
            return img.RICH_ROOT_FIRST + rel
    raise KeyError(placed_name)

def rich_empty_bucket():
    return img.RICH_ROOT_FIRST + img.rich_root_layout()[1][1]

@case("symlink_payload_in_public", "payload must be private", src=RICH)
def _(d):
    public_name = struct.unpack_from("<I", d, cell(rich_bucket(b"pub.txt")) + 8)[0]
    struct.pack_into("<I", d, cell(img.RICH_OBJ_SYM) + 4, public_name)

@case("public_name_in_private_part", "stored in the private part", src=RICH)
def _(d):
    # priv/ becomes world-listable, so "secret" must have moved to the public part.
    struct.pack_into("<H", d, cell(img.RICH_ATTR_PRIV_DIR) + 2, img.S_IFDIR | 0o755)

@case("private_payload_in_public_part", "none of its references is public", src=RICH)
def _(d):
    # pub.txt loses world-read, but its payload stays in the public part.
    struct.pack_into("<H", d, cell(img.RICH_ATTR_PUB) + 2, img.S_IFREG | 0o640)

@case("hash2_base_not_prime", "hash2_base", src=RICH)
def _(d):
    struct.pack_into("<I", d, 68, 65535)  # in range, but 3 * 5 * 17 * 257

@case("hash2_base_out_of_range", "hash2_base", src=RICH)
def _(d):
    struct.pack_into("<I", d, 68, 251)  # prime, but not > 2^8

@case("hardlink_attr_index_nonzero", "(HARDLINK): attr_index", src=RICH)
def _(d):
    struct.pack_into("<H", d, cell(img.RICH_OBJ_LINK) + 2, 1)

@case("hardlink_to_symlink", "is not a discovered FILE", src=RICH)
def _(d):
    struct.pack_into("<I", d, cell(img.RICH_OBJ_LINK) + 4, img.RICH_OBJ_SYM)

@case("fifo_data_off_nonzero", "(FIFO): data_off", src=RICH)
def _(d):
    struct.pack_into("<I", d, cell(img.RICH_OBJ_FIFO) + 4, 4)

@case("attr_ifmt_disagrees", "file-type bits disagree", src=RICH)
def _(d):
    struct.pack_into("<H", d, cell(img.RICH_ATTR_LNK) + 2, img.S_IFREG | 0o777)

@case("empty_bucket_data_off_nonzero", "empty bucket data_off", src=RICH)
def _(d):
    struct.pack_into("<I", d, cell(rich_empty_bucket()) + 4, 1)

@case("empty_bucket_bad_sentinel", "empty bucket next_index", src=RICH)
def _(d):
    struct.pack_into("<H", d, cell(rich_empty_bucket()) + 14, 0)

@case("unused_cell_nonzero", "unused cell", src=RICH)
def _(d):
    d[cell(img.RICH_UNUSED) + 15] = 1

@case("attr_unreferenced", "not referenced by root_attr", src=RICH)
def _(d):
    d[cell(img.RICH_UNUSED)] = img.TYPE_ATTR
    struct.pack_into("<H", d, cell(img.RICH_UNUSED) + 2, img.S_IFREG | 0o400)

@case("symlink_alias_of_shared_domain", "domains or announced lengths differ", src=RICH)
def _(d):
    # A symlink target must not alias a filename interval even with equal bytes.
    public_name = struct.unpack_from("<I", d, cell(rich_bucket(b"pub.txt")) + 8)[0]
    struct.pack_into("<I", d, cell(img.RICH_OBJ_SYM) + 4, public_name)

# ---- later revisions of the understood major ------------------------------

def future_reserved_bytes(d):
    """Every byte the format reserves as zero and a later revision may assign."""
    set_version(d, 0x0102)
    d[67] = 1                                                   # root_pad
    d[76] = d[77] = 1                                           # header padding
    d[78:128] = b"\xAA" * 50                                    # author
    d[cell(img.RICH_ATTR_ROOT) + 1] = 1                         # ATTR.reserved0
    struct.pack_into("<I", d, cell(img.RICH_ATTR_PUB) + 12, 4)  # ATTR.ext_off
    struct.pack_into("<I", d, cell(img.RICH_OBJ_PUB) + 12, 1)   # FILE.reserved_flags
    struct.pack_into("<I", d, cell(img.RICH_OBJ_SYM) + 12, 1)   # SYMLINK.reserved_flags
    struct.pack_into("<III", d, cell(img.RICH_OBJ_FIFO) + 4, 1, 2, 3)  # FIFO words
    struct.pack_into("<H", d, cell(img.RICH_OBJ_LINK) + 2, 7)   # HARDLINK.attr_index
    struct.pack_into("<II", d, cell(img.RICH_OBJ_LINK) + 8, 1, 2)      # HARDLINK reserved
    struct.pack_into("<I", d, cell(img.RICH_OBJ_EMPTY) + 4, 9)  # bucket_first of empty DIR
    realsize = struct.unpack_from("<I", d, cell(img.RICH_OBJ_PRIV) + 12)[0]
    struct.pack_into("<I", d, cell(img.RICH_OBJ_PRIV) + 12, realsize | 1 << 31)  # DIR bit 31
    d[cell(img.RICH_UNUSED) + 15] = 1                           # unused NULL cell
    bucket = cell(rich_bucket(b"pub.txt"))
    d[bucket + 1] = 1                                           # occupied bucket hash_p
    struct.pack_into("<H", d, bucket + 2, 1)                    # occupied bucket attr_index
    d[bucket + 12] = 1                                          # occupied bucket reserved
    empty = cell(rich_empty_bucket())
    d[empty + 1] = 1                                            # empty bucket hash_p
    struct.pack_into("<H", d, empty + 2, 1)                     # empty bucket attr_index
    struct.pack_into("<II", d, empty + 4, 1, 2)                 # empty bucket data_off, size
    d[empty + 12] = d[empty + 13] = 1                           # empty reserved, name_len
    meta_size = struct.unpack_from("<I", d, 48)[0]
    private_off = struct.unpack_from("<I", d, 44)[0]
    d[128 + meta_size] = 1                                      # metadata-to-content gap
    d[private_off - 1] = 1                                      # public-part hole
    d[-1] = 1                                                   # private-part hole

@case("future_revision_clean", None, src=RICH)
def _(d):
    set_version(d, 0x0102)

@case("future_revision_reserved_ignored", None, src=RICH)
def _(d):
    future_reserved_bytes(d)

@case("future_revision_hash2_base_unused", None)
def _(d):
    set_version(d, 0x01FF)
    struct.pack_into("<I", d, 68, 12345)  # unused without a DUAL directory

@case("future_revision_hash_still_checked", "image_hash mismatch", src=RICH, rehash=False)
def _(d):
    set_version(d, 0x0102)
    d[-1] = 1

@case("future_revision_structure_still_checked", "does not exactly equal", src=RICH)
def _(d):
    set_version(d, 0x0102)
    d[cell(rich_bucket(b"pipe"))] = img.TYPE_FILE

@case("future_revision_name_len_still_checked", "name_len", src=RICH)
def _(d):
    set_version(d, 0x0102)
    d[cell(rich_bucket(b"pipe")) + 13] = 3

@case("future_revision_unknown_type_rejected", "unknown non-aux type 0x42", src=RICH)
def _(d):
    set_version(d, 0x0102)
    d[cell(img.RICH_UNUSED)] = 0x42

@case("future_revision_dedup_still_checked", "must use one interval", src=RICH)
def _(d):
    # Duplicate "pipe" into a public hole and repoint its bucket there.
    set_version(d, 0x0102)
    private_off = struct.unpack_from("<I", d, 44)[0]
    dup = private_off - 8
    d[dup:dup + 5] = b"pipe\0"
    struct.pack_into("<I", d, cell(rich_bucket(b"pipe")) + 8, dup)
    # Keep another reference on the original bytes: the file payload now matches.
    pipe_off = d.find(b"pipe\0")
    obj = cell(img.RICH_OBJ_PUB)
    struct.pack_into("<II", d, obj + 4, pipe_off, 4)


if __name__ == "__main__":
    all_ok = True
    for name, (fn, expect, src, rehash) in cases.items():
        d = load(src)
        fn(d)
        if rehash:
            recompute_hashes(d)
        all_ok = run(name, d, expect) and all_ok
    sys.exit(0 if all_ok else 1)
