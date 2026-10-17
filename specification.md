# femtoFS: zero-fsck-giving, zero-frills, read-only, near-zero-copy filesystem image format

This document defines a compact, memory-friendly filesystem image format.

## Scope

The aim is to provide a filesystem image format optimal for booting a "firmware-like"
FreeBSD system in a highly resource-constrained environment.

femtoFS is designed for:
- FreeBSD 14+, amd64 and arm64 kernels.
- Read-only mounts.
- RAM-backed images (`md(4)` workflow).
- Fast O(1) lookup by hashed directory entries.
- Near-zero-copy `mmap(2)`.

Design priorities:
- Runtime performance for lookup, read, and `mmap(2)` behavior take precedence over image build speed.
- Builder work must remain reasonable, but offline search and packing costs are acceptable.
- Primary optimization targets are runtime performance and final image size.

Out of scope:
- In-place writes.
- Journaling.
- ACLs and rich metadata (but see `femtofs_metaext` below).

Hard limits:
- Total image size `< 2^32` bytes.
- Metadata table cells `< 2^16`.
- Non-root visible filesystem objects `< 2^15`; including the root, at most
  `2^15` objects are visible. Each non-root object consumes both an object cell
  and an occupied bucket cell, so the `< 2^16` shared-table limit imposes this
  bound even before attribute cells and empty hash buckets are counted.
- Entries in a single directory `< 2^15`.

Near-zero-copy `mmap(2)` is exposed through three mount modes:
- `dirty`: superset of `leaking` for small public files. For files in the
  public part referenced by a public-eligible canonical regular-file inode with
  `size < PAGE_SIZE`, the implementation accepts offsets not aligned to
  `VM_PAGE_SIZE` and uses shifted mappings to preserve zero-copy behavior.
  The returned logical address has the same within-page displacement as the
  first mapped source byte, so it may not be VM-page-aligned. No leading or
  trailing bytes in the mapped public pages are cleaned or copied.
  For non-public-eligible regular-file inodes and for all files with
  `size >= PAGE_SIZE`, `dirty` keeps the `leaking` alignment contract.
- `leaking`: VM-page-aligned mappings only. Preserves zero-copy behavior for
  suitably aligned public-part files whose size is at least `VM_PAGE_SIZE` and
  may expose neighboring bytes outside the requested range, again only from
  the public part.
- `clean` (default): POSIX-style behavior. Returned mappings are VM-page-aligned
  and bytes outside file bounds are zero-filled. A file whose size is at most
  `VM_PAGE_SIZE` requires at most one copied kernel VM page. A larger file also
  requires at most one copied tail page when its image data is VM-page-aligned;
  otherwise additional copied pages may be required.

Security note:
- In this version, public/private classification is defined only by classic
  Unix ownership+permission visibility rules on exported paths.
- If a future version makes ACL/MAC policy semantics active for access control,
  implementations should force `clean` mode unless the public/private
  classifier is extended to include those policies.
- Otherwise, `leaking`/`dirty` could expose neighboring bytes that are
  world-visible by mode bits but denied by ACL/MAC policy.

---

## On-disk ABI Rules

In format version `0x0100`, all multi-byte integers are little-endian and all
integers are fixed-width:
- `uint8_t`
- `uint16_t`
- `uint32_t`

Never use native-width types (`int`, `long`, `size_t`, `off_t`) in on-disk
records.

C implementations must enforce layout with compile-time checks:

```c
_Static_assert(sizeof(struct femtofs_header) == 128, "femtofs_header size");
_Static_assert(offsetof(struct femtofs_header, uuid) == 16, "femtofs uuid offset");
_Static_assert(offsetof(struct femtofs_header, image_size) == 32, "femtofs image_size offset");
_Static_assert(sizeof(union femtofs_object_role) == 12, "femtofs_object_role size");
_Static_assert(sizeof(struct femtofs_object) == 16, "femtofs_object size");
_Static_assert(sizeof(struct femtofs_attr) == 16, "femtofs_attr size");
_Static_assert(sizeof(struct femtofs_metaext) == 16, "femtofs_metaext size");
```

---

## Format Overview

The image is a single contiguous blob with three regions, in order:

1. Header (fixed, 128 bytes).
2. Metadata table (fixed cells, 16 bytes each).
3. Content region, split into:
   - public part: `[public_off, private_off)`
   - private part: `[private_off, image_size)`

All 32-bit byte offsets defined by version `0x0100` are relative to the start of
the image, including `public_off`, `private_off`, object `data_off` when used as
a byte offset, and bucket filename offsets. `ext_off` is a reserved field that
must be zero in this version; a future revision must define the offset rules
for it and any extension-record offsets it introduces.

Metadata-table locations and quantities are expressed in cells, not bytes.
`cell_count`, `root_size`, `root_real`, directory `size`, and directory packed
`N` are counts. `root_first`, `root_attr`, object `attr_index`, and directory,
hardlink, and bucket `data_off` are absolute metadata-table indices. Bucket
`realsize` is an index relative to the start of its containing directory slice.

The metadata table serves a triple role:
- object metadata (`file`, `dir`, `symlink`, `fifo`, `hardlink`)
- directory hash buckets
- deduplicated attribute records

There is no separate hash-table region.
There is no separate attribute-table region; attribute records are cells in the
same 16-byte metadata table.

---

## Header (`femtofs_header`)

```c
struct femtofs_header {
    uint8_t  magic[4];       // "0FS\0"
    uint8_t  format[2];      // { byte order, page shift minus 8 }, currently {0, 4}
    uint16_t version;        // format version, currently 0x0100
    uint32_t image_hash;     // FNV-1 32-bit hash from uuid through final image byte
    uint32_t meta_hash;      // FNV-1 32-bit hash over metadata table

    uint32_t uuid[4];        // random image UUID (128-bit)
    uint32_t image_size;     // exact femtoFS byte length, less than 2^32

    uint32_t cell_count;     // number of entries in metadata table
    uint32_t public_off;     // content-region start (public part, page-aligned)
    uint32_t private_off;    // split point: private-part start (page-aligned)
    uint32_t meta_size;      // metadata table size in bytes

    uint32_t root_first;     // root bucket slice start index in metadata table
    uint32_t root_size;      // root tablesize
    uint32_t root_real;      // root N (actual entry count)
    uint16_t root_attr;      // attribute cell index for root directory
    uint8_t  root_p;         // root hash control byte (mode + small-prime index)
    uint8_t  root_pad;       // must be 0
    uint32_t hash2_base;     // global second-hash base prime for dual-hash mode

    uint8_t  author[56];     // 'Andrea "Nemesi" Cocito - blackye at gmail dot com\0', zero-padded
};                           // exactly 128 bytes
```

Notes:
- Header starts at byte 0.
- Metadata table starts at byte 128.
- Content region starts at `public_off`.
- Public part is `[public_off, private_off)`.
- Private part is `[private_off, image_size)`.
- `private_off >= public_off`.
- `public_off >= 128 + meta_size`.
- `private_off` is the only boundary needed to classify any content pointer:
  `off < private_off` is public, `off >= private_off` is private.
- `meta_hash` is a non-cryptographic metadata-table integrity/sanity check.
- `image_hash` is a non-cryptographic image-data integrity/sanity check.
- `uuid` is a random per-image 128-bit identifier.
- Root descriptor is in header because root has no parent bucket entry.
- `root_p` uses the same hash-control encoding as directory `hash_p` fields.
- `hash2_base` is used only when at least one directory (including root) is in
  dual-hash mode; otherwise it must be `0`.
- `author` is the fixed 56-byte format-author identifier shown in the structure
  declaration: its 49 ASCII bytes occupy `author[0..48]`, `author[49]` is the
  terminating NUL, and `author[50..55]` are zero.
- Attribute cells may appear anywhere in the metadata table.

The two single-byte `format` fields can be read before interpreting any
multi-byte field:

- `format[0]` identifies the byte order:
  - `0` means little-endian and is the only value defined by version `0x0100`.
  - `1` is reserved specifically for a future big-endian format. It is not
    defined by version `0x0100` and must be rejected by version `0x0100`
    readers.
  - Values `2` through `255` are available for future format definitions.
- `format[1]` is the base-2 page-size exponent minus 8, so a defined value `n`
  denotes a page size of `2^(n + 8)` bytes. Version `0x0100` defines only `4`,
  meaning 4096 bytes; all other values are available for future format
  definitions and must be rejected by version `0x0100` readers. Implementations
  must validate this byte before using it as a shift count.

Therefore, the only valid format field in version `0x0100` is `{0, 4}`. Images
are portable between supported machines that implement this format; a VM page-
size mismatch is handled as specified below, and architecture names do not
otherwise affect the on-disk format. A kernel or other consumer must reject an
unsupported format field.

Future format definitions may assign additional byte-order and page-size
values. Power-of-two page sizes from 256 bytes upward preserve the alignment of
the 128-byte header and 16-byte metadata cells, although all existing offset,
image-size, and checked-arithmetic constraints still apply. Reserving a value
does not make it valid for version `0x0100`.

While this header retains 32-bit offsets and `image_size < 2^32`, the largest
structurally usable encoded page is `2^31` bytes (`format[1] == 23`). A value of
`24` would require the first positive page-aligned content offset to be at
least `2^32`, which this layout cannot represent.

Throughout this specification, `PAGE_SIZE` means the decoded image page size,
which is exactly 4096 bytes in version `0x0100`. `VM_PAGE_SIZE` means the
mounting kernel's runtime `PAGE_SIZE`; a "kernel VM page" is exactly
`VM_PAGE_SIZE` bytes.

### Image and VM Page-Size Compatibility

A kernel MUST NOT reject an otherwise supported image solely because
`PAGE_SIZE` and `VM_PAGE_SIZE` differ:

- If `VM_PAGE_SIZE <= PAGE_SIZE`, the mismatch MUST NOT by itself force a mount-
  mode downgrade. Both sizes are powers of two, so every image-page boundary,
  including `public_off` and `private_off`, is also a VM-page boundary.
  Individual sub-page blobs are not necessarily VM-page-aligned, so their
  mappings may still require the shifted or copied-page paths defined below.
- If `VM_PAGE_SIZE > PAGE_SIZE`, the kernel MUST mount using effective `clean`
  behavior, regardless of the requested mode. One VM page can span multiple
  image pages and may otherwise cross the public/private boundary or expose an
  unrelated packed blob.

These rules affect mounting behavior only after the consumer has accepted the
image's format and page-size code. They do not require a consumer to accept a
page-size code that its format version does not define.

### Short Semantic Version

The 16-bit `version` field is a short semantic version written as `0xMMrr`:

- `MM`, the high byte, is the major version.
- `rr`, the low byte, is the revision within that major version.
- The version defined by this specification is therefore major `1`, revision
  `0` (`0x0100`).

A producer that introduces an extension or specification change that can make
an image incompatible with consumers of the existing major version MUST assign
a different `MM`. A producer SHOULD assign a new `rr` when it introduces an
extension or specification change that remains backward compatible with all
earlier revisions of the same `MM`.

A consumer MUST accept only major versions that it explicitly understands. It
MAY implement previous major versions by selecting the corresponding parser and
semantics; compatibility between different major versions must never be
assumed. For an understood `MM`, a consumer SHOULD use only the features and
semantics through `min(image_rr, highest_rr_understood_for_MM)`. Consequently,
a revision greater than the highest one understood by the consumer is not, by
itself, a reason to reject an image of an understood major version.

Optional metadata extensions are explicitly eligible for a revision update
within major version `1`: adding an extension payload without changing the
meaning of the existing `mode`, `uid`, and `gid` fields is backward compatible
and does not require a new major version. A major-1 consumer that does not
understand an extension MUST ignore it and continue to enforce the classic
Unix attributes. Therefore, an extension introduced within major version `1`
cannot impose access restrictions that depend on every major-1 consumer
understanding it.

A canonical-byte validator that accepts a higher revision while implementing
only an earlier revision cannot identify storage owned by unknown extensions.
It MUST either use revision-aware extension validation or omit only those
canonical-byte checks that require a complete map of extension-owned bytes.
Unknown extensions do not relax structural validation of the version fields
and records the consumer does understand.

### Metadata Integrity Hash (`meta_hash`)

`meta_hash` is deterministic and algorithm-fixed:
- Hash function: FreeBSD kernel `fnv_32_buf()` from `<sys/fnv_hash.h>`
  (FNV-1, 32-bit state).
- Initial value: `FNV1_32_INIT`.
- Input bytes: exact metadata-table byte range
  `image[128 .. 128 + meta_size)`.
- Header bytes (including `meta_hash`) are not part of the hash input.
- No salt/nonce/secret key is used.

Builder rule:
- After finalizing all metadata-table bytes, compute:
  `meta_hash = fnv_32_buf(meta_bytes, meta_size, FNV1_32_INIT)`.

Reader rule:
- Before trusting metadata-table contents, implementations SHOULD recompute the
  same hash and require exact equality with header `meta_hash`; mismatch means
  corrupt/invalid image.

### Image Integrity Hash (`image_hash`)

`image_hash` is algorithm-fixed:
- Hash function: FreeBSD kernel `fnv_32_buf()` from `<sys/fnv_hash.h>`
  (FNV-1, 32-bit state).
- Initial value: `FNV1_32_INIT`.
- Input bytes: exact byte range from `uuid` (inclusive) to the
  end of image (`image_size`), i.e. in this layout:
  `image[offsetof(struct femtofs_header, uuid) .. image_size)`.
- The input includes `uuid[]`, `image_size`, the remainder of the header,
  metadata table, alignment padding, and both content parts.
- The 16-byte prefix containing `magic`, `format`, `version`, `image_hash`, and
  `meta_hash` is not part of the hash input.
- No secret key is used. The UUID is ordinary hashed image data.
- `meta_hash` and `image_hash` do not include each other's stored value and may
  be computed independently after their respective input bytes are finalized.

Builder rule:
- After finalizing all bytes from `uuid` through the end of image,
  compute:
  `image_hash = fnv_32_buf(image + offsetof(struct femtofs_header, uuid), image_size - offsetof(struct femtofs_header, uuid), FNV1_32_INIT)`.

Reader rule:
- Implementations MAY verify `image_hash` at mount/open time.

### Image Fingerprint and Same-Image Identity

The byte range `image[0 .. 32)` is the image fingerprint. It contains, in
order: `magic`, `format`, `version`, `image_hash`, `meta_hash`, and all four
words of `uuid[0..3]`. `image_size` begins at byte 32 and is covered indirectly
by `image_hash`.

When determining whether two mounted/opened images are "the same image",
implementations SHOULD require exact equality of this 32-byte fingerprint.
The fingerprint is intended for fast identity comparison, not cryptographic
authentication or image validation; FNV-1 is non-cryptographic. Readers must
still perform the required structural and integrity checks before trusting an
image.

Offset model:
- Every on-disk 32-bit pointer to content bytes is image-relative.
- `private_off` is the sole public/private classifier for those pointers.
- Pointer validity for content bytes is:
  `public_off <= off < image_size`.
- A future revision that defines extension pointers must also define their
  bounds and private-part requirements; an image whose revision is exactly
  `0x0100` requires `ext_off == 0`.

---

## Metadata Table Cells

```c
union femtofs_object_role {
    struct {
        uint32_t data_off;
        uint32_t size;
        uint32_t realsize;
    } raw;

    struct {
        uint32_t bucket_first;
        uint32_t bucket_count;
        uint32_t parent_n;
    } dir;

    struct {
        uint32_t image_off;
        uint32_t content_size;
        uint32_t reserved_flags;
    } filelike;

    struct {
        uint32_t target_index;
        uint32_t reserved0;
        uint32_t reserved1;
    } hardlink;

    struct {
        uint32_t object_index;
        uint32_t name_off;
        uint32_t next_index;
    } bucket;
};

struct femtofs_object {
    uint8_t  type;        // FEMTOFS_TYPE_* (kept first as on-disk discriminator)
    uint8_t  hash_p;      // directories: hash control byte; buckets: reserved (0)
    uint16_t attr_index;  // attribute cell index; ignored for buckets
    union femtofs_object_role role; // role-specific payload (12 bytes)
};                        // exactly 16 bytes

struct femtofs_attr {
    uint8_t  type;        // FEMTOFS_TYPE_ATTR
    uint8_t  reserved0;   // must be 0
    uint16_t mode;        // st_mode bits (type + perms + suid/sgid/sticky)
    uint32_t uid;         // numeric owner
    uint32_t gid;         // numeric group
    uint32_t ext_off;     // must be 0 in v0x0100; reserved extension pointer
};                        // exactly 16 bytes
```

```c
#define FEMTOFS_TYPE_NULL      0
#define FEMTOFS_TYPE_FILE      1
#define FEMTOFS_TYPE_DIR       2
#define FEMTOFS_TYPE_SYMLINK   3
#define FEMTOFS_TYPE_HARDLINK  4
#define FEMTOFS_TYPE_FIFO      5
#define FEMTOFS_TYPE_AUX       0x80
#define FEMTOFS_TYPE_ATTR      (FEMTOFS_TYPE_AUX | 1)
```

Object cells and attribute cells share the same 16-byte table. Their role is
identified by the leading `type` byte. Bucket cells are represented using the
`femtofs_object` layout with bucket semantics described below.

For compact notation in the rest of this document, `data_off`, `size`, and
`realsize` refer to `role.raw.data_off`, `role.raw.size`, and
`role.raw.realsize`, respectively.

`type` intentionally remains at byte 0. Reordering all members by width would
move the discriminator, change the on-disk ABI, and invalidate the existing
bucket/type interpretation rules.

Auxiliary cell rule:
- Any cell whose `type` has `FEMTOFS_TYPE_AUX` set is not a directory bucket
  occupant and must be treated as empty by hash lookup and directory iteration.
- `FEMTOFS_TYPE_ATTR` is the only defined auxiliary cell type in this version.

Cell role is contextual:
- Directory bucket slices must be pairwise disjoint.
- Inside a directory slice, a non-null, non-auxiliary cell is an occupied
  bucket regardless of its `type`; a `NULL` cell is an empty bucket and an
  auxiliary cell is empty for directory purposes.
- Outside every directory slice, a non-null, non-auxiliary cell is a filesystem
  object. A filesystem object must never occupy or be targeted inside a bucket
  slice.
- Attribute cells may be inside or outside directory slices. `NULL` cells
  outside directory slices are unused metadata cells.

### Field-Width Utilization Map (`femtofs_object`, v1)

This subsection makes explicit where on-disk fields carry values with a smaller
effective domain than their storage width. It is intended to show where future
extensions could add meaning.

Builder hygiene:
- Builders should write `0` in all currently unused bytes/bits.
- Readers should only require zero where this specification already marks fields
  as reserved and checks them in validation rules.

`femtofs_object` header fields:
- `type` (`uint8_t`): this version defines only
  `NULL`, `FILE`, `DIR`, `SYMLINK`, `HARDLINK`, `FIFO`, `ATTR`
  (plus `AUX` namespace bit). Other values are unassigned.
- `hash_p` (`uint8_t`):
  - `DIR`: hash-control byte:
    - bits `[5:0]` encode `p1_index` into `SMALL_PRIMES`
    - bits `[7:6]` encode hash mode (`00` single-hash, `01` dual-hash,
      `10/11` reserved)
  - Bucket cells: reserved and required `0`.
  - `FILE`/`SYMLINK`/`FIFO`/`HARDLINK`: currently unused by semantics.
- `root_p` in the header uses the same encoding as directory `hash_p`.
- `attr_index` (`uint16_t`):
  - `FILE`/`DIR`/`SYMLINK`/`FIFO`: attribute-cell index `< cell_count` where
    `cell_count < 2^16`.
  - `HARDLINK` and bucket cells: required `0`.

`union femtofs_object_role` payload fields (stored as three `uint32_t` words):
- Word 0 (`data_off` / `bucket_first` / `image_off` / `target_index` / `object_index`):
  - `FILE`/`SYMLINK` `image_off`: image-relative byte offset (`< 2^32`), full
    32-bit offset domain.
  - `DIR` `bucket_first`: metadata index (`< 2^16`), logically `u16` in `u32`.
  - `HARDLINK` `target_index`: metadata index (`< 2^16`), logically `u16` in
    `u32`.
  - Bucket `object_index`: metadata index (`< 2^16`), logically `u16` in `u32`.
  - `FIFO`: required `0`.
- Word 1 (`size` / `bucket_count` / `content_size` / `reserved0` / `name_off`):
  - `FILE`/`SYMLINK` `content_size`: announced size (`< 2^32`), full 32-bit
    length domain.
  - `DIR` `bucket_count`: `tablesize`, bounded by `<= 65521`, logically `u16`
    in `u32`.
  - Bucket `name_off`: image-relative byte offset (`< 2^32`), full 32-bit
    offset domain.
  - `HARDLINK` `reserved0`: required `0`.
  - `FIFO`: required `0`.
- Word 2 (`realsize` / `parent_n` / `reserved_flags` / `reserved1` / `next_index`):
  - `DIR` `parent_n`:
    - bits `[15:0]` parent index
    - bits `[30:16]` `N`
    - bit `[31]` reserved (`0`)
  - `FILE`/`SYMLINK` `reserved_flags`: required `0` in this version (entire
    word currently unused).
  - `HARDLINK` `reserved1`: required `0`.
  - Bucket `next_index`: chain next index or end sentinel `tablesize`,
    bounded by `<= 65521`, logically `u16` in `u32`.
  - `FIFO`: required `0`.

### Directory Hash-Control Encoding (`hash_p`, `root_p`)

Directory hash policy is encoded in one byte:
- low 6 bits: `p1_index` (index into `SMALL_PRIMES`)
- high 2 bits: hash mode

```c
#define FEMTOFS_HASH_P1IDX_MASK   0x3Fu
#define FEMTOFS_HASH_MODE_MASK    0xC0u
#define FEMTOFS_HASH_MODE_SHIFT   6u

#define FEMTOFS_HASH_MODE_SINGLE  0u
#define FEMTOFS_HASH_MODE_DUAL    1u
/* 2 and 3 are reserved */
```

Interpretation:
- `SINGLE` mode: one anchor hash (`h1`) using `p1 = SMALL_PRIMES[p1_index]`.
- `DUAL` mode: two anchor hashes (`h1`, `h2`):
  - `h1` uses `p1 = SMALL_PRIMES[p1_index]`
  - `h2` uses `hash2_base` from the image header.
- `SMALL_PRIMES` table content and order are defined in
  "Choosing Hash Parameters at Build Time".

`hash2_base` rules:
- If any directory (including root) is in `DUAL` mode, `hash2_base` must be a
  prime satisfying `2^8 < hash2_base < 2^24`.
- If no directory uses `DUAL` mode, `hash2_base` must be `0`.

Implementation note (non-normative):
`hash2_base` is global, so a consumer needs at most one primality test during
image validation, after determining whether any directory uses `DUAL` mode.
Lookup never needs to repeat the test.

A simple format-specific validator can enforce the
`2^8 < hash2_base < 2^24` bounds before testing primality:

```c
static bool
femtofs_valid_hash2_base(uint32_t n)
{
    uint32_t d;

    if (n <= (1u << 8) || n >= (1u << 24))
        return false;
    if (n % 2 == 0)
        return false;
    if (n % 3 == 0)
        return false;

    for (d = 5; d <= n / d; d += 6) {
        if (n % d == 0 || n % (d + 2) == 0)
            return false;
    }
    return true;
}
```

The trial-division loop is safe across the complete `uint32_t` domain, while
the wrapper intentionally rejects values outside the format's `hash2_base`
domain. Accepted candidates require divisors only below `2^12`, making the one
mount-time check inexpensive without a sieve, table, Miller-Rabin, or wider
arithmetic.

Builder policy flexibility:
- Builder implementations MAY choose per-directory mode (`SINGLE` vs `DUAL`)
  and MAY classify "hard" directories using their own heuristic.
- Builder implementations MAY test multiple candidate `hash2_base` primes in
  the allowed range and select the best candidate.
- Builders SHOULD choose parameters to balance lookup performance (worst-case
  chain length and average comparisons) against metadata size (bucket count).
- This specification does not mandate one optimizer algorithm.

### Directory Metadata Packing (`realsize`)

For `FEMTOFS_TYPE_DIR`, `realsize` packs both parent pointer and `N`:

- bits `[15:0]`: `parent_index` (`uint16_t`)
- bits `[30:16]`: `N` (actual entry count), so `N < 2^15`
- bit `[31]`: reserved, must be `0`

`parent_index = 0xFFFF` is reserved as `FEMTOFS_PARENT_ROOT` (the parent is the
root descriptor in the header). This avoids cross-filesystem `..` links in on-disk
metadata.

```c
#define FEMTOFS_PARENT_ROOT      0xFFFFu
#define FEMTOFS_DIR_PARENT_MASK  0x0000FFFFu
#define FEMTOFS_DIR_N_MASK       0x7FFF0000u
#define FEMTOFS_DIR_N_SHIFT      16

#define FEMTOFS_DIR_GET_N(x) \
    ((uint16_t)(((x) & FEMTOFS_DIR_N_MASK) >> FEMTOFS_DIR_N_SHIFT))
#define FEMTOFS_DIR_GET_PARENT(x) \
    ((uint16_t)((x) & FEMTOFS_DIR_PARENT_MASK))
#define FEMTOFS_DIR_PACK(parent, n) \
    (((uint32_t)(parent) & FEMTOFS_DIR_PARENT_MASK) | \
     ((((uint32_t)(n)) & 0x7FFFu) << FEMTOFS_DIR_N_SHIFT))

#define FEMTOFS_PART_PRIVATE       0u
#define FEMTOFS_PART_PUBLIC        1u
#define FEMTOFS_OFF_IS_PUBLIC(off, private_off) \
    ((uint8_t)(((uint32_t)(off) < (uint32_t)(private_off)) ? \
               FEMTOFS_PART_PUBLIC : FEMTOFS_PART_PRIVATE))
#define FEMTOFS_OFF_IS_PRIVATE(off, private_off) \
    ((uint8_t)(((uint32_t)(off) >= (uint32_t)(private_off)) ? 1u : 0u))
```

### Entry Roles

Metadata rules:
- Every non-root filesystem object except `HARDLINK` references a deduplicated
  attribute record via `attr_index`; a hardlink resolves attributes through its
  canonical `FILE` target.
- Every non-root filesystem object is referenced by exactly one occupied
  directory bucket. Multiple paths to one regular inode are represented by one
  canonical `FILE` object plus distinct `HARDLINK` objects, not by making
  multiple buckets target the same object cell.
- `femtofs_attr.mode` stores the exported `st_mode` bits, including permission
  bits, setuid, setgid, sticky, and file type bits.
- Object type and the `S_IFMT` bits in the resolved mode must agree: `FILE` and
  `HARDLINK` resolve to `S_IFREG`, `DIR` to `S_IFDIR`, `SYMLINK` to `S_IFLNK`,
  and `FIFO` to `S_IFIFO`. The root attribute must resolve to `S_IFDIR`.
- `femtofs_attr.uid` and `femtofs_attr.gid` store numeric FreeBSD owner/group IDs
  directly.
- `femtofs_attr.ext_off` must be `0` in version `0x0100`; see the reserved
  `femtofs_metaext` hook below.

`FEMTOFS_TYPE_DIR`:
- `hash_p`: hash-control byte (`p1_index` + mode, see encoding section)
- `attr_index`: attribute cell index for this directory
- `data_off`: first bucket index in metadata table
- `size`: `tablesize` (bucket count)
- `realsize`: packed `{ parent_index, N }`

`FEMTOFS_TYPE_FILE` and `FEMTOFS_TYPE_SYMLINK`:
- `attr_index`: attribute cell index for this object
- `data_off`: image-relative byte offset into content region
- `size`: announced byte length (original object size; excludes stored
  terminator and alignment padding)
- `realsize`: reserved for future filelike flags, must be `0` in this version
- part classification is derived from header boundary:
  - `data_off < private_off` => public part
  - `data_off >= private_off` => private part

`FEMTOFS_TYPE_FIFO`:
- `attr_index`: attribute cell index for this object
- `data_off`: `0` (no payload)
- `size`: `0`
- `realsize`: `0`

`FEMTOFS_TYPE_HARDLINK`:
- `attr_index`: `0` (ignored; attributes come from target object)
- `data_off`: target object index (not byte offset)
- `size`: `0`
- `realsize`: `0`
- `data_off` storage is `uint32_t`, but the valid domain is `< 2^16`
  (`cell_count < 2^16`), so this is logically a `u16` index carried in
  a `u32` field (upper bits must be zero).

Hardlink constraints:
- Target index must be `< cell_count`.
- Target must be a non-hardlink regular file object.
- No hardlinks to directories, symlinks, or FIFOs.

`FEMTOFS_TYPE_ATTR`:
- auxiliary metadata cells that may appear anywhere in the metadata table
- never referenced by directory buckets directly
- never resolved as filesystem objects through path lookup

Attribute-cell placement reference policy (builder):

The following construction and physical-placement algorithm is informative,
not mandatory. Builders MAY use a different construction order or placement,
provided their output satisfies this specification's validation rules. Exact
attribute-tuple deduplication remains mandatory.

1. Build object cells and directory bucket slices first.
2. Deduplicate attribute tuples by exact `(mode, uid, gid, ext_off)` equality.
   Order references with the root first, followed by filesystem object index;
   for placement purposes the root descriptor is in the header page.
3. Place each attribute cell:
   - first choice: first unused cell on the same page as the first referencing object
   - second choice: first unused cell on the same page as the next referencing object
   - third choice: if no more referencing objects remain, first unused cell anywhere in the existing table
   - last choice: extend the metadata table
4. Record only the chosen `attr_index`; readers must not assume any contiguous
   attribute range.

An unused `NULL` bucket cell inside a directory slice is an eligible placement
target for an attribute cell. Once populated with `FEMTOFS_TYPE_ATTR`, its AUX
bit makes it behave as an empty bucket during lookup and iteration. Directory
chains must never link to such a cell, and it does not contribute to directory
`N`.

### Bucket Entries (Directory Hash Slice)

Bucket entries are 16-byte cells using the `femtofs_object` layout in a
directory's bucket slice.

Occupied bucket:
- `type`: referenced object type (non-null)
- `hash_p`: reserved in bucket role, must be `0`
- `attr_index`: `0` (unused in bucket role)
- `data_off`: referenced object index
- `size`: image-relative filename offset (NUL-terminated string)
- `realsize`: next slice-relative bucket index in the chain, or `tablesize` as
  the end sentinel

The bucket `type` must exactly equal the referenced object's `type`. The target
must be a filesystem object cell outside every directory slice; it cannot be a
bucket, attribute, or unused cell.

Despite the compact `role.raw.size` alias, bucket `size` is a byte offset, not
a byte length. Bucket-handling code must interpret it only as `name_off`.
Likewise, bucket `realsize` is relative to the start of its directory slice;
it is not an absolute metadata-table index.

Empty bucket:
- `type = FEMTOFS_TYPE_NULL`
- `hash_p = 0`, `attr_index = 0`, `data_off = 0`, and `size = 0`
- `realsize = tablesize`

Bucket interpretation rules:
- In directory hash lookup, a cell is treated as empty if `type` is
  `FEMTOFS_TYPE_NULL` or if `type & FEMTOFS_TYPE_AUX` is nonzero.
- Builders must never emit chain links that target an auxiliary cell.

---

## Inode and Hardlink Semantics

Inode identity is based on canonical object index, never on `data_off`
payload offsets.

Canonical index:
- for non-hardlink object at index `i`: canonical index is `i`
- for hardlink object at index `i`: canonical index is `obj[i].data_off`

Recommended exported inode numbers:
- root: `st_ino = 1`
- non-root: `st_ino = canonical_index + 2`

Exported `st_nlink` is derived rather than stored:
- For a regular file, count all directory entries whose `FILE` or `HARDLINK`
  object resolves to the same canonical file index.
- For a directory, use `2 +` the number of immediate child directory entries,
  accounting for the synthesized `.` and child `..` links. This rule also
  applies to the root.
- Symlinks and FIFOs have `st_nlink == 1`.

Ownership and mode resolution:
- root attributes come from attribute cell `root_attr`.
- non-hardlink object attributes come from attribute cell `obj[i].attr_index`.
- hardlink object attributes resolve through the canonical target object.

Attribute constraints:
- `root_attr` must be `< cell_count` and refer to a `FEMTOFS_TYPE_ATTR` cell.
- For `FEMTOFS_TYPE_FILE`, `FEMTOFS_TYPE_DIR`, `FEMTOFS_TYPE_SYMLINK`, and
  `FEMTOFS_TYPE_FIFO`,
  `attr_index` must be `< cell_count` and refer to a `FEMTOFS_TYPE_ATTR` cell.
- Attribute records may be shared by multiple objects.

### Metadata Extension Hook (`femtofs_metaext`)

`femtofs_attr.ext_off` reserves an optional extension pointer for richer
metadata without affecting the common case.

```c
struct femtofs_metaext {
    uint16_t version;      // extension record version
    uint16_t kind_flags;   // FEMTOFS_METAEXT_* bits
    uint32_t acl_off;      // 0 or image-relative offset of ACL payload
    uint32_t xattr_off;    // 0 or image-relative offset of xattr payload
    uint32_t payload_size; // total bytes owned by this extension record
};                         // exactly 16 bytes
```

```c
#define FEMTOFS_METAEXT_ACL    0x0001u
#define FEMTOFS_METAEXT_XATTR  0x0002u
```

Rules:
- Version `0x0100` does not define an extension-record or ACL/xattr payload
  encoding. Its producers MUST write `ext_off == 0`, and consumers MUST reject
  an attribute with `ext_off != 0` when the image revision is exactly `0x0100`.
- When a major-1 consumer that understands only revision `0x0100` accepts a
  higher major-1 revision, it MUST ignore `ext_off` and extension payloads it
  does not understand. It continues to expose and enforce `mode`, `uid`, and
  `gid`; unknown ACLs, xattrs, and other extension metadata have no effect.
- A future revision that defines extension payloads MUST place every extension
  record and owned payload in the private part and specify their bounds and
  validation rules.
- The structure and flag values above reserve the shape of a possible future
  extension hook; they do not make any nonzero `ext_off` valid in version
  `0x0100`.

---

## Directory Lookup and `.` / `..`

### Hash Function

```c
uint32_t femtofs_hash(const char *name, uint32_t p, uint32_t tablesize) {
    if (tablesize == 0) return 0;
    if (tablesize == 1) return 0;
    uint32_t h = 0;
    while (*name)
        h = h * p + (unsigned char)*name++;
    return h % tablesize;
}
```

`p` is the full 32-bit base; implementations MUST NOT narrow `hash2_base`
before or during hashing. Each multiply-add is evaluated as `uint32_t`, with
unsigned wraparound modulo `2^32`, before the final reduction modulo
`tablesize`.

Implementation note (performance, non-normative):
- Because filename blobs are 4-byte aligned and zero-padded, implementations
  SHOULD use word-wise scanning in hot paths (hash and compare), then resolve
  the terminal word byte-by-byte to stop exactly at the first `0x00`.
- This is an optimization only; externally visible behavior must match the
  byte-wise reference function above.

### Lookup Algorithm

```c
if (strcmp(query, ".") == 0)
    return current_directory;

if (strcmp(query, "..") == 0) {
    if (is_root_directory)
        return root_directory;
    parent = FEMTOFS_DIR_GET_PARENT(dir->realsize);
    if (parent == FEMTOFS_PARENT_ROOT)
        return root_directory;
    return &cell_table[parent];
}

if (is_root_directory) {
    bucket_first = header->root_first;
    tablesize = header->root_size;
    hash_ctrl = header->root_p;
} else {
    bucket_first = dir->data_off;
    tablesize = dir->size;
    hash_ctrl = dir->hash_p;
}
buckets = &cell_table[bucket_first];

if (tablesize == 0)
    return ENOENT;

mode = (hash_ctrl & FEMTOFS_HASH_MODE_MASK) >> FEMTOFS_HASH_MODE_SHIFT;
p1_index = hash_ctrl & FEMTOFS_HASH_P1IDX_MASK;
p1 = SMALL_PRIMES[p1_index];

anchors[0] = femtofs_hash(query, p1, tablesize);
anchor_count = 1;
if (mode == FEMTOFS_HASH_MODE_DUAL) {
    h2 = femtofs_hash(query, header->hash2_base, tablesize);
    if (h2 != anchors[0])
        anchors[anchor_count++] = h2;
}

for (a = 0; a < anchor_count; ++a) {
    idx = anchors[a];
    if (buckets[idx].type == FEMTOFS_TYPE_NULL || (buckets[idx].type & FEMTOFS_TYPE_AUX))
        continue;

    for (;;) {
        name = image_base + buckets[idx].size;
        if (strcmp(name, query) == 0)
            return &cell_table[buckets[idx].data_off];
        if (buckets[idx].realsize == tablesize)
            break;
        idx = buckets[idx].realsize;
    }
}

return ENOENT;
```

This fast path assumes the image has already passed all normative validation,
including slice bounds, bucket typing, filename termination, chain safety, and
hash reachability.

### Directory Listing

`readdir` synthesizes:
- `.`
- `..`

Then it scans occupied buckets (`type != FEMTOFS_TYPE_NULL` and
`(type & FEMTOFS_TYPE_AUX) == 0`) in the directory slice. Listing order for
hash buckets is undefined.

---

## Choosing Hash Parameters at Build Time

Goal: minimize directory lookup cost while keeping metadata table growth small.

Reference objective for single-hash mode:
- minimize `sum_of_squares(chain_lengths) / N`
- subject to bounded `tablesize`.

Definitions:

```c
MAX_TABLESIZE_PRIME = 65521   // largest prime < 2^16
SMALL_PRIMES = [
    3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47,
    53, 59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107,
    109, 113, 127, 131, 137, 139, 149, 151, 157, 163, 167,
    173, 179, 181, 191, 193, 197, 199, 211, 223, 227, 229,
    233, 239, 241, 251
]                       // fixed on-disk index map for `p1_index`
SMALL_PRIMES_COUNT = 53
```

Implementations should derive the in-memory array length where possible and
must assert that it equals the normative `SMALL_PRIMES_COUNT`.

### Single-Hash Reference Policy

The following algorithm is informative (reference policy), not mandatory.
Builder implementations MAY use different tuning logic, provided encoded
outputs satisfy this specification's validation rules.

Reference algorithm:

```text
if N == 0:
    return (p1_index=0, tablesize=0)

if N == 1:
    return (p1_index=0, tablesize=1)

best_p1_index = 0
best_size   = N
best_score  = +inf
tablesize   = N                           // phase 1: fully packed
ceiling     = min(next_prime(2 * N), MAX_TABLESIZE_PRIME)

loop:
    for each (p1_index, p) in SMALL_PRIMES:
        simulate coalesced hash with (p, tablesize)
        score = sum_of_squares(chain_lengths) / N

        if score == 1.0:
            return (p1_index, tablesize)

        if score < best_score:
            best_score = score
            best_p1_index = p1_index
            best_size = tablesize

    if best_score < 1.1:
        return (best_p1_index, best_size)

    next = next_prime(tablesize)
    if next > ceiling:
        return (best_p1_index, best_size) // best-effort fallback at ceiling
    tablesize = next
```

Notes:
- `N < 2^15` and `MAX_TABLESIZE_PRIME = 65521` guarantee representable
  indices and sentinel behavior.
- `next_prime(x)` means the smallest prime strictly greater than `x`.
- For `N = 32767`, `next_prime(2*N) = 65537` is out of range; algorithm
  clamps to `65521` and returns best result seen.
- Phase 1 uses `tablesize = N` even if `N` is not prime.
- Phase 2 grows through primes only.

### Mixed Single/Dual Policy (Recommended, Not Mandated)

Builder implementations MAY use a mixed policy:
- keep `SINGLE` mode for directories that already meet target lookup quality
- switch selected "hard" directories to `DUAL` mode.

For `DUAL` mode, builder implementations MAY:
- pick `hash2_base` as any prime in `2^8 < p < 2^24`
- test multiple candidate primes (for example random samples) and keep the best.

Builders SHOULD tune these choices to balance:
- lookup quality (worst-case chain length and average comparisons)
- metadata growth (extra buckets / bytes).

This specification intentionally does not mandate one optimizer algorithm.

---

## Content Region (Public/Private Split)

The image has one content region split into two contiguous parts:
- Public part: `[public_off, private_off)`, bytes that are safe to expose in
  `dirty`/`leaking` neighbor spill, including eligible regular-file data and
  visible filename blobs.
- Private part: `[private_off, image_size)`, all other bytes.

Content-pointer visibility is determined only by comparison with `private_off`.

### Exported Path Model

- An exported path is an absolute path from the image root descriptor to one
  directory-entry occurrence, following bucket references.
- If multiple hardlink paths reach the same canonical file object, each path is
  an occurrence for visibility classification.

### Visibility Classification

- A canonical regular-file inode is public-eligible if and only if its resolved
  mode has world-read (`S_IROTH`) and every directory on at least one exported
  path to a `FILE` or referring `HARDLINK` occurrence has world-search
  (`S_IXOTH`). All such occurrences must be considered.
- A directory-entry name occurrence is public-visible only if at least one
  exported path to its containing directory is world-readable and world-
  searchable (`S_IROTH|S_IXOTH`) on each directory along that path.

The name rule is intentionally stricter than ordinary traversal of a known
pathname. For example, a world-searchable but non-world-readable `/hide`
directory may contain `/hide/$uuid/...`: callers that already know `$uuid` may
traverse it, but names below `/hide` remain private-part content and cannot be
exposed by `leaking` or `dirty` neighbor spill.

### Shared Blob Deduplication Domain

- Regular-file payload bytes and filename bytes are blobs in one shared dedup
  class domain.
- Blob-class key is exact byte identity of announced bytes.
- Part assignment is by class promotion: if any occurrence in a blob class is
  public (public-eligible payload reference or public-visible filename
  occurrence), that blob class is stored in the public part; otherwise it is
  stored in the private part.
- As a result, non-public file objects and private-tree entries may reference
  public-part blobs whenever the referenced bytes are shared with at least one
  public occurrence.

Symlink targets are always stored in the private part and are outside this
shared payload/filename blob domain. A future revision that defines metadata-
extension payloads must place them in the private part as well.

Interpretation is determined by references, not embedded tags.

### Blob Encoding And Word Alignment

All payload and filename blobs stored in content parts use one canonical
encoded form:
1. Write announced bytes.
2. Append one `0x00` terminator byte.
3. Append extra `0x00` bytes until next blob offset is aligned to a 4-byte word.

Definitions:
- `FEMTOFS_BLOB_WORD = 4`
- `stored_blob_bytes = align_up(announced_bytes + 1, FEMTOFS_BLOB_WORD)`

Consequences:
- Every stored blob is NUL-terminated in its content part.
- Every next blob begins at a 4-byte aligned offset.
- If `announced_bytes % 4 == 0`, the suffix is one full zero word.
- Otherwise the suffix is `1..3` zero bytes that simultaneously terminate and
  align.
- Therefore, for every stored blob, the 4th byte of its terminal 4-byte word
  is always `0x00`.
- Object-reported size remains the original announced size (`size` for file
  objects); suffix bytes are never part of object-visible length.

Every encoded-blob offset is 4-byte aligned. Its announced bytes are followed
by exactly the canonical zero suffix described above, and the complete stored
blob must remain within one content part. For packing checks, `content size`
always means `stored_blob_bytes`: a stored blob smaller than `PAGE_SIZE` must
not cross an image-page boundary, while one at least `PAGE_SIZE` must start on
an image-page boundary.

Implementation note (performance, non-normative):
- Kernel code MAY scan filename blobs as a sequence of 32-bit words and treat a
  word whose 4th byte is `0x00` as the terminal word.
- Hashing and string-compare semantics remain byte-accurate C-string semantics:
  stop at the first `0x00` byte, and do not include alignment-padding bytes in
  hash input.

### Packing Algorithm (Per Content Part)

Public and private parts are packed independently with the same algorithm.
All contents are sorted descending by size.
A sorted free-hole set (by hole size, then offset) tracks page-padding gaps.

For each content:

1. Try smallest fitting hole.
2. If no hole fits, append at tail.
3. If append would straddle page, advance tail to next page and record gap.

Packing unit:
- `content` size used by this algorithm is encoded blob size
  `align_up(announced_bytes + 1, 4)`, not announced size.

Consequences (per part):
- Contents `>= PAGE_SIZE` become page-aligned by construction.
- Holes are always `< PAGE_SIZE`.
- Sub-page contents stay page-contained.
- Every byte not occupied by an encoded blob, including free holes and part-end
  alignment gaps, must be written as `0x00`.

### Deduplication

Deduplication may cross object kinds by promotion:
- One shared blob class domain is used for regular-file payload bytes and
  filename bytes.
- For any shared blob class, store exactly one copy in exactly one content part:
  the public part if the class has at least one public occurrence, otherwise
  the private part.
- Cross-part duplication of a shared payload/filename blob class is not
  allowed.
- Stored blob bytes are derived deterministically from announced bytes by the
  canonical terminator/alignment rule above.

Content-interval rules:
- References in the shared regular-file/filename domain with identical
  announced bytes must use the same image offset and stored interval.
- References may share an interval only when their deduplication domain and
  announced bytes are identical. A symlink target never aliases a regular-file
  payload or filename interval, even when its bytes are equal.
- Distinct stored intervals must not overlap.
- Every byte in the content region not belonging to a unique stored interval
  is padding or a packing hole and must be `0x00`.

Public/private placement is also a mount-safety invariant, not merely a builder
optimization. For every shared-domain interval in the public part, at least
one reference must come from a public-eligible canonical regular-file inode or
a public-visible filename occurrence under the visibility rules above.
Symlink targets must always be in the private part.

---

## `mmap(2)` Contract by Mount Mode

Common constraints:
- Filesystem storage is read-only. Shared writable mappings and shared write
  protection upgrades are rejected.
- Private mappings may be writable, initially or through a later protection
  upgrade. Writes use process-private copy-on-write pages; the image and shared
  clean-page cache remain immutable. Unmodified eligible pages still map directly.
  File access, execute restrictions and security policy checks still apply.
- Only regular files are mappable.
- Alignment of mapping requests and VM mappings is measured against
  `VM_PAGE_SIZE`; packing and on-disk alignment are measured against
  `PAGE_SIZE`.
- If `VM_PAGE_SIZE > PAGE_SIZE`, the effective mode is always `clean` as
  specified in **Image and VM Page-Size Compatibility**.

For the direct shifted mapping defined by `dirty`, let:

```text
source_first = data_off + offset
source_shift = source_first % VM_PAGE_SIZE
source_base  = source_first - source_shift
mapping_span = align_up(source_shift + length, VM_PAGE_SIZE)
```

The implementation maps the image beginning at `source_base` to a
VM-page-aligned `mapping_base` and returns `mapping_base + source_shift` as the
logical address of the requested file bytes. The mapped VM range is exactly
`mapping_span` bytes. Consequently, the returned address is not VM-page-aligned
when `source_shift != 0`. All additions and alignment operations above are
checked operations and MUST reject overflow.

Public/private eligibility is derived from the `private_off` boundary:
- A file payload is public when `data_off < private_off`.
- A file payload is private when `data_off >= private_off`.
- Public payloads may use direct mapping with neighbor spill according to rules
  below.
- Private payloads must use clean behavior (no private-byte spill), even when
  mount mode is `dirty` or `leaking`.
- The `dirty` unaligned optimization applies only to public-eligible file
  inodes with `size < PAGE_SIZE`, even if additional non-public objects
  reference the same promoted public bytes.

`clean` mode (default):
- `offset` must be `VM_PAGE_SIZE`-aligned, otherwise `EINVAL`.
- File size `<= VM_PAGE_SIZE`: use at most one cached anonymous kernel VM page
  containing file bytes plus zero-fill. If the entire mapping is directly
  representable, no copied page is required.
- File size `> VM_PAGE_SIZE` with VM-page-aligned `data_off`: map full leading
  VM pages directly from the image. If the file has a final partial VM page,
  map that tail from one cached anonymous kernel VM page containing tail bytes
  plus zero-fill; otherwise the file is fully direct-mapped.
- File size `> VM_PAGE_SIZE` with a `data_off` not aligned to `VM_PAGE_SIZE`:
  use anonymous clean pages wherever the source cannot be represented by
  direct VM-page mappings. This may require copying more than one kernel VM
  page, including all pages of the file.

`leaking` mode:
- `offset` must be `VM_PAGE_SIZE`-aligned, otherwise `EINVAL`.
- Public-part files with `size >= VM_PAGE_SIZE` can skip tail-page copy and map
  directly even when the final page is partial, provided the source range is
  suitably VM-page-aligned.
- Any bytes outside file bounds that become visible must originate from the
  public part.

`dirty` mode:
- For private payload files and public payload files with `size >= PAGE_SIZE`,
  `offset` must be `VM_PAGE_SIZE`-aligned (same as `leaking`).
- For a public-eligible regular-file inode whose payload is in the public part
  and whose `size < PAGE_SIZE`, the implementation MUST accept an otherwise
  valid `offset` that is not aligned to `VM_PAGE_SIZE` and MUST use the direct
  shifted mapping defined above. It MUST NOT allocate an anonymous clean page
  or copy file bytes for such a mapping.
- When `source_shift != 0`, a successful call returns a non-VM-page-aligned
  logical address. Bytes before and after the requested file range that occupy
  the mapped VM pages remain accessible and are not zero-filled. The complete
  mapped source-page range MUST remain inside the public part.
- Without `MAP_FIXED`, a non-null `addr` is a hint for the returned logical
  address. The implementation MUST ignore a hint whose within-page displacement
  is not `source_shift`, choose a suitable page-aligned `mapping_base`, and
  return the shifted logical address.
- With `MAP_FIXED`, the requested address can be honored only when its
  within-page displacement is `source_shift` and it satisfies the platform's
  other fixed-mapping constraints. An incompatible request, including a
  page-aligned address when `source_shift != 0`, MUST fail with `EINVAL`.
- Callers using an interface that requires a page-aligned mapping address must
  operate on `mapping_base = align_down(returned_address, VM_PAGE_SIZE)` and
  include `source_shift` when computing the affected length.
- Any bytes outside file bounds that become visible must originate from the
  public part.

The shifted returned address and visible leading/trailing bytes are intentional
non-POSIX behavior selected by mounting in `dirty` mode.

Operational summary:
- Clean behavior copies at most one kernel VM page for a file no larger than
  `VM_PAGE_SIZE`. For a larger file, it copies at most the final partial kernel
  VM page when `data_off` is VM-page-aligned. Only a larger, unaligned file may
  require additional copied pages. Copied pages are populated lazily and are
  cacheable.
- Leaking mode can avoid that copy for suitably VM-page-aligned public-part
  files with `size >= VM_PAGE_SIZE`.
- When `dirty` remains the effective mode, every otherwise valid mapping of a
  nonempty public-eligible file uses direct image pages and no copied clean
  pages: files with `size >= PAGE_SIZE` use the `leaking` path, while smaller
  files use a shifted logical address.
- Private-part files never leak neighboring bytes in any mount mode.

Anonymous-page cache key for clean behavior:
- Key by stable source location of copied bytes, i.e. image-relative offset of
  the first copied byte (`src_first_byte_off`).
- Public/private classification is derivable from `private_off`, so separate
  part-selector bits are not required in the key.
- This naturally shares cached pages across hardlinks and deduplicated file
  contents whenever copied source region is identical.
- Implementation may also record `copied_len` as a consistency check.

---

## Alignment and Padding

Header and metadata table:
- `femtofs_header` is exactly 128 bytes.
- `femtofs_object` is exactly 16 bytes.
- `femtofs_attr` is exactly 16 bytes.
- No inter-record padding.

Content region split:
- `public_off`, `private_off`, and `image_size` must be image-page-aligned.
- `private_off` marks the start of the private part and therefore the end of
  the public part.
- Contents `>= PAGE_SIZE` are page-aligned by packing algorithm.
- Contents `< PAGE_SIZE` must remain page-contained.
- All bytes in the header/metadata-to-content alignment gap, content-packing
  holes, public/private alignment gap, and final image padding must be `0x00`.
  Builders must initialize these ranges explicitly. Full validation must reject
  nonzero padding; a kernel may omit the complete padding scan as described
  below.

---

## Normative Validation Rules (Builder/Reader/Kernel)

This section defines normative (`MUST`/`SHOULD`/`MAY`) checks for deterministic
interoperability and safe mounting.

Structural checks are mandatory for builders and all consumers. Canonical-byte
checks are mandatory for builders and offline/full validators but may require a
complete content scan; a kernel may omit only checks explicitly marked
canonical-byte below. If an implementation performs such a check and detects a
violation, the image is invalid.

Header and top-level bounds:
- All additions, multiplications, alignments, and range-end calculations must
  use checked arithmetic wide enough to detect overflow before narrowing to an
  on-disk field.
- Implementations MUST check `magic`, `format`, and `version`:
  - `magic` must be exactly `{'0','F','S','\0'}`.
  - `format` must be exactly `{0, 4}` for version `0x0100`.
  - The major byte `MM` of `version` must be explicitly supported by the
    reader; unsupported major versions must be rejected. The revision byte
    `rr` must be handled according to **Short Semantic Version** above.
- A kernel reader must apply **Image and VM Page-Size Compatibility**; page-size
  mismatch alone is not grounds for rejecting an otherwise supported image.
- `author` must equal the specified NUL-terminated format-author identifier and
  zero-filled remainder.
- `root_pad == 0`.
- `image_size > 0` and `image_size <= backing_provider_size`.
- `0 < cell_count < 2^16`.
- `meta_size == cell_count * 16`.
- `128 + meta_size <= public_off <= private_off <= image_size`.
- `public_off`, `private_off`, and `image_size` must be aligned to `PAGE_SIZE`.
- `root_attr < cell_count` and `cell[root_attr].type == FEMTOFS_TYPE_ATTR`.
- `root_size <= MAX_TABLESIZE_PRIME`.
- `root_real < 2^15` and `root_real <= root_size`.
- If `root_size == 0`, then `root_real == 0` and `root_first == 0`.
- If `root_size > 0`, then `root_first + root_size <= cell_count`.
- Root hash control (`root_p`) must decode to:
  - mode in `{FEMTOFS_HASH_MODE_SINGLE, FEMTOFS_HASH_MODE_DUAL}`
  - `p1_index < SMALL_PRIMES_COUNT`.
- Implementations SHOULD check `meta_hash` to ensure metadata-table integrity:
  `meta_hash == fnv_32_buf(image + 128, meta_size, FNV1_32_INIT)`.
- Implementations MAY check `image_hash` at mount/open time:
  `image_hash == fnv_32_buf(image + offsetof(struct femtofs_header, uuid), image_size - offsetof(struct femtofs_header, uuid), FNV1_32_INIT)`.
- To decide whether two images are "the same image", implementations SHOULD
  check exact equality of the first 32 header bytes, through `uuid[3]`.

Metadata role map and object graph:
- Validation must start with the root bucket slice and recursively discover
  filesystem objects through occupied buckets. Discovering a `DIR` object
  claims its `[data_off, data_off + size)` bucket slice.
- All claimed directory slices must be in range and pairwise disjoint. No
  filesystem object target may lie in any claimed slice, including a slice
  claimed later during traversal.
- Every occupied bucket must target an in-range filesystem object outside all
  directory slices, and its `type` must exactly equal the target object's
  `type`.
- Every non-root filesystem object must be targeted by exactly one occupied
  bucket and must be reachable from the root. After traversal, any non-null,
  non-auxiliary cell outside all directory slices that was not discovered as
  such an object is invalid. The total discovered non-root object count must be
  `< 2^15`.
- If a bucket in the root targets a `DIR`, that directory's packed parent must
  be `FEMTOFS_PARENT_ROOT`. If a bucket in non-root directory object `d`
  targets a `DIR`, the target's packed parent must be the metadata index of
  `d`. This also makes directory cycles and disconnected subtrees invalid.

Cell typing, attributes, and reserved fields:
- The only valid types are `NULL`, `FILE`, `DIR`, `SYMLINK`, `HARDLINK`,
  `FIFO`, and `ATTR`; unknown non-auxiliary and auxiliary types are invalid.
- For each discovered `FILE`, `DIR`, `SYMLINK`, or `FIFO` object,
  `attr_index < cell_count` and `cell[attr_index]` must be an `ATTR` cell.
- Every `ATTR` cell, referenced or not, must have `reserved0 == 0` and
  `ext_off == 0` when the image revision is exactly `0x0100`. A consumer that
  accepts a higher major-1 revision follows the forward-compatibility rules in
  **Short Semantic Version** for an extension it does not understand.
- Every `ATTR` cell must be referenced by the root or at least one discovered
  non-hardlink object, and each distinct `(mode, uid, gid, ext_off)` tuple must
  be represented by exactly one `ATTR` cell.
- A `NULL` cell outside every directory slice is unused and all of its
  remaining 15 bytes must be zero.
- Resolved attribute file-type bits must agree with object role as specified in
  **Entry Roles**. The attribute referenced by `root_attr` must have `S_IFDIR`
  file-type bits.
- Each discovered `DIR` object's `hash_p` must decode to a supported mode and
  `p1_index < SMALL_PRIMES_COUNT`; packed `realsize` bit `[31]` must be `0`.
- For every discovered `FILE` or `SYMLINK` object, `realsize == 0`.
- For every discovered `FIFO` object, `data_off == 0`, `size == 0`, and
  `realsize == 0`.
- For every discovered `HARDLINK` object, `attr_index == 0`, `size == 0`, and
  `realsize == 0`.
- Every occupied bucket must have `hash_p == 0` and `attr_index == 0`.
- If any directory, including root, uses `DUAL`, `hash2_base` must be prime and
  satisfy `2^8 < hash2_base < 2^24`; otherwise `hash2_base` must be `0`.

Directory-slice and hash-chain rules:
- For root, `(bucket_first, tablesize, N)` is
  `(root_first, root_size, root_real)`. For a non-root `DIR`, it is
  `(data_off, size, FEMTOFS_DIR_GET_N(realsize))`.
- Every descriptor must have `tablesize <= MAX_TABLESIZE_PRIME`, `N < 2^15`,
  and `N <= tablesize`. If `tablesize == 0`, then `N == 0` and
  `bucket_first == 0`; otherwise `bucket_first + tablesize <= cell_count`.
- For every cell in a directory slice:
  - `NULL` is an empty bucket and must have `hash_p == 0`, `attr_index == 0`,
    `data_off == 0`, `size == 0`, and `realsize == tablesize`;
  - an auxiliary cell is empty for directory purposes and must validate by its
    auxiliary type, without bucket-role field checks; or
  - a non-null, non-auxiliary cell is an occupied bucket whose `realsize` is
    either the `tablesize` end sentinel or an index `< tablesize`.
- `N` must equal the number of occupied buckets in the slice, and duplicate
  filenames within one directory are invalid.
- After validating each occupied bucket's filename and stored bounds below,
  compute its own anchor or anchors exactly as in **Lookup Algorithm**:
  - `h1 = femtofs_hash(name, SMALL_PRIMES[p1_index], tablesize)`;
  - in `DUAL` mode, also
    `h2 = femtofs_hash(name, hash2_base, tablesize)`;
  - equal `h1` and `h2` values represent one distinct anchor.
- The occupied bucket at slice-relative index `i` must be reachable by
  following `realsize` links from at least one of its own distinct anchors;
  reachability from an unrelated bucket or anchor is insufficient.
- Validation must process every distinct computed anchor exactly as lookup
  does. A `NULL` or auxiliary anchor terminates that anchor immediately.
  Otherwise it must traverse through the end sentinel even after finding the
  bucket being checked; every cell in that chain must be an occupied bucket.
  Each traversal must terminate in at most `tablesize` steps without cycles,
  out-of-slice jumps, or links into `NULL` or auxiliary cells.

Filename and path-component rules:
- For each occupied bucket, `name_off = size` must be 4-byte aligned and
  satisfy `public_off <= name_off < image_size`.
- The first NUL terminator must occur within the same content part as
  `name_off`. The preceding filename length must be `1..255` bytes and those
  bytes must not contain `'/'`.
- On-disk directory entries named `"."` or `".."` are forbidden.
- With `stored_name_bytes = align_up(name_length + 1, 4)`, the complete range
  `[name_off, name_off + stored_name_bytes)` must remain in that part, and all
  bytes from the terminator through the end of the stored range must be zero.
- If `stored_name_bytes < PAGE_SIZE`, the stored range must not cross an image-
  page boundary; otherwise `name_off` must be `PAGE_SIZE`-aligned.

File/symlink payload bounds:
- For each discovered `FILE` or `SYMLINK`, `data_off` must be 4-byte aligned
  and satisfy `public_off <= data_off < image_size`.
- Let `stored_blob_bytes = align_up(size + 1, 4)`, computed in 64-bit
  arithmetic and required to fit in `uint32_t` without overflow.
- If `data_off < private_off` (public part), require
  `data_off + stored_blob_bytes <= private_off`.
- If `data_off >= private_off` (private part), require
  `data_off + stored_blob_bytes <= image_size`.
- Bytes `[data_off + size, data_off + stored_blob_bytes)` must all be zero.
- If `stored_blob_bytes < PAGE_SIZE`, the stored range must not cross an image-
  page boundary; otherwise `data_off` must be `PAGE_SIZE`-aligned.
- `SYMLINK` payloads must contain no NUL in their announced bytes and are
  private-only: `data_off >= private_off`.

Content intervals, deduplication, and visibility:
- Build the set of stored intervals referenced by filenames, regular files,
  and symlinks. Two references may share an interval only if their domains and
  announced bytes are identical; all distinct intervals must be disjoint.
- Canonical-byte check: within the shared regular-file/filename domain,
  references with identical announced bytes must have the same
  `data_off`/`name_off`. Symlink targets are a separate private-only domain and
  must not alias shared-domain intervals.
- A shared-domain interval must be in the public part if and only if at least
  one of its references is public under **Visibility Classification**. Regular-
  file references include every `FILE`/`HARDLINK` path occurrence that resolves
  to the interval's canonical file. This check uses the validated root-
  reachable object graph and resolved attributes.
- Canonical-byte check: every byte in `[128 + meta_size, public_off)` and every
  byte in the content region not covered by a unique stored interval must be
  `0x00`.

Hardlink rules:
- `data_off` (target index) must be `< cell_count`.
- Target must be a discovered `FILE` object outside all directory slices
  (never `DIR`, `SYMLINK`, `FIFO`, `HARDLINK`, `ATTR`, `NULL`, or a bucket).

Builder source-tree mapping policy:
- Supported source inode kinds are regular file, directory, symlink, and FIFO.
- Additional directory entries that reference the same regular-file inode are
  encoded as `FEMTOFS_TYPE_HARDLINK`.
- Hardlinks to non-regular files are not supported.
- If multiple source paths reference the same FIFO inode, image build must fail
  explicitly.
- Unsupported source inode kinds (device nodes, socket, whiteout, etc.) must
  make image build fail explicitly.

Reader behavior on violation:
- Any detected violation above is a hard format error; mount/open/regeneration
  must reject the image rather than applying repair heuristics. A kernel may
  omit only the checks explicitly marked canonical-byte.
