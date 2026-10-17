# FreeBSD resident-bank module

Target: stock FreeBSD 15.1 amd64. The build guard accepts 1501000–1599999; later
releases are not automatically verified. Dirty mode and 14.x are deferred.
See [design.md](design.md) and [specification.md](../specification.md).
Resident-bank implementation is undergoing native verification; results are in
`tmp/NODEV-execution.md`. Previous patched-md results are historical.

## Build and use

Run portable checks locally, commit/push, then pull and build on FreeBSD:

```sh
make -f ko/Makefile.host
python3 ko/test-image.py tmp/notes-audit-build/makefemtofs tmp/ko-host/check-image tmp/ko-host
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

`vfs.femtofs.borrowed_bytes` counts logical loader-bank bytes, `snapshot_bytes`
counts imported wired allocations, and `metadata_bytes` counts decoded nodes/blobs.
They include unmounted banks. `clean_bytes` counts mount clean copies; `clean_pages`,
`direct_faults` and `borrowed_faults` are cumulative. `released_pages` counts complete
preload pages passed to stock reclamation, not a global free-memory measurement.
`femtoctl list` distinguishes image size and retained allocation extent.

Writable mmap and later write upgrades are rejected. Pager vnode references keep
mounts/banks alive after fd close. Busy/forced unmounts are refused. Exec/sendfile
can use the ordinary vnode cache; the direct-mmap path does not make all consumers
zero-copy. Fully dynamic root/private writable COW remains outside this contract.

Validation includes structural checks, metadata hash, terminators, overlaps,
visibility and reachability. Optional whole-image hash and global canonical-byte
checks remain omitted. Unknown compatible metadata extensions are ignored.
Original unaligned mmap offsets are normalized by FreeBSD before filesystem entry;
rejection of the original offset remains an unresolved contract limitation.

## Disposable verification

Use a stock boot tree and matching source tree. No host reboot or boot installation:

```sh
python3 ko/test-boot.py /boot tmp/ko-build/makefemtofs /usr/src tmp/nodev-root
python3 ko/test-boot.py /boot tmp/ko-build/makefemtofs /usr/src tmp/nodev-control control
python3 ko/test-boot.py /boot tmp/ko-build/makefemtofs /usr/src tmp/nodev-reverse reverse
```

The harness builds module/static tools, boots QEMU without networking, and keeps
console logs. Root init/probes are static; Python/builder libraries run from UFS.
The control guest exercises final reclamation and module unload/reload. Tests
cover loader banks, synchronous imports, races, retained remount, clean-tail reuse,
filesystem operations, and the converted 512/4096-sector import matrix. Only
resources created by each run are cleaned up. Private `fsck/` tests are not used.

Native arm64/alternate VM pages, active MAC policy execution, NFS export and forced
unmount remain outside current native acceptance. Source/build checks alone do
not establish runtime behavior; consult the execution log for actual results.
