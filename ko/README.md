# FreeBSD resident-bank module

Target: stock FreeBSD 15.1 amd64. The build guard accepts 1501000–1599999; later
releases are not automatically verified. Dirty mode and 14.x are deferred.
See [design.md](design.md) and [specification.md](../specification.md).
Resident banks pass stock FreeBSD 15.1 root and UFS-control guest verification.
Verified results and remaining limits are summarized below; previous patched-md
results are historical and remain available in Git history.

## Build and use

Run portable checks locally, commit/push, then pull and build on FreeBSD:

```sh
make -f ko/Makefile.host
python3 ko/test-image.py build/makefemtofs tmp/ko-host/check-image tmp/ko-host
```

Native build against matching stock sources, with output kept under `tmp/`:

```sh
mkdir -p tmp/nodev-native
env MAKEOBJDIR="$PWD/tmp/nodev-native" make -C ko SYSDIR=/usr/src/sys
env MAKEOBJDIR="$PWD/tmp/nodev-native" make -C ko -f Makefile.mount
env MAKEOBJDIR="$PWD/tmp/nodev-native" make -C ko -f Makefile.ctl
sudo kldload "$PWD/tmp/nodev-native/femtofs.ko"
sudo tmp/nodev-native/femtoctl add data /path/to/data.0fs
sudo tmp/nodev-native/femtoctl list
sudo tmp/nodev-native/mount_femtofs -o clean data /existing/mountpoint
sudo umount /existing/mountpoint
sudo tmp/nodev-native/femtoctl discard data
sudo kldunload femtofs
```

`add` copies the complete image into wired RAM, validates and publishes it. The
source can be a regular file or readable, seekable character device, including
a GEOM disk. Devices must have compatible read alignment. Source references are
released after import; later changes/detach cannot affect the bank. Use a quiescent
source for a coherent snapshot during copying. Allocation failure returns ENOMEM.
Asynchronous imports are not implemented and are never silently copied.
The `SUPPLIED_SOURCE` list column is the path string supplied by `femtoctl`; the
descriptor passed to the kernel is authoritative and may refer to the same object
through a different path.

Labels contain 1–63 ASCII letters, digits, underscores or hyphens. Mounts accept
labels only; direct device mounts and the old fallback path are removed. The
helper does not resolve the label as a path. Normal helper/fstab dispatch uses:

```text
data /existing/mountpoint femtofs ro,clean 0 0
```

One independent mount may claim a bank. Unmount retains backing and decoded
metadata. Discard returns EBUSY while claimed; module unload requires all banks
to be discarded. Clean-page caches are per mount and freed on unmount.
`/dev/femtofs` is an administrator-only ioctl endpoint, not an image device.
Mounts also require `PRIV_DRIVER` (root), even with `vfs.usermount=1`. An
unprivileged caller cannot claim a loader bank by knowing its label.

## Loader banks and root boot

Loader allocations are reused without a second full-image copy. Use the distinct
`femtofs_image` type so stock md does not expose a writable frontend:

```text
load /boot/kernel/femtofs.ko
load -t femtofs_image /boot/root.0fs
load -t femtofs_image /boot/data.0fs
set femtofs.bank.root=/boot/root.0fs
set femtofs.bank.data=/boot/data.0fs
set vfs.root.mountfrom="femtofs:"
set vfs.root.mountfrom.options="ro,clean,bank=root"
boot
```

Stock rc defaults `root_rw_mount` to `YES` and aborts startup when its
`mount -uw /` fails. A FemtoFS root must set `root_rw_mount="NO"` in
`/etc/rc.conf`.

The environment maps labels to exact loader-recorded names, consumed on module
initialization. Names must be unique across preload types and allocations cannot
be registered under multiple labels. Extents must be VM-page aligned at both ends
and cover a valid image. Failed registrations log errors and preserve preload
memory; unregistered invalid records remain reserved until reboot.

Discard releases a loader bank through stock preload deletion, including its
metadata record. amd64 bootstrap-page release is the tested target. A discarded
bank cannot be remounted unless populated again. No md/GEOM/kernel patch is needed.

## Mapping behavior and accounting

Eligible mmap pages alias bank physical pages. Partial clean tails and unaligned
file pages are copied lazily, zero-filled and shared by stored blob/page within
the mount. The cache persists after a process exits. `leaking` exposes only public
spill; private payloads use clean behavior. Larger VM pages and active MAC policies
force clean behavior. `fadvise` is a hint; current backing is already resident.

Counters under `vfs.femtofs` distinguish retained storage from logical contents:

| Counter | Meaning |
| --- | --- |
| `loader_bytes`, `imported_bytes` | Retained allocation bytes by bank origin |
| `loader_image_bytes`, `imported_image_bytes` | Logical image bytes by bank origin |
| `metadata_bytes` | Decoded node/blob allocation bytes |
| `clean_bytes` | Live mount clean-page allocations |
| `clean_pages` | Clean pages populated since module load |
| `direct_faults`, `loader_faults` | Pager faults aliasing bank pages, all origins / loader only |
| `loader_released_pages` | Complete preload pages passed to stock reclamation since module load |

Bank byte counters include unmounted banks. `femtoctl list` reports each bank's
logical size and allocation extent. The old `borrowed_bytes`, `snapshot_bytes`,
`borrowed_faults` and `released_pages` names are removed. VM-managed private COW
and ordinary vnode-cache pages are separate from these module allocation counters.

Private writable mappings and later private write upgrades use FreeBSD VM COW:
reads retain eligible direct bank pages, writes populate anonymous process-private
pages. An immutable VM view separates user mappings from the device pager to
avoid amd64's writable device-superpage prefault optimization. The view contains
no copied file pages. They cannot alter the bank, the shared clean cache or another process's
mapping. Shared writable mmap and shared write upgrades remain rejected. Pager
vnode references keep mounts/banks alive after fd close. Busy/forced unmounts are
refused. Exec/sendfile can use the ordinary vnode cache; direct mmap does not make
all consumers zero-copy. Clean/leaking tail rules apply to private mappings too.

Validation includes structural checks, metadata hash, terminators, overlaps,
visibility and reachability. Optional whole-image hash and global canonical-byte
checks remain omitted. Unknown compatible metadata extensions are ignored.
Original unaligned mmap offsets are normalized by FreeBSD before filesystem entry;
rejection of the original offset remains an unresolved contract limitation.

## Disposable verification

Use a stock boot tree and matching source tree. No host reboot or boot installation:

```sh
python3 ko/test-boot.py /boot build/makefemtofs /usr/src tmp/nodev-root
python3 ko/test-boot.py /boot build/makefemtofs /usr/src tmp/nodev-control control
python3 ko/test-boot.py /boot build/makefemtofs /usr/src tmp/nodev-reverse reverse
python3 ko/test-boot.py /boot build/makefemtofs /usr/src tmp/nodev-audit audit
env FEMTOFS_TEST_KERNBUILDDIR=/usr/obj/usr/src/amd64.amd64/sys/FEMTOFS_DEBUG \
    python3 ko/test-boot.py /boot build/makefemtofs /usr/src tmp/nodev-debug-control control
env FEMTOFS_TEST_KERNBUILDDIR=/usr/obj/usr/src/amd64.amd64/sys/FEMTOFS_DEBUG \
    python3 ko/test-boot.py /boot build/makefemtofs /usr/src tmp/nodev-debug-root-smoke root-smoke
```

The harness builds module/static tools, boots QEMU without networking, and keeps
console logs. Stock init and probes are static; the normal dynamic `/bin/sh`,
`echo`, `cat`, runtime linker and base libraries are stored in FemtoFS. The shell
runs before the auxiliary UFS runtime is mounted. Python/builder live on UFS.
The control guest exercises final reclamation and module unload/reload. Tests
cover loader banks, synchronous imports, races, retained remount, clean-tail reuse,
filesystem operations, and the converted 512/4096-sector import matrix. Only
resources created by each run are cleaned up. Audit mode runs the aligned shared
mapping security probe and first-touch timing from a UFS root, so it can also be
used with a historical module that cannot host the current dynamic userspace.
Private `fsck/` tests are not used.

Native arm64/alternate VM pages, active MAC policy execution, NFS export and forced
unmount remain outside current native acceptance.

Generic FreeBSD mount(8) sends the label in `from` directly; the module accepts
that form without a `bank` option. The helper and root boot use empty `from` plus
`bank=LABEL`. Conflicting selectors and device paths are rejected.

## Verification recorded 17 October 2026

The current implementation passes local FreeBSD-target compilation with
`-Werror`, 13 portable decoder tests, native builds and stock-kernel QEMU root,
UFS-control and reversed-loader runs.
The root run passes 80 mmap probes, including private writes, write upgrades,
fork isolation, descriptor lifetime and EOF; two additional aligned 2 MiB tests
cover private COW and shared protection, including MADV_WILLNEED. It also passes
all four 387-object clean/leaking and 512/4096-sector matrix combinations,
10 malformed imports, tail reuse, nullfs, import/discard races and control checks.
Source devices are detached before mounting imported banks.

The normal dynamic shell, echo, cat, runtime linker and base libraries run from
FemtoFS before the auxiliary runtime is mounted. Both unprivileged bank-mount
forms return EPERM with vfs.usermount=1; root can subsequently mount the same bank.
Private writes preserve read() contents and do not bypass noexec.
Cold `mlock`, `msync(MS_INVALIDATE)` and debugger writes are also covered:
debugger writes to read-only shared mappings are refused, while writes to private
mappings use COW and leave the bank unchanged.

The aligned shared-map probe reproduces a security defect in `09ac10a`: an amd64
device-superpage prefault installed a writable mapping despite `PROT_READ`, and
the attempted write changed the loader bank. The immutable view introduced in
`c1d971c` makes the same write trap and preserves the bank. This matters on a
FemtoFS root because modifying bank bytes can change data later read or executed
by privileged processes. A 4 MiB first-touch QEMU/TCG microbenchmark measured
median times of 9.91 ms without advice and 10.39 ms with `MADV_WILLNEED` before
the fix, versus 16.07 ms and 15.94 ms after it. These numbers describe mapping
fault cost, not warm access after page-table entries exist.

The sampled data page matches the loader's physical address plus its image offset.
The data fixture contains 4,259,840 logical bytes in a 4,268,032-byte preload:
discard increases VM-managed pages by exactly 1,042 and removes its metadata.
Counters distinguish the 8,192 bytes of retained padding; imports retain only
logical image bytes on the tested 4 KiB VM-page target.

All three configurations pass the same 80 mmap and two aligned-page probes.
The control guest additionally discards the root bank (13,492 pages), unloads and
reloads the module, and verifies discarded banks stay absent. No panic, assertion
failure or Python traceback appears in these passing runs.

An INVARIANTS/WITNESS control guest initially found a missing
`VSTATE_CONSTRUCTED` transition when publishing a new vnode. `bba303c` adds that
transition. A later debug control run covers the complete lifecycle/import and
filesystem-operation suite with shared vnode locks and no vnode assertion,
kernel panic or WITNESS diagnostic. A separate debug `root-smoke` run covers
mountroot, a dynamic shell and its libraries from FemtoFS, and shutdown without
auxiliary mount crossings. Read-only mount updates, rejected writable and policy
changing updates, `statfs` and `pathconf` are part of the native matrix.

Not covered: allocator exhaustion, device read-error injection, malformed loader
address records, unload during a deliberately stalled import, active MAC policy
execution, other architectures/VM page sizes or other FreeBSD releases.
Asynchronous imports and dirty mode remain deferred. Root
shutdown still reports unsupported forced unmount (45), then syncs and powers
off. Expected fixture/CTF diagnostics remain in the logs.
