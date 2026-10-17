# FemtoFS resident-bank design

Implementation target: stock FreeBSD 15.1 amd64, without md or GEOM patches.
The verification summary and remaining limits are in `ko/README.md`; superseded
md designs remain available in Git history.

A named bank owns one fully resident image and its validated decoded metadata.
Loader images of type `femtofs_image` reuse their original allocation without
copying. `femtoctl add LABEL PATH` synchronously imports a regular file or readable
character device into wired RAM. Only labels are mount sources; device paths are
import sources. No automatic device-mount fallback remains.

`femtofs.bank.LABEL=EXACT_LOADER_NAME` assigns labels through the kernel environment.
Root uses `vfs.root.mountfrom="femtofs:"` and options `ro,clean,bank=LABEL`.
Registration precedes root mount. Labels are case-sensitive, 1–63 ASCII letters,
digits, underscores or hyphens. Original preload names must be unambiguous across
all types, since deletion is by name. Invalid registrations preserve loader memory.

`mount_femtofs LABEL DIRECTORY` requires `PRIV_DRIVER`, matching bank control,
regardless of `vfs.usermount`. It claims a bank; only one independent mount is
allowed. Unmount releases mount state and clean copies, retaining the bank.
`femtoctl discard LABEL` refuses claimed banks and otherwise frees decoded
metadata and backing. Preloads use stock `preload_delete_name`; imported buffers
use their matching allocation release. Module unload refuses while banks or
control operations exist. Explicit discard precedes unload.

The module-owned `/dev/femtofs` provides privileged add/list/discard ioctls, with
no image data mapping interface. The importer uses a referenced read-only fd,
validates a completed private copy before publication and closes its source.
The displayed import source is the path string supplied by `femtoctl`; the passed
descriptor is authoritative. A source modified during import is not an atomic
snapshot, so point-in-time consistency requires a quiescent source. All current
backing stays resident and has a contiguous kernel virtual view; physical
contiguity is not required. Asynchronous VM-object imports are deferred.

Decoded metadata belongs to the bank. Clean/leaking policy, vnodes and the clean
page cache belong to the mount. Pager references retain the mount claim until
aliases disappear. A clean tail page is reused after an earlier process exits;
unmount frees the cache. Bank byte accounting persists until discard.

Private mappings permit writable protection and later write upgrades through
FreeBSD's VM copy-on-write path. The backing pager remains read-only: writes go
to anonymous shadow pages, preserving the bank and shared clean-tail cache.
Shared mappings remain read-only. Stock init must boot a normal dynamic shell
with its runtime linker and shared libraries in the bank in acceptance tests.

Each user mapping has an immutable, non-anonymous OBJT_SWAP view backed by the
bank pager. The view holds no copied file pages; reads fault through to original
physical pages. It keeps OBJT_DEVICE out of the user map, avoiding amd64's
device-superpage prefault path, which can install writable entries without
applying read-only/COW protection. Private writes get a separate anonymous shadow
with normal VM reservation and accounting.

Vnodes and lookup operations use shared locks. The lockless namecache fast path
is not enabled because it also requires SMR-safe `v_data` reclamation. Clean-page
cache locking occurs only on faults; optimizing hits with a second lock mode would
add synchronization complexity to a path whose pages are then mapped directly.

`mount -u -o ro` is an accepted no-op and a request to become writable returns
EROFS. Forced unmount remains unsupported because live pager handles retain vnode
state. Shutdown may therefore report error 45 before syncing and powering off.

Leaking mode is permitted only while no active MAC vnode hook can override the
mode-bit visibility decision. The check runs at mount and at every new pager
fault, so loading a policy later forces subsequent faults through clean pages.
Already established mappings follow normal VM lifetime rules and are not revoked.

Counters distinguish loader/imported logical image bytes from retained allocation
bytes. Loader fault/release counters name their origin explicitly; clean-page
and decoded-metadata accounting remain separate from image storage and VM COW.
The decoder records its own persistent allocation size, avoiding a copied formula
in the bank layer.

Delivery uses local build and portable tests, then commit/push, toolbox pull,
native compilation, and disposable stock/debug-kernel QEMU tests. The host kernel
is never replaced. Dirty mode, forced unmount, NFS export, FreeBSD 14.x and
alternate native architectures remain outside scope. Exec/sendfile can use the
ordinary vnode cache; direct mmap page reuse does not imply that every file
consumer avoids copies.
