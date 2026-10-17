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

- Target platforms: FreeBSD 14+, `amd64` and `arm64` (this is our "Tier 1", plugs are there for portability)
- Mode: read-only
- Image size limit: `< 2^32` bytes
- Metadata-table cell limit: `< 2^16`
- Non-root visible-object limit: `< 2^15`
- Per-directory entry limit: `< 2^15`
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

## Example Size and Cost Model for a Minimal FreeBSD Boot Image

Reference inputs:
- `FemtoFS` image-size estimate from `programs/femtofsSim` on `misc/list`
  (mixed hash default, 4 KiB pages)
- UFS image size from the corresponding space-optimized reference build:
  **202,829,824 B** (**193.43 MiB**)

These are size estimates and operation-count models, not mounted-filesystem
benchmarks or elapsed-time measurements.

Image footprint:
- `FemtoFS` estimated full image (public/private split): **179,146,752 B**
  (**170.85 MiB**)
- Delta vs UFS reference image: **-23,683,072 B** (**-11.68%**)

Modeled directory lookup cost for newly accessed paths:
- All averages below are entry-weighted over the same directory entries.
- UFS (no `dirhash`, no namecache hit): linear scan, about **`(n+1)/2`**
  `strcmp()` on success and **`n`** on miss in a directory with `n` entries
- On this dataset (`N=4041` entries across `252` directories), using
  entry-weighted directory sizes: rough UFS expectation is
  **~83.75** `strcmp()` success, **~166.49** miss
- Replaying the specified lookup algorithm over the bucket layout selected by
  the current `makefemtofs` policy gives **4,807 / 4,041** `strcmp()` calls on
  successful lookups: **1.1896** per lookup. **3,322 / 4,041** lookups
  (**82.2074%**) finish after one `strcmp()`; the maximum stored chain length
  is **2**, while a lookup can make up to **3** comparisons across both anchors.
- Unsuccessful lookup cost depends on the queried names. Under the simulator's
  independent uniform-anchor model, the current builder-equivalent policy
  averages **0.8229** `strcmp()` per miss. This includes queries starting at
  overflow buckets and probes equal anchors only once.
- UFS `dirhash` and VFS namecache can materially change real results, especially
  for repeated lookups. The counts above do not establish wall-clock performance
  or general superiority for either filesystem.

`mmap()` page-path estimate (UFS-on-md vs `FemtoFS`-on-md, `clean` mode):
- Selection rule used for estimate: non-symlink files ending in `.so`, plus
  regular files in `/bin`, `/sbin`, `/usr/bin`, `/usr/sbin`, `/usr/local/bin`,
  `/usr/local/sbin`
- After inode-identity deduplication, that rule selects **804 files** containing
  **18,628 logical 4 KiB pages**. The listing cannot reveal byte-identical
  payloads belonging to different inodes, which `makefemtofs` may deduplicate.
- With 4 KiB image and VM pages, current packing rules and the clean-mode
  contract classify **17,825 / 18,628 pages** (**95.69%**) as directly
  representable from the image. The remaining **803 / 18,628 pages**
  (**4.31%**) use a clean copied-page path: 29 whole small-file pages and 774
  partial tail pages.
- A simple UFS-on-md first-touch comparison counts up to all **18,628** logical
  pages as newly materialized. Cache state and pager behavior can reduce the
  actual work.
- No kernel implementation was exercised for these figures. "Directly
  representable" describes eligibility under the format and `mmap()` contract,
  not observed zero-copy execution or timing.

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
mappings. The resident-bank implementation replaces the historical md hooks:
bootloader images are adopted without copying; `femtoctl add` copies files or
devices into named RAM banks. Mount by label, unmount to retain, and explicitly
discard unused banks. No md/GEOM/kernel patches or direct device mounts are part
of this design. Mounting banks requires root. Private writable mappings use COW;
shared mappings remain read-only. Stock FreeBSD 15.1 dynamic-shell root and
UFS-control verification passes; commands and coverage limits are described in
the [module documentation](ko/README.md). Asynchronous imports remain deferred.

Image construction is offline work. Its efficiency matters, but deterministic
output, safe publication, mounted lookup performance, and final image size have
higher priority than build speed.

The input archive or directory tree must remain unchanged for the entire image
construction. `makefemtofs` detects some concurrent changes and fails when it
does, but it does not provide snapshot semantics or complete TOCTOU protection.

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
build/makefemtofs input-directory output.img
build/makefemtofs input.tar.zst output.img
```

Run `makefemtofs --help` for reproducible UUIDs, archive path stripping,
synthetic parent directories, verification modes, and guarded FreeBSD device
output.

## Pitch In One Sentence

`FemtoFS` is the filesystem for people who want immutable images to mount fast,
map safely, stay small, and never waste a boot on repair theater.
