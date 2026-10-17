// File: ko/bank.c
// Created by Andrea "Nemesi" Cocito on 17/10/2026
// Resident image ownership, loader adoption and privileged control operations.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/capsicum.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/file.h>
#include <sys/kenv.h>
#include <sys/kernel.h>
#include <sys/linker.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/pmap.h>
#include <machine/atomic.h>
#include "bank.h"

static MALLOC_DEFINE(M_FFBANK, "femtofs banks", "resident FemtoFS banks");
static struct sx bank_lock;
static struct ff_bank *banks;
static struct cdev *control;
static unsigned active;
static int stopping;
static uint64_t generation;
static u_long borrowed_bytes, snapshot_bytes, metadata_bytes, released_pages;
SYSCTL_DECL(_vfs_femtofs);
SYSCTL_ULONG(_vfs_femtofs, OID_AUTO, borrowed_bytes, CTLFLAG_RD, &borrowed_bytes, 0,
    "Logical image bytes in loader banks, including unmounted banks");
SYSCTL_ULONG(_vfs_femtofs, OID_AUTO, snapshot_bytes, CTLFLAG_RD, &snapshot_bytes, 0,
    "Wired allocation bytes in imported banks, including unmounted banks");
SYSCTL_ULONG(_vfs_femtofs, OID_AUTO, metadata_bytes, CTLFLAG_RD, &metadata_bytes, 0,
    "Decoded bank node/blob allocation bytes");
SYSCTL_ULONG(_vfs_femtofs, OID_AUTO, released_pages, CTLFLAG_RD, &released_pages, 0,
    "Complete preload pages passed to bootstrap reclamation since load");

static int valid_label(const char *s)
{
    size_t n = strnlen(s, FF_LABEL_SIZE);
    if (!n || n == FF_LABEL_SIZE) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
            (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-')) return 0;
    return 1;
}
static u_long metadata_size(const struct ff_bank *b)
{
    return ((size_t)b->image.count + 1) * sizeof(struct ff_node) +
        (size_t)b->image.count * 2 * sizeof(struct ff_blob);
}
static struct ff_bank *find_bank(const char *label)
{
    sx_assert(&bank_lock, SA_XLOCKED);
    for (struct ff_bank *b = banks; b; b = b->next)
        if (!strcmp(b->label, label)) return b;
    return NULL;
}
static int publish(struct ff_bank *b)
{
    int error = 0;
    sx_xlock(&bank_lock);
    if (find_bank(b->label)) error = EEXIST;
    else {
        b->next = banks; banks = b; generation++;
        metadata_bytes += metadata_size(b);
        if (b->preload) borrowed_bytes += b->image.size;
        else snapshot_bytes += b->extent;
    }
    sx_xunlock(&bank_lock);
    return error;
}
int ff_bank_claim(const char *label, struct ff_bank **result)
{
    struct ff_bank *b;
    int error = 0;
    if (!valid_label(label)) return EINVAL;
    sx_xlock(&bank_lock);
    b = find_bank(label);
    if (stopping) error = ENXIO;
    else if (!b) error = ENOENT;
    else if (b->claimed) error = EBUSY;
    else { b->claimed = 1; generation++; *result = b; }
    sx_xunlock(&bank_lock);
    return error;
}
void ff_bank_release(struct ff_bank *b)
{
    sx_xlock(&bank_lock);
    KASSERT(b->claimed, ("unclaimed FemtoFS bank"));
    b->claimed = 0; generation++;
    sx_xunlock(&bank_lock);
}

static int read_image(struct file *fp, void *buffer, size_t size, off_t off,
    struct thread *td)
{
    while (size) {
        struct iovec iov = {buffer, MIN(size, 65536)};
        struct uio uio = {.uio_iov = &iov, .uio_iovcnt = 1, .uio_offset = off,
            .uio_resid = iov.iov_len, .uio_segflg = UIO_SYSSPACE,
            .uio_rw = UIO_READ, .uio_td = td};
        size_t count = iov.iov_len;
        int error = fo_read(fp, &uio, td->td_ucred, FOF_OFFSET, td);
        if (error) return error;
        count -= uio.uio_resid;
        if (!count) return EIO;
        buffer = (char *)buffer + count; size -= count; off += count;
    }
    return 0;
}
static int import(struct ff_bank_request *r, struct thread *td)
{
    struct file *fp;
    struct stat st;
    struct ff_bank *b;
    cap_rights_t rights;
    off_t capacity;
    u_int sector = 1;
    uint8_t *header;
    int error;
    error = fget_read(td, r->fd, cap_rights_init(&rights, CAP_READ, CAP_SEEK, CAP_FSTAT, CAP_IOCTL), &fp);
    if (error) return error;
    error = fo_stat(fp, &st, td->td_ucred);
    if (error) goto close;
    if (!(fp->f_ops->fo_flags & DFLAG_SEEKABLE) ||
        (!S_ISREG(st.st_mode) && !S_ISCHR(st.st_mode))) { error = EOPNOTSUPP; goto close; }
    capacity = st.st_size;
    if (S_ISCHR(st.st_mode)) {
        if (fo_ioctl(fp, DIOCGMEDIASIZE, &capacity, td->td_ucred, td)) capacity = UINT32_MAX;
        if (fo_ioctl(fp, DIOCGSECTORSIZE, &sector, td->td_ucred, td)) sector = 1;
    }
    if (!sector || FF_PAGE % sector || capacity < FF_PAGE) { error = EINVAL; goto close; }
    b = malloc(sizeof(*b), M_FFBANK, M_WAITOK | M_ZERO);
    strlcpy(b->label, r->label, sizeof(b->label));
    strlcpy(b->source, r->source, sizeof(b->source));
    header = malloc(FF_PAGE, M_FFBANK, M_WAITOK);
    error = read_image(fp, header, FF_PAGE, 0, td);
    if (!error) error = ff_header(header, capacity, &b->image);
    free(header, M_FFBANK);
    if (error) goto fail;
    b->extent = round_page(b->image.size);
    b->image.bytes = kmem_malloc(b->extent, M_NOWAIT | M_ZERO);
    if (!b->image.bytes) { error = ENOMEM; goto fail; }
    error = read_image(fp, __DECONST(uint8_t *, b->image.bytes), b->image.size, 0, td);
    if (!error) error = ff_validate(&b->image);
    if (!error) error = publish(b);
    if (!error) goto close;
fail:
    ff_destroy(&b->image);
    if (b->image.bytes) kmem_free(__DECONST(uint8_t *, b->image.bytes), b->extent);
    free(b, M_FFBANK);
close:
    fdrop(fp, td);
    return error;
}
static int bank_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int flags,
    struct thread *td)
{
    struct ff_bank_request *r = (void *)data;
    struct ff_bank *b, **link;
    int error;
    (void)dev; (void)flags;
    if ((error = priv_check(td, PRIV_DRIVER))) return error;
    if (cmd != FF_BANK_ADD && cmd != FF_BANK_LIST && cmd != FF_BANK_DISCARD) return ENOTTY;
    if (r->version != FF_BANK_VERSION) return EINVAL;
    if (cmd != FF_BANK_LIST && !valid_label(r->label)) return EINVAL;
    if (cmd == FF_BANK_ADD && !memchr(r->source, 0, sizeof(r->source))) return EINVAL;
    sx_xlock(&bank_lock);
    if (stopping) { sx_xunlock(&bank_lock); return ENXIO; }
    active++;
    error = 0;
    /* kldunload also deletes preload records. Do not wait on the linker while
     * holding our lock: module uninit acquires it to check active operations. */
    if (cmd == FF_BANK_DISCARD) {
        sx_xunlock(&bank_lock);
        error = linker_kldload_busy(LINKER_UB_UNLOCK | LINKER_UB_PCATCH);
        sx_xlock(&bank_lock);
        if (error) {
            active--;
            sx_xunlock(&bank_lock);
            return error;
        }
    }
    if (cmd == FF_BANK_LIST) {
        if (r->index && r->generation != generation) error = EAGAIN;
        else {
            b = banks;
            for (uint32_t i = 0; b && i < r->index; i++) b = b->next;
            if (!b) error = ENOENT;
            else {
                r->generation = generation;
                r->size = b->image.size; r->extent = b->extent;
                r->preload = b->preload; r->claimed = b->claimed;
                memcpy(r->uuid, b->image.bytes + 16, 16);
                strlcpy(r->label, b->label, sizeof(r->label));
                strlcpy(r->source, b->source, sizeof(r->source));
            }
        }
    } else if (cmd == FF_BANK_DISCARD) {
        for (link = &banks; *link && strcmp((*link)->label, r->label); link = &(*link)->next) {}
        b = *link;
        if (!b) error = ENOENT;
        else if (b->claimed) error = EBUSY;
        else {
            *link = b->next; generation++;
            metadata_bytes -= metadata_size(b);
            if (b->preload) borrowed_bytes -= b->image.size;
            else snapshot_bytes -= b->extent;
            /* Serialize preload metadata deletion as well as bank removal. */
            ff_destroy(&b->image);
            if (b->preload) {
                preload_delete_name(b->source);
                atomic_add_long(&released_pages, b->extent / PAGE_SIZE);
                if (bootverbose) printf("femtofs: discarded preload %s %zu pages\n", b->label, b->extent / PAGE_SIZE);
            } else kmem_free(__DECONST(uint8_t *, b->image.bytes), b->extent);
            free(b, M_FFBANK);
        }
    } else {
        if (find_bank(r->label)) error = EEXIST;
        else {
            sx_xunlock(&bank_lock);
            error = import(r, td);
            sx_xlock(&bank_lock);
        }
    }
    sx_xunlock(&bank_lock);
    if (cmd == FF_BANK_DISCARD) linker_kldload_unbusy(0);
    sx_xlock(&bank_lock);
    active--;
    sx_xunlock(&bank_lock);
    return error;
}
static struct cdevsw bank_cdevsw = {
    .d_version = D_VERSION, .d_name = "femtofs", .d_ioctl = bank_ioctl
};

static int adopt(const char *label, const char *name)
{
    caddr_t md = NULL, p = NULL;
    struct ff_bank *b;
    const char *type, *other;
    const uint8_t *base;
    size_t extent;
    int error;
    if (!valid_label(label) || strlen(name) >= sizeof(b->source)) return EINVAL;
    while ((p = preload_search_next_name(p))) {
        other = preload_search_info(p, MODINFO_NAME);
        if (other && !strcmp(other, name)) {
            if (md) return EINVAL;
            md = p;
        }
    }
    if (!md) return ENOENT;
    type = preload_search_info(md, MODINFO_TYPE);
    if (!type || strcmp(type, "femtofs_image")) return EINVAL;
    base = preload_fetch_addr(md); extent = preload_fetch_size(md);
    if (!base || !extent || extent > UINT32_MAX || ((uintptr_t)base & PAGE_MASK) ||
        (extent & PAGE_MASK) || extent > UINTPTR_MAX - (uintptr_t)base) return EINVAL;
    /* Deletion releases whole pages; reject overlap with any other preload. */
    p = NULL;
    while ((p = preload_search_next_name(p))) {
        uintptr_t addr = (uintptr_t)preload_fetch_addr(p);
        size_t len = preload_fetch_size(p);
        if (p != md && addr && len &&
            (addr > UINTPTR_MAX - len || ((uintptr_t)base < addr + len && addr < (uintptr_t)base + extent))) return EINVAL;
    }
    for (b = banks; b; b = b->next)
        if (b->image.bytes == base) return EEXIST;
    b = malloc(sizeof(*b), M_FFBANK, M_WAITOK | M_ZERO);
    b->preload = 1; b->extent = extent;
    strlcpy(b->label, label, sizeof(b->label)); strlcpy(b->source, name, sizeof(b->source));
    error = ff_header(base, extent, &b->image);
    if (!error) { b->image.bytes = base; error = ff_validate(&b->image); }
    if (!error) error = publish(b);
    if (error) { ff_destroy(&b->image); free(b, M_FFBANK); }
    else if (bootverbose) printf("femtofs: preload %s <%s> %zu bytes at %p\n", label, name, extent, base);
    return error;
}
int ff_banks_init(void)
{
    const char prefix[] = "femtofs.bank.";
    char *config, *out, *entry, *eq;
    size_t size = 1;
    int error;
    if (!dynamic_kenv) return ENXIO;
    sx_init(&bank_lock, "femtofs banks");
    /* Snapshot only our environment entries while kenv pointers are protected. */
    mtx_lock(&kenv_lock);
    for (int i = 0; kenvp[i]; i++)
        if (!strncmp(kenvp[i], prefix, sizeof(prefix)-1)) size += strlen(kenvp[i]) + 1;
    config = malloc(size, M_FFBANK, M_NOWAIT | M_ZERO);
    if (config) {
        out = config;
        for (int i = 0; kenvp[i]; i++) if (!strncmp(kenvp[i], prefix, sizeof(prefix)-1)) {
            size = strlen(kenvp[i]) + 1; memcpy(out, kenvp[i], size); out += size;
        }
    }
    mtx_unlock(&kenv_lock);
    if (!config) { sx_destroy(&bank_lock); return ENOMEM; }
    for (entry = config; *entry; entry += strlen(entry) + 1) {
        eq = strchr(entry, '=');
        if (!eq) continue;
        *eq = 0;
        error = adopt(entry + sizeof(prefix)-1, eq+1);
        if (error) printf("femtofs: cannot register %s: error %d\n", entry, error);
        *eq = '=';
    }
    free(config, M_FFBANK);
    control = make_dev(&bank_cdevsw, 0, UID_ROOT, GID_WHEEL, 0600, "femtofs");
    return 0;
}
int ff_banks_uninit(void)
{
    sx_xlock(&bank_lock);
    if (banks || active) { sx_xunlock(&bank_lock); return EBUSY; }
    stopping = 1;
    sx_xunlock(&bank_lock);
    destroy_dev(control);
    sx_destroy(&bank_lock);
    return 0;
}
// END File: ko/bank.c
