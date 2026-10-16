# FreeBSD module

The build guard accepts FreeBSD 15.1 and later within 15.x; runtime verification
currently covers 15.1. FreeBSD 14.x and `dirty` mode are deferred.
The on-disk contract is [specification.md](../specification.md).
The [kernel design](design.md) specifies the required single-copy boot-image
backing and the md interface implemented in the author's FreeBSD fork. Mounts
borrow preloaded image pages; EOPNOTSUPP automatically selects the original
snapshot path for file-backed md and other unsupported providers.

Build the portable decoder and run its regressions locally, from the repository root:

```sh
make -f ko/Makefile.host
python3 ko/test-image.py tmp/notes-audit-build/makefemtofs tmp/ko-host/check-image tmp/ko-host
```

Commit and push locally before pulling and building on FreeBSD. Native builds use
the patched kernel sources and BSD make; keep output under `tmp/`. The running
kernel must provide MD_PRELOAD_API_VERSION=1 (the fork's md hooks); an unpatched
kernel cannot load this module:

```sh
mkdir -p tmp/ko-native
env MAKEOBJDIR="$PWD/tmp/ko-native" make -C ko SYSDIR="$PWD/tmp/freebsd-src/sys"
env MAKEOBJDIR="$PWD/tmp/ko-native" make -C ko -f Makefile.mount
sudo kldload "$PWD/tmp/ko-native/femtofs.ko"
sudo tmp/ko-native/mount_femtofs -o clean /dev/mdN /existing/mountpoint
```

Use only an explicitly allocated test provider, and unmount it before detaching
that provider or unloading the module. No installation into `/boot/kernel` or
changes to boot configuration are needed for testing.

For a preloaded md, the mount borrows and validates the original loader allocation,
with no image-sized copy. Its md hold prevents writes and forced detach until
unmount. Only EOPNOTSUPP selects the existing wired snapshot fallback; other
borrowing errors and invalid borrowed images fail the mount. The fallback costs
one image-sized allocation; allocation failure returns `ENOMEM`.
Metadata is decoded once. Lookup uses the
image's hash chains and the VFS namecache. Hardlinks retain canonical inode
identity; directories synthesize `.` and `..`; FIFOs use FreeBSD's FIFO operations.
Times are zero because this format does not store timestamps.

Aligned mmap pages directly alias the selected backing's physical pages. Partial clean
tails and unaligned file pages are copied lazily, zero-filled, and cached by blob
offset and page index. Deduplicated files share that cache while retaining their
distinct inode identity. `leaking` exposes only validated public-part pages; private
payloads use clean behavior. Larger VM pages force clean behavior. The standard
vnode pager remains available for kernel consumers such as executable loading and
sendfile. `fadvise` is accepted as a hint; the image is already resident.
Active MAC access/lookup/open/mmap policies also force clean behavior at mount.
`vfs.femtofs.direct_faults` and `vfs.femtofs.clean_pages` expose cumulative pager
counters for checking direct-page use and copied-page cache sharing.
`borrowed_bytes`, `snapshot_bytes` and `clean_bytes` under the same sysctl node
report live backing/copy usage; `borrowed_faults` counts direct faults from
original preloaded pages. With bootverbose, each mount logs its backing address
and ownership so the boot regression can compare it with the loader's md address.

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

The root-boot regression reuses the boot artifacts from the fork's
`tools/test/md_preload/run.sh` regression. Run it as an ordinary user on toolbox
with a new output directory:

```sh
python3 ko/test-boot.py tmp/md-guest-4 tmp/ko-build/makefemtofs \
  tmp/freebsd-src tmp/femtofs-boot-test
```

It builds the module and static probes, boots a FemtoFS root through the loader,
checks a second preloaded image in both modes, and runs the file-backed matrix.
It compares mount backing addresses against md's original loader addresses and
checks live snapshot/clean-copy accounting, failed-mount cleanup and busy-provider
rejection. QEMU uses TCG with no guest network and preserves its console log.
The root init and probes are static; Python, the builder and their shared libraries
live on a separate UFS test volume. This does not establish a fully dynamic FemtoFS
root: writable private mmap remains rejected by the current contract.

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
