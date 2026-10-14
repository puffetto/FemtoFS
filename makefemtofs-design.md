# Design: `makefemtofs`

Status: initial host-side builder implemented; FreeBSD raw-device integration
still requires on-platform verification.

The command name is `makefemtofs`.

## 1. Purpose and boundaries

`makefemtofs` builds one raw femtoFS filesystem image from either:

- a directory tree, walked through libarchive's disk reader; or
- any archive format/filter accepted by the installed libarchive.

It writes the image to either a regular file or a raw device. The output is a
femtoFS volume beginning at byte zero; it does not contain a partition table.

The builder must preserve the source-tree information representable by the
format: regular files, directories, symlinks, FIFOs, regular-file
hardlinks, numeric uid/gid, and all Unix mode bits. Unsupported inode kinds or
metadata must be reported rather than silently converted.

Image creation is an offline operation. Deterministic layout, strict input
validation, safe output publication, mounted lookup performance, and final
image size take priority over build latency. Construction efficiency remains
important, but it is not a primary design objective.

## 2. Format contract used by the builder

The builder targets the format in `specification.md`, short semantic version
`0x0100` (major 1, revision 0). The header records the exact filesystem length
directly:

```c
uint8_t  magic[4];
uint8_t  format[2];  /* { byte order, page shift minus 8 } = {0, 4} */
uint16_t version;    /* 0x0100 */
uint32_t image_hash;
uint32_t meta_hash;
uint32_t uuid[4];
uint32_t image_size;  /* exact femtoFS byte length, less than 2^32 */
```

The builder and verifier require:

```text
0 < header.image_size <= backing_provider_size
0 < header.cell_count < 2^16
header.meta_size == header.cell_count * 16
128 + header.meta_size <= public_off <= private_off <= header.image_size < 2^32
public_off % 4096 == 0
private_off % 4096 == 0
header.image_size % 4096 == 0
```

All content bounds and the image FNV calculation use
`header.image_size`, not the size of the backing provider. A regular image file
created by this tool has an EOF exactly at `image_size`; bytes after
`image_size` on a device are outside the filesystem and are ignored.

`image_hash` is computed over the exact half-open range
`image[offsetof(struct femtofs_header, uuid), image_size)`, ending with the byte
at `image_size - 1`.
That range includes `uuid`, `image_size`, the remainder of the header,
metadata, padding, and content. It excludes the 16-byte prefix containing
`magic`, `format`, `version`, `image_hash`, and `meta_hash`.

The first 32 header bytes form the image fingerprint:

```text
magic | format | version | image_hash | meta_hash | uuid
```

`meta_hash` depends only on the metadata table, while `image_hash` does not
include the stored `meta_hash`. The two values have no circular dependency and
can be computed independently. The 32-byte fingerprint is suitable for fast
same-image comparison, but not for cryptographic authentication.

The builder writes the exact fixed `author[56]` identifier, zeroes `root_pad`,
and writes `hash2_base == 0` unless at least one directory uses dual-hash mode.
Every attribute has `ext_off == 0`; version `0x0100` does not define metadata-
extension payloads. A future major-1 revision may define them without a major-
version bump: older major-1 consumers ignore unknown extension metadata and
continue to use classic `mode`, `uid`, and `gid` semantics.

The initial builder always emits format `{0, 4}`: little-endian integers and a
4096-byte image page. It must use the encoded target page size rather than the
build host's `getpagesize()` value. A reader must reject version `0x0100`
images with any other format bytes. A kernel whose VM page is no larger than
the encoded image page does not need a page-size-driven mount-mode downgrade.
A kernel with a larger VM page may still mount the image but must enforce
effective `clean` behavior; page-size mismatch alone is not a mount rejection
condition.

Byte-order code `1` is reserved specifically for a future big-endian format;
the initial builder does not emit it. Additional byte-order and page-size codes
may be defined in the future. Power-of-two page sizes from 256 bytes upward
preserve the alignment of the 128-byte header and 16-byte metadata cells, but
the builder must continue to enforce the format's offset, image-size, and
checked-arithmetic constraints.

## 3. Command-line interface

The initial interface is:

```text
makefemtofs [options] INPUT OUTPUT

  -f, --force                 replace an existing regular output file
      --device                authorize OUTPUT when it is a raw device
      --dry-run               plan and validate, but do not write OUTPUT
      --strip-components N    remove N leading archive path components
      --synthesize-dirs       create omitted archive parents explicitly
      --root-mode OCTAL       override/default archive root mode (default 0755)
      --root-uid UID          override/default archive root uid (default 0)
      --root-gid GID          override/default archive root gid (default 0)
      --traverse-mounts       cross mount points while reading a directory
      --uuid UUID             use a supplied UUID instead of a random UUID
      --temp-dir DIR          location for the payload spool
      --verify MODE           full (default) or structure
  -v, --verbose               print planning and packing statistics
  -V, --version
  -h, --help
```

`INPUT` and `OUTPUT` are paths in the first implementation. Standard input is
not necessary because libarchive input can already be consumed in one pass;
it can be added later without changing the builder core. Standard output is
not proposed because safe publication requires final-header commit semantics.

Input kind is selected with `lstat(2)`:

- a directory uses `archive_read_disk_new()`;
- a regular file uses `archive_read_new()` with all compiled-in formats and
  filters; and
- every other input type is rejected.

Output kind is deliberately stricter:

- a missing path or regular file is a file output;
- a block/character device is accepted only with `--device`; and
- symlinks and all other output kinds are rejected.

The output must not identify the input archive itself (including through a
hardlink). For a directory input, the resolved output location must also be
outside the input tree. Perform these identity/location checks before creating
or opening any output target.

`--force` never authorizes a device. `--device` is the explicit destructive
operation acknowledgement. On FreeBSD the tool also checks the provider with
`DIOCGMEDIASIZE`/`DIOCGSECTORSIZE` and refuses a mounted provider. It writes at
offset zero only; callers wanting a partition should pass the partition
provider, not a whole disk plus an offset.

The initial implementation returns `0` on success and `1` for any diagnosed
failure. More granular exit classes can be added when callers need them.

## 4. Architecture

Keep source ingestion separate from femtoFS planning and byte emission:

```text
directory/archive
       |
       v
 libarchive EntrySource
       |
       v
 normalized Tree + BlobStore spool
       |
       v
 visibility, dedup, hash and packing planners
       |
       v
 immutable ImagePlan
       |
       v
 regular-file or device ImageSink
       |
       v
 structural/full verifier
```

Initial source layout:

```text
include/femtofs/format.h
include/femtofs/hash_plan.h
programs/makefemtofs.cpp
units/test-format.cpp
units/test-hash-plan.cpp
units/test-makefemtofs.sh
```

The implementation keeps ingestion, planning, writing, and verification as
separate functions in one translation unit for now. They can move to builder
library units when another program needs those interfaces.

`format.h` is shared by the builder, validator, simulator, and eventual kernel
reader. It contains only fixed-width on-disk structures, constants, endian
helpers, and compile-time layout assertions. No native C/C++ structure is
written with a raw `write()`; every integer is encoded explicitly as little
endian.

The main in-memory records are conceptually:

```text
Node
  normalized full path and basename as byte strings
  type, uint32 uid/gid, uint16 st_mode
  parent/children references
  source hardlink identity and canonical object reference
  regular payload or symlink-target BlobId
  final metadata-table index

BlobClass
  domain (shared file/name or private symlink)
  announced size, spool offset, digest
  public occurrence flag
  selected content part and final image offset

DirectoryPlan
  object index (or root marker), parent index
  ordered children
  hash mode, p1 index, table size and bucket cells

ImagePlan
  final header fields, metadata role map, metadata bytes
  ordered, non-overlapping blob placements
```

All planning arithmetic uses checked 64-bit values. Values are narrowed only
after proving the femtoFS field limit.

## 5. Input ingestion with libarchive

### Directory source

Use `archive_read_disk_new()`, physical symlink handling, and explicit descent.
Do not follow symlinks. By default set the no-mount-traversal behavior so a
tree containing `/dev`, `/proc`, or another mounted filesystem is not captured
accidentally; `--traverse-mounts` opts in.

The root directory's `lstat` metadata supplies root attributes unless a root
override was given. Regular-file identity is `(st_dev, st_ino)`. Repeated
directory entries for the same regular inode become one canonical file object
and hardlink objects for the other paths. Repeated inodes of any other kind
are rejected as required by the specification.

### Archive source

Use `archive_read_new()`, `archive_read_support_filter_all()`,
`archive_read_support_format_all()`, and `archive_read_open_filename()`. Data is
read with `archive_read_data()`, whose libarchive contract reconstructs sparse
logical gaps as zero bytes without extracting a temporary tree.

An explicit archive hardlink pathname is normalized with the same rules as an
entry pathname and may resolve forward or backward. Formats such as cpio may
express hardlinks by device/inode/nlink metadata instead; when those fields are
present and `nlink > 1`, they form the source identity. Every group must resolve
to a regular file and have consistent attributes. Explicit link placeholders
derive size and content from their target. For inode groups, empty placeholders
are allowed; multiple data-bearing members must be byte-identical. An all-empty
group represents an empty file. Hardlink chains are collapsed so an emitted
hardlink always targets a non-hardlink file cell.

An explicit `.` root directory entry supplies archive-root attributes. If it
is absent, the root defaults/overrides from the CLI are used. `--root-mode`
specifies permission/special bits; the builder always adds the required
`S_IFDIR` type bits. Missing non-root parent directory entries are errors by
default; `--synthesize-dirs` creates them with the selected root attributes and
emits a warning. No top-level directory is stripped implicitly.

### Common normalization and policy

Paths are handled as POSIX byte strings rather than locale-converted
`std::filesystem::path` values. Reject NUL-containing and absolute pathnames
before removing anything. A directory entry's trailing slashes are then
removed; a trailing slash on any other entry type is an error. Normalization
next removes leading `./` components and the requested `--strip-components`
components, then rejects:

- `..`, empty interior components, and a path reduced to empty unless a
  directory entry deliberately normalizes to the image root;
- components longer than 255 bytes or named `.`/`..` after normalization;
- duplicate normalized paths; and
- a child whose normalized parent is not a directory.

No archive is extracted to the host filesystem, so archive entry paths cannot
write anywhere on the host. Symlink targets are stored verbatim and are not
followed; absolute targets and `..` in a target are valid filesystem content
and are not a builder escape risk.

For every entry, copy numeric uid/gid and `st_mode` type/permission bits from
the libarchive entry. Reject negative or out-of-range IDs, mode bits that do not
fit the on-disk `uint16_t`, type/mode disagreement, a declared size that cannot
be encoded, devices, sockets, whiteouts, and all other unsupported types. ACLs,
xattrs, and file flags are not representable in version `0x0100`. The initial
builder deliberately ignores them and emits one aggregate warning. Classic
`mode`, `uid`, and `gid` are the complete access-control input for the emitted
image.

Libarchive warnings are promoted to errors unless the operation is explicitly
documented as harmless. Diagnostic messages include the normalized entry path,
archive error string, and archive errno.

## 6. Blob spooling and exact deduplication

The complete tree must be known before visibility and placement can be
decided, but archive input may be compressed and non-seekable. Stream every
regular payload once into an unlinked or mode-0600 temporary spool file while
computing a size plus 64-bit hash key.

Hash matches are candidates, not proof of equality. Compare candidate bytes
from the spool before merging them. The initial implementation retains source
copies in the temporary spool but emits only one content blob per exact class.
RAM use remains proportional to metadata rather than payload size while the
output satisfies the specification's exact-byte dedup rule.

Regular-file payloads and filename bytes enter one shared blob domain. This is
important: a file whose bytes are exactly a filename may share that blob.
Identical symlink targets may share a separate, private-only domain, but never
deduplicate with file/name blobs. The canonical encoded size is always:

```text
align_up(announced_size + 1, 4)
```

The writer obtains the announced bytes from the spool, appends a NUL, and adds
zero alignment bytes. Empty regular files still have one four-byte stored blob.

For a directory input, the reader verifies the number of bytes read and checks
file identity/size around the read. A mutation detected during the build is a
hard error rather than a potentially inconsistent image.

The source archive or directory tree is required to remain unchanged from its
initial inspection until output publication. The builder's identity, size,
and alias checks detect selected concurrent changes, but they do not provide a
filesystem snapshot and cannot detect every mutation.

## 7. Planning the image

Planning is deterministic apart from the default random UUID. With the same
tree, options, tool version, and explicit `--uuid`, directory and archive input
representing the same metadata produce identical bytes.

### Tree and object indices

Sort paths by unsigned bytewise pathname order. The lexicographically first
path in each regular hardlink group owns the canonical `FILE` object; remaining
paths receive `HARDLINK` objects. Allocate all non-root filesystem object cells
in that order. Root remains in the header. Emit exactly one occupied bucket
targeting each non-root object; no two buckets target the same object cell.

Check early that every directory has fewer than `2^15` entries and that every
filename meets the on-disk restrictions. Reject a plan with `2^15` or more
non-root objects before allocating metadata.

### Visibility and blob promotion

Compute public eligibility exactly as `specification.md` defines it:

- a canonical regular-file inode is eligible when its resolved mode is world-
  readable and at least one of its `FILE`/`HARDLINK` occurrences has world-
  search on every directory in that occurrence's path; and
- a filename occurrence needs world-read and world-search on every directory
  through its containing directory.

The filename rule intentionally treats a world-searchable but non-world-
readable ancestor as private. Thus a layout such as `/hide/$uuid/...`, with
`/hide` executable but not readable by others, permits traversal of a known
path without making descendant names eligible for public-part spill.

OR the result across every occurrence of a shared blob class. A class with any
public occurrence is placed once in the public part; all other shared classes
go in the private part. Symlink targets always go in the private part.

### Directory hash tables

Shared prime growth and candidate sampling, hash scoring, and deterministic
single/dual selection live in `include/femtofs/hash_plan.h` and are used by both
the simulator and builder.
The builder-specific global metadata budget and the simulator's experimental
reporting remain in their respective programs. The initial builder uses this
mixed policy:

1. evaluate the reference single-hash choices from `SMALL_PRIMES`;
2. classify a directory as hard when its best single-hash maximum chain is
   greater than two;
3. test 100 `hash2_base` prime candidates using the fixed seed
   `0x0F5F2026`; and
4. select by maximum chain, weighted score, bucket count, a proxy for
   unsuccessful lookup cost, then numeric prime value.

A hard directory switches to dual mode only when the selected dual choice
strictly improves its own `(maximum chain, sum of squared chain lengths)` pair
in that priority order. Otherwise it keeps its single-hash choice; equal hash
quality does not justify the second lookup probe.

The shared hash primitive takes a full `uint32_t` base and performs every
multiply-add with `uint32_t` wraparound. `hash2_base` must never be narrowed.
The planner records the index of the selected `SMALL_PRIMES` entry in `hash_p`,
not the prime value itself; empty and one-entry single-hash directories use
`p1_index == 0`, and empty directories use `tablesize == 0`. If no directory
selects dual mode, write `hash2_base == 0`.

Sort child names before every simulation so archive enumeration order cannot
affect the result. Hash-policy options can be exposed later, but the default
policy and seed are part of the builder's reproducibility contract.

Hash selection is subject to the global metadata budget, not just each
directory's `MAX_TABLESIZE_PRIME`. After counting object and deduplicated
attribute cells, account for attributes that can reuse empty buckets. If the
preferred mixed plan is too large, replace one directory at a time with its
minimum single-hash plan, choosing the largest bucket reduction first and
breaking ties in directory-plan order (root first, then bytewise path order).
Stop as soon as the shared table fits below `2^16` cells, preserving the
preferred choices for all remaining directories. The final fallback is
`tablesize == N` for every directory. Report a format-limit error only if even
that minimum layout plus required objects and attributes does not fit.

For physical bucket construction, first group names by their chosen anchor,
reserve every non-empty anchor as a chain head, then put remaining chain
members in the lowest-numbered unreserved cells and link with slice-relative
indices. This prevents one chain's overflow cell from consuming another
chain's anchor. Verify every emitted bucket is reachable from at least one of
the anchors computed from its own filename, and traverse every computed anchor
through its sentinel to prove it is cycle-free and never enters an empty or
auxiliary cell.

Allocate every directory slice as a disjoint metadata interval. Object cells
must remain outside all slices; only `NULL`, occupied bucket, and auxiliary
attribute roles may occur inside them.

### Attribute cells

Build object cells and directory bucket slices, then deduplicate exact
`(mode, uid, gid, ext_off)` tuples. `ext_off` is zero in this version. Follow
the specification's same-page placement preference using deterministic
reference order: root first, then object indices. Treat the root descriptor as
residing in the header page for this policy. Empty bucket cells may be replaced
with `ATTR` auxiliary cells; chain construction must never reference them.
Extend the table only when no eligible unused cell remains.

After placement, reject `cell_count >= 2^16`, re-check every index and slice,
and encode directory parent/N words with checked narrowing. Verify that every
directory object's parent field matches the directory bucket containing it,
that every bucket type matches its target object type, and that the complete
object graph is reachable from root with exactly one incoming bucket per
non-root object. Every deduplicated attribute cell must have at least one
reference; all unused non-slice cells and all unused fields of empty bucket
cells are zero.

### Content packing

Pack public and private classes independently. Sort by encoded size descending,
then by digest and finally exact bytes to make ties deterministic. For each
class, use the smallest fitting hole (hole-size then offset order); otherwise
append, moving to the next 4096-byte page and recording the gap if the blob
would straddle a page.

Every placement starts on a 4-byte boundary. Encoded blobs smaller than 4096
bytes remain within one image page; larger blobs start on an image-page
boundary. Distinct stored intervals never overlap. Shared file/name classes are
placed in the public part if and only if at least one occurrence is public;
symlink-target classes remain separate and private even when their bytes equal
a file or filename.

Layout calculation is:

```text
meta_size   = cell_count * 16
public_off  = align_up(128 + meta_size, 4096)
private_off = align_up(public_off + public_packed_tail, 4096)
content_end = private_off + private_packed_tail
image_size  = align_up(content_end, 4096)
```

Every canonical terminator/alignment suffix, packing hole, alignment gap, and
final partial page is zero. The final page alignment makes file and device
output identical and makes the full image safe to write in page-sized chunks.

### Header and integrity values

Generate a random 128-bit UUID with the platform CSPRNG unless `--uuid` was
provided. `--uuid` accepts canonical `8-4-4-4-12` hexadecimal text; its 16
display-order octets are written consecutively to the on-disk `uuid` byte
range. Treat the field as 16 opaque bytes at this boundary rather than relying
on a host UUID structure's field endianness. Fill every required padding and
role-reserved field with zero and the defined author field exactly.
Compute `meta_hash` over the finalized metadata bytes and independently compute
`image_hash` over
`image[offsetof(struct femtofs_header, uuid), header.image_size)`. FNV state
updates are streaming; the complete image is never held in memory.

Before writing, run structural validation plus all canonical-byte checks over
the immutable plan. A plan that fails its own validator is an internal error.

## 8. Safe output and publication

The writer uses shared positional-write functions and an explicit zero-range
operation. The regular-file sink may issue arbitrary
byte-range writes. The device sink initializes the image with complete zeroed
4096-byte pages and services logical byte ranges with complete-page
read/modify/write operations. Every device offset and I/O length is therefore
aligned to a sector size that passed the divisibility check below.

For a regular file:

1. create a mode-0600 temporary file in the output directory with
   `O_CREAT|O_EXCL|O_NOFOLLOW`;
2. write and verify the complete image;
3. apply the final mode (0644 by default) and fsync it;
4. immediately before publication, re-check that the output is neither the
   input archive nor located inside a directory input tree;
5. publish without replacement via an atomic no-clobber operation, or use
   atomic rename-over only when `--force` permits; and
6. fsync the parent directory.

The late alias check narrows the time-of-check/time-of-use window but does not
eliminate it. Source immutability for the complete construction interval is an
explicit caller precondition, as described in **Blob spooling and exact
deduplication**.

Before publication, an error removes only the private temporary output and
leaves an existing destination untouched. After publication, a parent-directory
fsync failure reports an error with uncertain durability; it does not roll back
the published image.

For a device:

1. require `--device`, validate type/capacity/sector size, check it is not
   mounted, ensure `image_size` fits, and require its sector size to divide the
   4096-byte format page;
2. write and flush a zeroed first 4096-byte page before any other device write,
   then zero the remaining image with complete page writes, keeping the target
   unrecognizable as femtoFS during data writes;
3. write the image with the first 16 header bytes (`magic` through `meta_hash`)
   still zero and header bytes 16 through 127 populated, then flush;
4. rewrite the first page with the final header as the commit record and flush
   again; and
5. re-read and verify the result.

Any failure before the commit write leaves the header invalid if the underlying
device honored the preceding flush. A failure during the commit write or its
final flush has indeterminate persistence: the tool re-reads and validates the
provider, reports whether it currently contains a valid image, and never
claims rollback or crash atomicity. Bytes beyond `header.image_size` are not
erased.

`--verify=full` reopens the result, validates every metadata relationship,
checks exact deduplication and zero padding/hole bytes, and recomputes
`meta_hash` over the metadata table and `image_hash` through the byte at
`image_size - 1`.
`--verify=structure` omits `image_hash`, exact-deduplication comparisons, and
the complete unreferenced-byte scan, but still validates header, metadata roles
and graph reachability, offsets, interval non-overlap, canonical suffixes, page
placement, visibility, and `meta_hash`.

## 9. Reusable material already present

The HaliveOS snapshot contains useful patterns, but its extraction API cannot
be reused wholesale:

- `tmp/haliveos_code/libsource/archiveExtract.cpp` has reusable libarchive
  error reporting, filter/format setup, pathname normalization ideas, and a
  bounded buffered read loop. Its public operation extracts to disk, discards
  source ownership/mode, and rejects symlinks, hardlinks, and FIFOs, so it does
  not satisfy this builder's source model.
- `tmp/haliveos_code/libsource/zfsDeltaArchive.cpp` contains small RAII archive
  and file-descriptor guards and careful archive error conversion that can be
  generalized for the reader side.
- `tmp/haliveos_code/libsource/moduleTree.cpp::generateArchive()` demonstrates
  secure extraction flags, but extraction through `archive_write_disk` is the
  wrong architecture here: it adds temporary-tree I/O, loses archive-level
  hardlink intent, and introduces host-filesystem interpretation.
- `tmp/haliveos_code/cmake/import.cmake` confirms the portable CMake path:
  `find_package(LibArchive REQUIRED)` and `LibArchive::LibArchive`. The large
  static-dependency machinery is project-specific and should not be copied.

The reusable implementation in this repository is divided at the consumer
boundary:

- `include/femtofs/hash_plan.h` contains shared prime growth and sampling,
  directory scoring, and deterministic single/dual hash selection.
- `programs/femtofsSim.cpp` contains experimental hash policies, its visibility
  model, and best-fit page-hole packing simulation.
- `include/isPrime.h` supplies the prime test used by that planner.

The simulator's arbitrary-policy experiments, `find -ls` parser, reporting,
and inode-size-only payload model are simulation-specific and remain outside
the builder. Visibility or packing code should move to another shared unit only
when a second production consumer needs those exact interfaces.

## 10. Validation and test plan

Unit and integration coverage should include:

- exact on-disk structure sizes, offsets, little-endian encoding, and a golden
  minimal image;
- accepted `{0,4}` format bytes and rejection of unsupported byte-order or
  page-size codes;
- page-size compatibility policy: an equal or smaller VM page preserves the
  requested mode, while a larger VM page forces `clean` without rejecting the
  image;
- the UUID hash boundary, independent `meta_hash`/`image_hash` calculation,
  exact author field, and exact 32-byte fingerprint comparison;
- empty root, one entry, empty files, page-edge blob sizes, packing-hole ties,
  and maximum representable counts;
- all 53 `SMALL_PRIMES` indices, rejection of index 53, and independent root
  `N < 2^15`/`tablesize <= 65521` boundaries, plus `cell_count` values 65535
  and 65536;
- full-width `hash2_base`, modulo-`2^32` multiply-add behavior, zero
  `hash2_base` without dual mode, and rejection of buckets unreachable from
  both of their own anchors;
- directory/archive equivalence with a fixed UUID;
- tar, pax, cpio, zip, compressed filters, sparse files, forward/backward
  archive hardlinks, symlinks, FIFOs, and empty directories;
- duplicate paths, traversal paths, overlong names, missing parents,
  unsupported types, hardlinks to non-files, metadata mismatches, and files
  changing during a directory build;
- public/private propagation through non-searchable directories, hardlink
  occurrences with different paths, filename promotion, and cross-kind
  file/name byte deduplication;
- stable hash choices and bucket chains regardless of archive entry order,
  including complete anchor-chain termination checks;
- deterministic fallback from preferred hash layouts to the global minimum
  bucket budget, including cases where attribute reuse changes the fit;
- every normative validator failure in `specification.md`, including corrupted
  chain links, overlapping directory slices, bucket/object role confusion,
  wrong target or mode types, duplicate/missing object references and attribute
  cells, incorrect directory parents, nonzero `ext_off`, bad part bounds,
  hardlink-path visibility errors, overlapping blobs, noncanonical suffix/
  padding bytes, and integer overflows;
- output symlink refusal, existing-file preservation, out-of-space failure,
  input/output alias refusal, sector-aligned device I/O, failures before and
  during the commit write, and final-header commit behavior; and
- FreeBSD `md(4)` integration: write to a provider larger than `image_size`,
  mount it, compare the full tree and metadata, exercise all mmap modes, and
  confirm bytes beyond `image_size` are ignored.

Add libFuzzer targets for archive-path normalization and image validation.
Run builds under ASan/UBSan, and use deterministic fault injection around every
writer flush/pwrite boundary.

## 11. Implementation sequence and acceptance criteria

1. Centralize the specified format ABI (version `0x0100`, format `{0,4}`,
   recorded `image_size`, and 4096-byte image page), then build a standalone
   validator and golden-image tests.
2. Extract the simulator's hash/visibility/packing algorithms without changing
   simulator output.
3. Implement libarchive directory/archive sources plus tree and blob spooling.
4. Implement deterministic planning, metadata placement, and regular-file
   output with full verification.
5. Add guarded FreeBSD raw-device output and `md(4)` integration tests.

The initial implementation is complete when the same fixture built from a
directory and from at least tar and cpio archives yields the same verified
bytes with a fixed UUID, all representable metadata survives a mount/readback
test, malformed input cannot produce a committed image, and a failed build
cannot replace an existing regular output file or leave valid femtoFS magic on
a partially written device.
