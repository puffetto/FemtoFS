# FemtoFS

**Firmware-oriented Efficient Memory-mappable Table of Objects File System**

`FemtoFS` is a zero frills, zero fskcing, almost zero-copy, read-only filesystem image format designed for one job: get bytes
from storage into userspace fast, predictably, and without dragging around the
complexity of a writable filesystem pretending to be an appliance image.

If you want a compact image that mounts cleanly, looks up paths in O(1)-style
hashed directories, offers clean/leaking/dirty `mmap(2)` mount modes, and
never needs a repair utility, this is the point.

If you want a real filesystem you can write on at runtime, look elsewhere.

## Why It Exists

Most filesystems are built for mutation first and deployment second. `FemtoFS`
goes the other way.

It is built for:

- read-only system images
- Named resident image banks populated by the loader or synchronous imports
- small, memory-friendly metadata
- correct `uid`/`gid`/mode semantics for real system trees
- deterministic lookup behavior
- aggressive sharing of packed content bytes
- mode-selectable `mmap(2)` behavior:
  - `clean` (default): no neighboring-byte exposure
  - `leaking`/`dirty`: only public-part neighboring bytes may be exposed

That makes it a strong fit for base system payloads, installer media, rescue
environments, embedded OS/firmeare deployments, immutable appliances, VM images,
and container-like distribution formats, where correctness and startup latency
matter and you do not need write support.

## What Makes It Different

`FemtoFS` is intentionally small and sharp:

- A whole image is three regions: fixed 128-byte header, fixed-size metadata
  table, and one content region split into public/private parts.
- Every on-disk record uses fixed-width little-endian fields. No ABI roulette.
- Directories are hashed at build time, so lookups are cheap and predictable.
- Object metadata preserves full Unix mode bits plus numeric `uid`/`gid`.
- Regular-file payload and filename dedup classes can be promoted to the public
  part when any visible reference is public-eligible.
- Symlink targets and metadata-extension payloads stay in the private part by
  policy.
- `clean` mode guarantees POSIX-style `mmap(2)` alignment and EOF zero-fill.
  With matching image and VM page sizes it copies at most one page per mapped
  file object: the only page of a sub-page file or the partial tail page of a
  larger file.
- `leaking` removes the tail copy for suitably aligned public-part files not
  smaller than a VM page, at the cost of exposing neighboring public bytes
  instead of zero-filling the partial page after EOF.
- `dirty` also gives public-eligible sub-page files a copy-free shifted mapping.
  If the payload begins within a VM page, `mmap(2)` returns the
  correspondingly shifted, non-page-aligned address and leaves the surrounding
  public bytes visible. An incompatible non-fixed address hint is ignored; an
  incompatible `MAP_FIXED` request fails.
- There is no journal, no write path, no recovery dance, and therefore no
  `fsck` story to apologize for.

This is not a general-purpose filesystem. That is the advantage.

## Why FreeBSD as primary target platform

FreeBSD already has the right instincts for clean kernel interfaces and
practical system engineering. `FemtoFS` fits that culture:

- The on-disk ABI is simple enough to audit.
- The mount semantics are narrow enough to implement well.
- The read path is optimized for what the base system actually does a lot:
  open, lookup, read, and `mmap`.
- The format is honest about scope. It preserves the metadata that actually
  matters for a bootable tree: ownership and full mode bits, including
  setuid/setgid/sticky.
- It does not spend core ABI space on BSD file flags whose practical value is
  marginal on a read-only image.
- It leaves a forward hook for future ACL and extended-attribute support
  without bloating the common case.
- Security policy is explicit and auditable: private-part bytes are never
  exposed by neighbor spill in any mode; only public-part bytes may spill in
  `leaking`/`dirty`.

Filename privacy intentionally requires world-read as well as world-search on
every ancestor directory. A known path below an executable but unreadable
directory such as `/hide/$uuid/` can still be traversed normally, while its
descendant names remain private-part content.

For maintainers, this means less policy hidden in edge cases. For users, it
means faster cold starts, less metadata overhead, and a filesystem that behaves
like a deployable artifact instead of a tiny database.

## Design Snapshot

- Current kernel-module target: FreeBSD 15.1 `amd64`; 14.x and `arm64` are
  future ports and have not been verified
- Mode: read-only
- Image size limit: `< 2^32` bytes
- Metadata-table cell limit: `< 2^16`
- Non-root visible-object limit: at most 32,767, for a trimmed appliance tree
- Per-directory entry limit: `< 2^15`
- Runtime storage: the complete image remains in wired RAM for the lifetime of
  its resident bank
- Root directory metadata stored directly in the header
- Full Unix metadata carried through deduplicated attribute cells:
  `uid`, `gid`, and complete `st_mode`
- Supported object kinds: regular file, directory, symlink, FIFO, and regular-
  file hardlink
- Directory entries, object metadata, and deduplicated attribute records share
  the same 16-byte metadata table
- Per-directory single/dual hashing is selected deterministically at build time
- All 32-bit content offsets are image-relative; `private_off` is the public/
  private boundary classifier
- Hardlinks resolve by canonical object index
- One content region split into public/private parts separates spill-safe
  public bytes from private bytes
- Regular-file payload classes are public/private by eligibility and dedup
  promotion
- Filename string classes are public/private by directory visibility and dedup
  promotion
- Symlink targets remain private-part content
- Per-part packing keeps sub-page objects page-contained and page-aligns large
  objects automatically
- `mmap` mount modes:
  - `clean` (default): page-aligned + zero-fill outside file range
  - `leaking`: page-aligned, public-part spill allowed
  - `dirty`: `leaking` plus shifted zero-copy for public-eligible sub-page
    files
- One reserved extension pointer in each attribute record leaves room for
  future ACL/xattr payloads in private-part content
- Metadata-table integrity check is deterministic FreeBSD kernel FNV-1
  (`fnv_32_buf`, `FNV1_32_INIT`) over metadata bytes
- Optional whole-image integrity check uses deterministic FreeBSD kernel FNV-1
  over bytes from `uuid` through end-of-image, independently of `meta_hash`,
  with a random 128-bit `uuid` in the header for same-image identity checks

The result is a format that is easy to generate, cheap to mount, and pleasant
to reason about under a debugger.

## What You Do Not Get

You do not get in-place writes, journaling, or a kitchen-sink metadata model.
Version 1 intentionally skips BSD file flags and does not yet define ACL or
extended-attribute payloads, even though the format leaves space for them
later. If you need a mutable filesystem, use one. `FemtoFS` is for shipping
known-good trees efficiently and mounting them with minimal drama.

The current `makefemtofs` ignores source ACLs, extended attributes, and file
flags with an aggregate warning. The resulting image preserves and enforces
only numeric ownership and classic Unix mode bits.

Future metadata extensions can be introduced by a compatible major-1 revision.
Major-1 consumers that do not understand an extension ignore it and continue
to enforce classic `mode`, `uid`, and `gid`; such an incremental extension does
not require a new major format version.

## UFS+md versus FemtoFS: a minimal FreeBSD 15.1 distribution

Measured on **18 October 2026**, using the same trimmed **FreeBSD
15.1-RELEASE-p4 amd64** root tree produced by
[the compressed-boot acceptance test](ko/test-boot-compressed.py). This is a
working multi-user appliance tree with stock utilities, shared libraries,
configuration, locale/zoneinfo data, and kernel/module files, rather than a
complete base installation with development files and documentation. It has
**4,076 directory entries**, **287 directories including root**, and **2,904
regular-file inodes**. The compressed FemtoFS image boots through stock rc,
console login and service checks; see [BOOTING.md](BOOTING.md).

Actual **uncompressed filesystem image lengths**, excluding the surrounding
boot disk, partition table and bootloader:

| Filesystem | Block / fragment or image page size | Bytes | MiB |
| --- | --- | ---: | ---: |
| UFS2 | 4,096 B blocks / 4,096 B fragments | 374,276,096 | 356.94 |
| UFS2 | 4,096 B blocks / 512 B fragments | 368,861,184 | 351.77 |
| FemtoFS (`bootme.0fs`) | 4,096 B image pages | 339,423,232 | 323.70 |

FemtoFS is **34,852,864 bytes (9.31%)** smaller than the first UFS image and
**29,437,952 bytes (7.98%)** smaller than the second. These include each format's
metadata and packing overhead, and FemtoFS's identical-content deduplication.
The same FemtoFS image compresses to **109,361,667 bytes** with gzip.

**512 bytes is the UFS fragment size, not its block size.** FreeBSD `makefs`
rejects 512-byte blocks; its minimum UFS block size is 4 KiB. Both measured UFS2
images were built with the following settings, changing only `fragment`:

```sh
for fragment in 4096 512; do
    sudo makefs -t ffs \
        -o version=2,bsize=4096,fsize=$fragment,minfree=1,optimization=space \
        ufs-frag$fragment.ufs appliance-root
done
```

Automatic sizing with `minfree=0` ran out of space while populating this tree.
With `minfree=1`, both builds completed; their actual unallocated space is
**1,691,648 bytes** and **3,053,056 bytes**, respectively. These are reproducible
build settings, not a claim of the smallest possible UFS images. Both passed
FreeBSD `fsck_ufs -fn` and read-only md mount checks. All **4,077 paths including
root** matched the source tree in type, mode, uid/gid, regular-file size and
SHA-256, and symlink target; the same checks passed for the decoded FemtoFS image.

Directory lookup analysis of this actual image:

- Replaying the specified lookup algorithm over every stored name gives
  **4,750 / 4,076 comparisons**, or **1.1654 per successful lookup**.
  **3,444 / 4,076 (84.49%)** finish with one comparison. The maximum stored
  chain length is **2**; probing both anchors can take **4 comparisons**.
  The simulator's default mixed policy independently reproduces these counts.
- Misses depend on the queried names. The independent uniform-anchor model
  gives **0.8807 comparisons per miss**, weighting directories by entry count
  and probing equal anchors only once.
- For comparison, a simple linear directory-scan model gives **76.73 name
  comparisons on success** and **152.46 on a miss**, using `(n+1)/2` and `n`
  for a directory with `n` entries and the same weighting. These are models,
  not measured UFS lookup counts: UFS `dirhash`, namecache hits and name-length
  filtering affect actual work. No lookup timing benchmark is claimed.

`mmap()` layout analysis with 4 KiB VM pages and FemtoFS `clean` mode:

- Select regular ELF files recursively under `/bin`, `/sbin`, `/usr/bin`,
  `/usr/sbin`, `/usr/libexec`, `/lib`, `/libexec`, and `/usr/lib`, including
  versioned shared libraries. Count hardlinked files once. This selects
  **1,170 file inodes** with **70,338 logical pages (274.76 MiB)**.
- Actual stored offsets make **69,168 pages (98.34%)** directly mappable from
  the resident image. The remaining **1,170 (1.66%)** are partial last pages
  requiring a zero-filled clean copy; none of the selected ELF files is smaller
  than a page.
- After identical-content deduplication, those files occupy **1,004 stored
  blobs**, with **64,388 distinct direct source pages** and **1,004 distinct
  clean tails**. Faulting every selected page would therefore allocate
  **4,112,384 bytes (3.92 MiB)** of clean-page storage, shared within the mount,
  in addition to the resident bank. Private writes allocate separate COW pages.
- UFS on a preload md retains its backing image and populates a separate file
  cache as data is accessed; processes mapping the same file share those cached
  pages. FemtoFS instead aliases the bank's eligible pages. The figures above
  describe whole files, not just the ELF segments touched during startup, and
  exclude kernel bookkeeping, page tables and private COW copies. They are
  layout counts, not measured process RSS, boot peak RAM or fault timings.

## Repository Status

The core format is documented in [specification.md](specification.md).
`makefemtofs` builds and verifies images from directory trees or any archive
format/filter supported by libarchive; the simulator in
[programs/femtofsSim.cpp](programs/femtofsSim.cpp) explores hash selection,
visibility split behavior, and packing overhead.

The portable `makefemtofs` suite covers construction, specification-derived
output checks, boundary cases, and deterministic publication/device-commit
fault injection, and passes on macOS and FreeBSD 15.1, including ASan/UBSan runs.

The [kernel module](ko/README.md) targets FreeBSD 15.1 with `clean` and `leaking`
mappings. Images live in resident banks: bootloader images are adopted without
copying; `femtoctl add` copies files or devices into named RAM banks. Mount by
label, unmount to retain, and explicitly discard unused banks. No kernel patches
or direct device mounts are part of this design. Mounting banks requires root.
Private writable mappings use COW; shared mappings remain read-only. Stock
FreeBSD 15.1 dynamic-shell root and UFS-control verification passes; commands
and coverage limits are described in the [module documentation](ko/README.md).
Asynchronous imports remain deferred.

Image construction is offline work. Its efficiency matters, but deterministic
output, safe publication, mounted lookup performance, and final image size have
higher priority than build speed.

The input archive or directory tree must remain unchanged for the entire image
construction. `makefemtofs` detects some concurrent changes and fails when it
does, but it does not provide snapshot semantics or complete TOCTOU protection.
It rejects archives containing no entries. Archive readers can accept a stream
truncated cleanly between entries, so callers must verify an archive checksum or
signature before building when completeness or authenticity matters.

Build with CMake:

```sh
cmake -S . -B build
cmake --build build
```

The test build also needs Python 3 and libarchive's `tar`/`bsdtar` for the
archive-format fixtures. Use `-DBUILD_TESTING=OFF` for a programs-only build.

Or build the programs with BSD make:

```sh
bsdmake -C programs
```

Create an image from a directory or archive:

```sh
build/makefemtofs input-directory output.0fs
build/makefemtofs input.tar.zst output.0fs
```

Raw mtree/METALOG input is rejected because it does not reliably carry regular-
file payloads and can otherwise silently encode empty files. Convert a FreeBSD
`METALOG` to an archive that carries both metadata and payloads first:

```sh
bsdtar -cf root.tar @METALOG
build/makefemtofs root.tar root.0fs
```

Run `makefemtofs --help` for reproducible UUIDs, archive path stripping,
synthetic parent directories, verification modes, and guarded FreeBSD device
output.

## Booting a compressed `.0fs` root image

See [BOOTING.md](BOOTING.md) for image preparation, bootloader settings,
read-only root configuration, and verified boot results.

## Pitch In One Sentence

`FemtoFS` is the filesystem for people who want immutable images to mount fast,
map safely, stay small, and never waste a boot on repair theater.
