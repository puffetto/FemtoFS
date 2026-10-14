# FreeBSD module

The initial target is FreeBSD 15.1. FreeBSD 14.x and `dirty` mode are deferred.
The on-disk contract is [specification.md](../specification.md).

Build the portable decoder and run its regressions locally, from the repository root:

```sh
make -f ko/Makefile.host
python3 ko/test-image.py tmp/notes-audit-build/makefemtofs tmp/ko-host/check-image tmp/ko-host
```

Commit and push locally before pulling and building on FreeBSD. Native builds use
the installed kernel sources and BSD make; keep output under `tmp/`:

```sh
mkdir -p tmp/ko-native
env MAKEOBJDIR="$PWD/tmp/ko-native" make -C ko
env MAKEOBJDIR="$PWD/tmp/ko-native" make -C ko -f Makefile.mount
sudo kldload "$PWD/tmp/ko-native/femtofs.ko"
sudo tmp/ko-native/mount_femtofs -o clean /dev/mdN /existing/mountpoint
```

Use only an explicitly allocated test provider, and unmount it before detaching
that provider or unloading the module. No installation into `/boot/kernel` or
changes to boot configuration are needed for testing.

The mount takes an immutable, wired snapshot of the image from the GEOM provider.
This costs one image-sized allocation in addition to the provider's own storage;
allocation failure returns `ENOMEM`. Metadata is decoded once. Lookup uses the
image's hash chains and the VFS namecache. Hardlinks retain canonical inode
identity; directories synthesize `.` and `..`; FIFOs use FreeBSD's FIFO operations.
Times are zero because this format does not store timestamps.

Aligned mmap pages directly alias the snapshot's physical pages. Partial clean
tails and unaligned file pages are copied lazily, zero-filled, and cached by blob
offset and page index. Deduplicated files share that cache while retaining their
distinct inode identity. `leaking` exposes only validated public-part pages; private
payloads use clean behavior. Larger VM pages force clean behavior. The standard
vnode pager remains available for kernel consumers such as executable loading and
sendfile. `fadvise` is accepted as a hint; the snapshot is already resident.
Active MAC access/lookup/open/mmap policies also force clean behavior at mount.
`vfs.femtofs.direct_faults` and `vfs.femtofs.clean_pages` expose cumulative pager
counters for checking direct-page use and copied-page cache sharing.

The custom mmap entry point rejects writable mappings and prevents later write
permission upgrades. Pager references keep the vnode and mount alive after the
descriptor closes. Forced unmount is deliberately unsupported; normal unmount
returns busy while files or mappings retain references.

Validation includes all structural checks, metadata hash, terminators, interval
overlap, public/private visibility and own-anchor reachability. It omits the
optional image hash and the explicitly optional canonical-byte checks for global
padding and content-equality deduplication. Higher major-1 revisions are accepted
with unknown extensions ignored as required by the format.

One interface limitation remains under discussion: FreeBSD normalizes unaligned
mmap offsets before calling filesystem file operations. A standalone module sees
the normalized offset and cannot enforce the specification's rejection of the
original unaligned request. The original offset rule requires a kernel change or
an explicit scope exception; it is not claimed as implemented here.

Kernel compilation and mounted integration results are recorded in the local
`tmp/REVIEW-execution.md`; portable tests alone do not establish kernel correctness.

To run the disposable-provider mount matrix after loading the module:

```sh
cc -Wall -Wextra -Werror -g ko/test-mmap.c -o tmp/ko-native/test-mmap
cc -Wall -Wextra -Werror -g ko/test-directory.c -o tmp/ko-native/test-directory
sudo python3 ko/test-mount.py tmp/ko-build/makefemtofs \
  tmp/ko-native/mount_femtofs tmp/ko-native/test-mmap tmp
sudo kldunload femtofs
```

The harness verifies both mount modes at 512/4096-byte provider sectors, malformed
mount rejection, mounted-provider overwrite refusal, directory cookies, metadata,
unprivileged access, hardlinks, FIFO traffic, concurrent lookups, and mmap page
contents, read-only protections, closed-descriptor lifetime and EOF faults. Pager
counters check actual direct-page use and sharing of clean copies. All providers
and mounts belong to the test; cleanup failure retains backing files.

The decoder can also run under coverage-guided fuzzing:

```sh
cc -O1 -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
  ko/image.c ko/fuzz-image.c -o tmp/ko-native/fuzz-image
# Place valid images from makefemtofs in the corpus first.
mkdir -p tmp/ko-fuzz-images tmp/ko-fuzz-artifacts
tmp/ko-native/fuzz-image -seed=20261014 -max_len=1048576 -max_total_time=600 \
  -artifact_prefix=tmp/ko-fuzz-artifacts/ tmp/ko-fuzz-images
```

NFS export and forced unmount are not supported by this initial module. Native
runtime coverage currently targets amd64 with 4096-byte VM pages; alternate VM
page sizes have portable policy tests, not kernel execution evidence.
