# FemtoFS kernel backing and boot design

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

## Current baseline and the missing interface

The initial module opens the GEOM provider, reads its header, allocates
round_page(image_size) bytes with kmem_malloc, copies the complete image with
bread/memcpy, and validates that copy. Its direct mmap pager aliases pages of
that new allocation. ff_free releases the allocation at unmount.

FreeBSD 15.1's sys/dev/md/md.c already retains the original loader pointer in
MD_PRELOAD state. mdstart_preload copies from that pointer into BIO_READ buffers.
Neither its existing GEOM attributes nor its public ioctl interface expose a
safe kernel borrowing operation. Its private md_s layout must not be duplicated
in FemtoFS. Preload metadata alone cannot establish the association with an
arbitrary current md provider or prevent changes through that provider.

Therefore the proposed design requires a small FreeBSD md extension. This is a
proposal pending approval of the kernel interface change, not an implemented
capability. The host kernel and boot configuration are not replaced for testing.

## Proposed md borrowing interface

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

For ordinary device testing, a full-copy snapshot path may remain available only
through an explicit mount option. The default boot path requires borrowed
backing. Tests must distinguish these paths rather than accidentally validating
the snapshot path as evidence of zero-copy boot support.

Keep the existing decoder, lookup, access controls, and clean/leaking page policy.
Direct faults use physical pages corresponding to the borrowed image address.
Clean copies remain cached per stored blob and relative page, shared across
deduplicated references. Additional counters distinguish borrowed image bytes,
owned snapshot bytes, live clean-copy bytes and direct faults. Boot diagnostics
must establish the provider-to-backing association and actual page identity;
zero-valued allocation counters alone are insufficient evidence.

## Verification and delivery order

1. Clarify implementation instructions, then this design, then code. Obtain
   approval for the specific md interface extension before implementing it.
2. Use local compilation, portable decoder regressions and static checks for
   every code cycle. Commit and push before pulling on toolbox.
3. Compile against FreeBSD 15.1 on toolbox. Keep kernel-source modifications and
   custom kernel artifacts in a disposable checkout/build area; never install
   them into the toolbox host's boot tree.
4. Run the existing mounted regression matrix on the explicitly selected snapshot
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

## Existing limitations

Original unaligned mmap offsets are normalized by FreeBSD before filesystem
file operations receive them; rejection of the original offset remains a
separate contract issue. NFS export and forced filesystem unmount are unsupported.
Alternate VM page sizes, arm64 and active MAC-policy fallback require additional
native verification. This design does not silently resolve those separate items.
