# Full-directory descendant indexes

Status: design proposal, 25 October 2026. This document records the proposed
extension; [specification.md](specification.md) describes the current format.
Format `0x0100` is frozen. The incompatible layout changes proposed here require
a new major format version, with builder, reader and generated images updated
together.

## Purpose

Each directory indexes every object in its subtree using a path relative to
that directory. Object metadata and file contents remain shared; the repeated
entries are lookup references, not additional filesystem objects or hardlinks.

For example, the same object can be reached through these keys:

```text
/               "usr/local/bin/bash"
/usr            "local/bin/bash"
/usr/local      "bin/bash"
/usr/local/bin  "bash"
```

The goals are to support a future lookup of several components at once, retain
ordinary component lookup, and keep directory listing proportional to immediate
contents. Current FreeBSD VFS would continue resolving paths component by
component; the additional descendant index gives it no automatic shortcut.

## Two regions per directory

Every directory owns one contiguous bucket slice, divided into:

```text
[ direct-child buckets, including holes ][ deeper-descendant buckets, including holes ]
```

Let `F` be the absolute first bucket index, `D` the direct-region bucket count,
`T` the total bucket count, and `N` the actual number of immediate children.

| Region | Absolute metadata indices | Keys |
|---|---|---|
| Direct children | `[F, F + D)` | Basenames without `/` |
| Deeper descendants | `[F + D, F + T)` | Relative paths containing `/` |

Every descendant appears once in this directory's combined index. The direct
region may contain holes; `D` is a slot count, not an occupied-entry count.
The builder must bound over-allocation, targeting roughly 50–100% occupancy
with suitable rounding. In particular, `D` must remain within a bounded factor
of `N`, so listing a small directory never scans a huge sparse prefix.

Basename lookup hashes modulo `D`. Multi-component lookup hashes modulo
`T - D`, then adds `F + D`. An empty selected region returns a miss before any
modulo operation. The existing hash-control byte and global second-hash base
can serve both regions, but parameter selection must evaluate both regions.
Equal primary and secondary anchors are traversed only once.

Each region has its own chains. The combined slice need not have a prime size;
region sizing, hash quality and the combined size bound must be considered
together.

## On-disk layout

All multi-byte fields remain little-endian; signed deltas use 16-bit two's
complement encoding. Cells remain 16 bytes and the header remains 128 bytes.

### Bucket cell

```c
struct femtofs_bucket {
    uint8_t  type;
    uint8_t  reserved;
    uint16_t attr_index;     // retained; zero for buckets
    uint32_t object_index;
    uint32_t name_off;
    uint16_t name_len;       // bytes 12–13, excluding NUL
    int16_t  next_delta;     // bytes 14–15, in metadata cells
};
```

The old reserved byte at offset 12 and 8-bit length become one 16-bit length.
The old slice-relative `next_index` becomes a displacement from the current
bucket. Zero terminates a chain; a valid chain never links a bucket to itself.
An empty `NULL` bucket is entirely zero. Attribute cells can still occupy
otherwise empty slots and retain their auxiliary-cell semantics.

The region determines whether a key is direct, leaving all 16 length bits
available. Keys must be nonempty and at most 65,535 bytes. Individual components
retain their 255-byte limit. Relative keys have no leading/trailing slash,
empty component, `.` or `..` component, or embedded NUL; the stored terminator
must agree with `name_len`.

Decode the delta as signed and calculate the destination in a sufficiently
wide type before checking bounds or narrowing. Its range is
`-32768..32767` cells. The builder must choose nearby collision-chain slots,
retry placement/hash selection, or reject a layout whose links do not fit.
Being in the same directory slice alone does not guarantee a representable hop.

For ordinary lookup, using absolute coordinates:

```text
if next_delta == 0:
    stop
next = current + next_delta
if next < F or next >= F + D:
    stop
```

Using slice-relative coordinates, the corresponding bounds are `[0, D)`.
Descendant lookup uses its own region bounds. Import validation must establish
that chains stay in their region, reach their required entries and terminate
without cycles; a runtime boundary stop alone does not establish validity.

### Directory cell and root descriptor

Replace the directory's 32-bit `bucket_count` word with two 16-bit fields:

```c
struct femtofs_directory {
    uint8_t  type;
    uint8_t  hash_p;
    uint16_t attr_index;
    uint32_t bucket_first;
    uint16_t direct_bucket_count; // offset 8: D, including holes
    uint16_t bucket_count;        // offset 10: T, both regions combined
    uint32_t parent_n;
};
```

The packed `parent_n` keeps its original meaning: low 16 bits are the actual
parent-directory index (`0xffff` denotes root), the next 15 bits are `N`, and
the top bit is reserved. `N` counts immediate children, not shortcuts.

Root needs the same split: replace header `root_size` with
`uint16_t root_direct_bucket_count` followed by `uint16_t root_bucket_count`.
Keep `root_first`, `root_real` (actual immediate-child count), `root_attr` and
hash fields in their existing storage widths.

Other object and attribute layouts remain unchanged. Directory listing uses
the direct bucket region itself; no separate listing blob, enumeration array
or companion cell is required in this version of the plan.

## Lookup API and listing

The shared primitive conceptually accepts `(directory_inode, name, length)`.
It can perform an exact basename or relative-path-key lookup. It must always
honour the supplied length: the backing buffer may contain further components
beyond that range.

Ordinary `VOP_LOOKUP` remains confined to the direct-child region, including
its initial hash anchor and chain traversal. `.` and `..` retain their current
special handling; `..` follows the actual parent reference. They are not stored
as index keys. `readdir` synthesizes them and scans only the direct region,
skipping empty and auxiliary cells. Its offset/cookie bounds must also use the
direct region rather than the entire slice.

A new path-lookup VOP would explicitly expose the multi-component capability.
Adding it requires a modified kernel interface. The filesystem operation is an
index lookup within one image; a future VFS path resolver must provide the
permission checks, symlink expansion, mount crossings and process-root rules
needed for pathname resolution. In particular, an index hit cannot establish
that a runtime mount has not obscured an intermediate directory.

Stock FreeBSD 15.1 documents `VOP_LOOKUP` as a single-component operation, and
its pathname walker excludes `/` from the supplied component length. Other
kernel callers are responsible for obeying that contract.
See the [manual source](https://github.com/freebsd/freebsd-src/blob/releng/15.1/share/man/man9/VOP_LOOKUP.9)
and [pathname walker](https://github.com/freebsd/freebsd-src/blob/releng/15.1/sys/kern/vfs_lookup.c).

## Canonical tree and visibility

Only direct entries define the filesystem tree. Build and validate that tree
first. Descendant shortcuts must agree exactly with paths through its directory
edges, ending at the referenced object. They do not traverse symlinks in the
stored image; a shortcut may name a symlink itself.

Repeated shortcut references must not overwrite parents, increase object or
hardlink counts, alter directory link counts, or promote file payloads to the
public content part. The current decoder's one-bucket-per-object discovery
logic must be replaced with direct-entry discovery plus shortcut verification.

Visibility of a shortcut name follows the actual path, not just the directory
holding the index. Root's index contains keys for private descendants too.
Applying root's visibility to all those strings would expose hidden names in
the public arena, potentially through neighbouring bytes in leaking mappings.
All copies of those keys must obey the existing public/private content rules.

## Bounds, cost and implementation impact

- Retain the current limit of fewer than `2^15` non-root object records.
  Hardlink records are included in that count. Each subtree therefore has at
  most 32,767 indexed entries, and immediate-child counts still fit `N`.
- Lift the global `<2^16` metadata-cell rule. `cell_count`, `meta_size`, slice
  starts and object references already have 32-bit fields. Keep attributes
  within reach of their 16-bit references and parent-addressable directory
  objects below index `0xffff`. Construction must budget that low-index area.
- Require `0 <= D <= T <= 65535`, enough slots in each region for its entries,
  and no empty-region division. Prime rounding and occupancy targets must fit
  the combined limit. A directory with no children has no descendants either.
- Retain the image-size limit below `2^32` bytes. Check all cell-to-byte
  multiplication, interval ends, signed-delta arithmetic and allocation sizes
  in wide arithmetic before narrowing. Merely removing the current cell-count
  check would make existing 32-bit arithmetic unsafe.
- Keep format key-length bounds separate from OS pathname-input bounds.
  FreeBSD 15.1 defines `NAME_MAX = 255` and `PATH_MAX = 1024`; a per-call pathname
  limit does not by itself bound a tree constructed through relative operations.
  See [syslimits.h](https://github.com/freebsd/freebsd-src/blob/releng/15.1/sys/sys/syslimits.h).

For object depth `d(o)` measured in components from root, the total occupied
bucket count is `E = sum(d(o))`. With maximum depth `H`, this is `O(objects * H)`;
a chain of directories gives quadratic growth in object count. At 32,767
objects, that chain has 536,854,528 references: almost 8 GiB of occupied buckets
alone, exceeding the image limit. The object limit therefore cannot replace a
cumulative size/depth budget. Repeated relative-path strings add further cost.

Lookup hashes the query bytes and scans a collision chain: expected bucket
work can be small, but worst-case work depends on chain length and compared
key lengths. The immutable builder can measure both regions' maximum chains.
Listing costs `O(D)` bucket visits, with `D` bounded relative to direct contents.
Validation must avoid repeatedly rediscovering entire subtrees for shortcuts.

Current [decoder](ko/image.c) allocations scale with metadata cells, including
unused slots, rather than only unique objects. A 64-bit host measurement gives
76 retained bytes and approximately 52 additional temporary bytes per cell,
besides the resident image. Expanded indexes therefore need an explicit import
memory budget; their disk-byte cost alone understates RAM impact.

The builder needs per-directory index-entry records holding a target object,
relative key and key-blob reference. A single `Node.nameBlob` cannot represent
all keys for that object. Directory planning, hash scoring, placement, encoding,
validation and tests must handle both regions and the enlarged cumulative table.

## Observed tree and verification work

Counts from toolbox's `work/root-dev`, measured on 25 October 2026:

| Quantity | Count or bytes |
|---|---:|
| Directories, including root | 208 |
| Non-root entries | 3,275 |
| Occupied buckets with all ancestor indexes | 11,873 |
| Additional occupied-bucket bytes at 16 bytes each | 137,568 |
| Relative-key bytes including NUL, before deduplication | 245,187 |

These are tree-derived counts, not a built-image size or performance result.
Empty buckets, alignment, deduplication and decoded metadata affect actual cost.

Implementation verification must cover empty and leaf directories; both hash
regions and their density; root's split descriptor; positive and negative
lookups; signed-delta limits and cycles; metadata above 65,535 cells; malformed
keys and inconsistent shortcuts; canonical parents and hardlinks; private
filename/payload visibility; and listing offsets that attempt to enter the
descendant region. Native component lookup and directory listing must remain
correct on unmodified FreeBSD VFS. The new VOP and traversal integration require
their own modified-kernel verification.
