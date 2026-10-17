# FemtoFS resident-bank design

Implementation target: stock FreeBSD 15.1 amd64, without md or GEOM patches.
This replaces the historical md-hook design below. Implementation/verification
is in progress; see `tmp/NODEV-execution.md` for observed results.

A named bank owns one fully resident image and its validated decoded metadata.
Loader images of type `femtofs_image` reuse their original allocation without
copying. `femtoctl add LABEL PATH` synchronously imports a regular file or readable
character device into wired RAM. Only labels are mount sources; device paths
are import sources. No automatic device-mount fallback remains.

`femtofs.bank.LABEL=EXACT_LOADER_NAME` assigns labels through the kernel environment.
Root uses `vfs.root.mountfrom="femtofs:"` and options `ro,clean,bank=LABEL`.
Registration precedes root mount. Labels are case-sensitive, 1–63 ASCII letters,
digits, underscores or hyphens. Original preload names must be unambiguous across
all types, since deletion is by name. Invalid registrations preserve loader memory.

`mount_femtofs LABEL DIRECTORY` claims a bank; only one independent mount is
allowed. Unmount releases mount state and clean copies, retaining the bank.
`femtoctl discard LABEL` refuses claimed banks and otherwise frees decoded
metadata and backing. Preloads use stock `preload_delete_name`; imported buffers
use their matching allocation release. Module unload refuses while banks or
control operations exist. Explicit discard precedes unload.

The module-owned `/dev/femtofs` provides privileged add/list/discard ioctls, with
no image data mapping interface. The importer uses a referenced read-only fd,
validates a completed private copy before publication and closes its source.
A source modified during import is not an atomic snapshot; use a quiescent source.
All current backing stays resident and has a contiguous kernel virtual view;
physical contiguity is not required. Asynchronous VM-object imports are deferred
in `tmp/ASYNCH.md`, with no advance pager framework in this implementation.

Decoded metadata belongs to the bank. Clean/leaking policy, vnodes and the clean
page cache belong to the mount. Pager references retain the mount claim until
aliases disappear. A clean tail page is reused after an earlier process exits;
unmount frees the cache. Bank byte accounting persists until discard.

Delivery sequence: update instructions/design, compile and test locally, commit
and push, pull on toolbox, native build and disposable stock-kernel QEMU tests.
Do not replace the host kernel. Acceptance includes root boot, file/device
imports, retained remount, failure/race cleanup and measured preload reclamation.
The detailed checklist is `tmp/NODEV-plan.md`. Private `fsck/` code is not accessed.

Dirty mode, private writable/COW mappings, fully dynamic root, forced unmount,
NFS export, FreeBSD 14.x and alternate native architectures remain outside scope.
Exec/sendfile can still use the ordinary vnode cache; direct mmap page reuse does
not imply all file consumers avoid copies.

## Historical patched-md design and evidence (superseded)

The remainder records the previous implementation only. Its interface, build
requirements and acceptance results do not describe or verify resident banks.


## Contract

The primary target is FreeBSD 15.1 amd64 booting from loader-preloaded md images.
The build version guard permits 15.1 through the remainder of 15.x; that is not
runtime verification of every release. The filesystem implements clean and
leaking mappings. Dirty mappings and 14.x compatibility remain deferred.

For each boot image, the loader allocation is the only full backing copy.
Mount-time validation, reads and eligible direct mmap faults use those original
pages. Mounting must not allocate or populate a second image-sized buffer, even
temporarily. Multiple images are selected by their actual md provider, never by
matching contents or guessing loader order from md unit numbers.

Decoded metadata, vnode state, clean-mode boundary/unaligned page copies, and
process-private/COW memory are additional overhead. This requirement does not
claim that every executable or sendfile operation avoids the vnode page cache.
Those consumers currently use the standard vnode pager and must be measured
separately from image backing.

## Original baseline and the required interface

The initial module opened the GEOM provider, reads its header, allocates
round_page(image_size) bytes with kmem_malloc, copies the complete image with
bread/memcpy, and validates that copy. Its direct mmap pager aliases pages of
that new allocation. ff_free releases the allocation at unmount.

FreeBSD 15.1's sys/dev/md/md.c already retains the original loader pointer in
MD_PRELOAD state. mdstart_preload copies from that pointer into BIO_READ buffers.
Neither its existing GEOM attributes nor its public ioctl interface expose a
safe kernel borrowing operation. Its private md_s layout must not be duplicated
in FemtoFS. Preload metadata alone cannot establish the association with an
arbitrary current md provider or prevent changes through that provider.

The selected design requires a small FreeBSD md extension. On 15 October 2026,
the author authorized implementing and committing it to the puffetto/freebsd-src
fork, checked out under tmp/. Work targets releng/15.1 on a separate fork branch.
The alternative separate rmd driver is not selected. The host kernel and boot
configuration are not replaced for testing.

## md borrowing interface

The extension is implemented in the author's [FreeBSD fork branch
femtofs/md-preload-15.1](https://github.com/puffetto/freebsd-src/tree/femtofs/md-preload-15.1).
The kernel entry points are md_preload_hold and md_preload_release, declared in
sys/mdioctl.h under _KERNEL, with MD_PRELOAD_API_VERSION set to 1. The production
patch adds 70 lines across md.c and mdioctl.h. The standalone boot regression is
under tools/test/md_preload in that fork. Existing FreeBSD copyright and license
notices are retained.

Add kernel-only hold/release operations, declared in a kernel header, implemented
inside md.c where the provider and backing ownership are known. The hold accepts
the filesystem's attached GEOM consumer and returns the original backing address
and usable length for that exact MD_PRELOAD provider. It performs no allocation
or image read. Non-preloaded providers return EOPNOTSUPP.

The caller holds the GEOM topology lock. A successful hold requires readable,
exclusive, non-writing consumer access and no provider writers. An md-owned
borrow count prevents write access and detach, including forced detach, until
the corresponding release. Detach checks and hold/release use the same lock;
the rejection must occur before orphaning the provider. Existing behavior for
unborrowed md devices remains unchanged. Preloaded-device resizing is already
unsupported. Exposing raw kernel addresses through a user-readable GEOM
attribute or ioctl is explicitly excluded.

Release drops the borrow count; it never frees the loader allocation. The normal
consumer access remains held until after release. The implementation must avoid
inverting md_sx and the GEOM topology lock. The API needs an explicit version
marker so source builds can identify support without assuming all 15.x kernels
provide this project-specific extension.

## Filesystem changes

Acquire protected access before obtaining any borrowed pointer. Check that the
image base is VM-page aligned and the backing extent covers the declared image.
Validate the header and complete image directly in place. Reject unsupported
alignment or a missing borrowing interface; do not silently copy a boot image.
The image remains protected from validation through the last mapping reference.

Record backing ownership explicitly in ff_mount. Borrowed storage is released
through md; only filesystem-owned snapshot storage can reach kmem_free. Every
failure path unwinds exactly the resources it acquired. The existing pager's
vnode references continue to prevent unmount while mappings survive closed file
descriptors. Non-root unmount/remount must preserve the original backing bytes.

The author clarified on 16 October 2026 that EOPNOTSUPP from md_preload_hold
automatically selects the existing full-copy snapshot path. No opt-in option is
needed for file-backed md or other unsupported providers. Other errors abort the
mount; invalid borrowed images or alignment never trigger copying. The module
requires the patched md API at build/load time, so missing hooks are not silently
treated as an unsupported provider. Tests must distinguish borrowed backing from
the fallback rather than accepting snapshot tests as zero-copy boot evidence.

Keep the existing decoder, lookup, access controls, and clean/leaking page policy.
Direct faults use physical pages corresponding to the borrowed image address.
Clean copies remain cached per stored blob and relative page, shared across
deduplicated references. Additional counters distinguish borrowed image bytes,
owned snapshot bytes, live clean-copy bytes and direct faults. Boot diagnostics
must establish the provider-to-backing association and actual page identity;
zero-valued allocation counters alone are insufficient evidence.

## Verification and delivery order

1. Clarify implementation instructions, then this design, then code. Approval
   for the md interface extension was granted on 15 October 2026.
2. Use local compilation, portable decoder regressions and static checks for
   every code cycle. Commit and push before pulling on toolbox.
3. Compile against FreeBSD 15.1 on toolbox. Keep kernel-source modifications and
   custom kernel artifacts in a disposable checkout/build area; never install
   them into the toolbox host's boot tree.
4. Run the existing mounted regression matrix on the automatically selected snapshot
   path, including malformed-image rejection and failure cleanup.
5. Boot a disposable VM through the FreeBSD loader with the module and two
   distinct md_image payloads. Mount FemtoFS as root and run a test init from it.
   Mount the second image and verify distinct contents and backing identity.
6. Exercise clean/leaking mappings, executable loading, directory/metadata and
   permission behavior. Verify source physical-page identity for direct faults,
   no owned snapshot bytes, and separately accounted clean-copy memory. Compare
   different image sizes so an image-sized hidden allocation is observable.
7. Attempt provider writes, normal/forced detach and non-root unmount while
   mappings are live. They must not invalidate or change the backing. Release
   mappings, unmount/remount, and verify the same image remains intact. Include
   a malformed preloaded image to exercise failed-mount release.
8. Inspect guest logs and cleanup. Record commands, commit IDs and results in
   tmp/REVIEW-execution.md. Preserve explicit unverified limits; a successful
   ordinary mount is not proof of boot-root support.

## md verification status, 15 October 2026

Fork commit 86bd2308 passes local FreeBSD-target compilation of md and its test
consumer, shellcheck, and native GENERIC/module builds. The QEMU regression
prints MD_PRELOAD ALL TESTS PASSED and exits successfully. Two loader-preloaded
images of 64/128 KiB are borrowed at their exact original addresses. Nested holds,
write denial, ordinary/forced detach denial, module-unload denial and release
behavior pass; an unrelated malloc device remains writable and detachable.
The test checks identity through independent provider reads because loader module
order changed between observed boots. The running toolbox kernel is unchanged.

That regression verifies the md interface with a UFS root. The FemtoFS consumer
now uses the hold/release API and retains the old snapshot path exclusively for
EOPNOTSUPP. See the consumer boot regression below.

## FemtoFS verification status, 16 October 2026

Consumer commit df3b952 and boot-harness fixes through 69e56e7 pass local
FreeBSD-target compilation, 13 portable decoder regressions, and native module
compilation. The QEMU guest boots its static init from a 33,480,704-byte FemtoFS
root and mounts a distinct 61,440-byte preloaded image. Both backing addresses
match md's original loader addresses. Live snapshot bytes remain zero throughout
the borrowed-image tests; direct borrowed faults increase and clean-copy memory
returns to baseline after secondary-image unmount.

Both clean/leaking borrowed-image tests pass content, directory, hardlink,
symlink, FIFO, mmap protection/tail/lifetime/EOF and sendfile checks. Busy preload
rejection and two malformed preload failures leave accounting balanced and release
holds. Forced detach is refused while mounted; unmount/remount preserves contents.
The file-backed md fallback succeeds even with an existing writer, and the existing
clean/leaking matrix passes at 512/4096-byte sectors: 316 objects per combination,
48 mmap probes, static executable loading, permissions, concurrent lookup, empty
root and ten malformed-image rejections. Snapshot accounting returns to zero.

The host runner confirms FEMTOFS_BOOT ALL TESTS PASSED, guest status zero, matching
backing addresses, no panic and QEMU exit zero. Artifacts are retained under
`tmp/femtofs-boot-evidence/` locally and `tmp/femtofs-boot-3/` on toolbox.
The unprivileged fixture preserves non-root ownership and emits login.conf trust
warnings; fixture construction also reports the rescue shell missing from the
host shell list and discarded unrepresentable metadata. These are recorded rather
than claimed as a warning-free boot. Shutdown reports root unmount error 45
because forced unmount is unsupported; buffers sync and the VM powers off.
The module compiler emits no warnings.

The root init and probes are static. Python, the builder and shared libraries
execute from UFS: writable private mappings remain prohibited by FemtoFS's
current mmap contract. A fully dynamic root, arm64, other VM page sizes and other
15.x releases are not established by this test. Standard vnode-pager allocations
for executable loading/sendfile are separate from the single original image
backing; these tests do not claim that every I/O path is zero-copy.

## Existing limitations

Original unaligned mmap offsets are normalized by FreeBSD before filesystem
file operations receive them; rejection of the original offset remains a
separate contract issue. NFS export and forced filesystem unmount are unsupported.
Alternate VM page sizes, arm64 and active MAC-policy fallback require additional
native verification. This design does not silently resolve those separate items.

Generic FreeBSD mount(8) sends the label in `from` directly; the module accepts
that form without a `bank` option. The helper and root boot use empty `from` plus
`bank=LABEL`. Conflicting selectors and device paths are rejected.
