// File: ko/femtofs.c
// Created by Andrea "Nemesi" Cocito on 14/10/2026
// FreeBSD 15.1 read-only VFS and shared-image mmap pager.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/stat.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vm_radix.h>
#include <vm/vnode_pager.h>
#include <security/mac/mac_framework.h>
#include <machine/atomic.h>
#include "bank.h"

#if __FreeBSD_version < 1501000 || __FreeBSD_version >= 1600000
#error "This module requires FreeBSD 15.1 or later within FreeBSD 15.x"
#endif

static MALLOC_DEFINE(M_FEMTOFS, "femtofs", "femtofs mount and vnode data");
static u_long ff_direct_faults, ff_clean_pages;
static u_long ff_clean_bytes, ff_loader_faults;
static int ff_pager_type = -1;
SYSCTL_NODE(_vfs, OID_AUTO, femtofs, CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "femtofs pager");
SYSCTL_ULONG(_vfs_femtofs, OID_AUTO, direct_faults, CTLFLAG_RD, &ff_direct_faults, 0,
    "Faults resolved by aliasing immutable image pages");
SYSCTL_ULONG(_vfs_femtofs, OID_AUTO, clean_pages, CTLFLAG_RD, &ff_clean_pages, 0,
    "Clean pages populated since module load");
SYSCTL_ULONG(_vfs_femtofs, OID_AUTO, clean_bytes, CTLFLAG_RD, &ff_clean_bytes, 0,
    "Live wired bytes allocated for clean mapping pages");
SYSCTL_ULONG(_vfs_femtofs, OID_AUTO, loader_faults, CTLFLAG_RD, &ff_loader_faults, 0,
    "Direct faults resolved from original preloaded image pages");
struct ff_mount {
    struct ff_bank *bank;
    struct sx cache_lock;
    vm_page_t **clean; /* Wired managed pages; one cache per stored blob. */
    vm_object_t *objects;
    int leaking, draining;
};
struct ff_vnode { struct vnode *vp; struct ff_mount *fs; uint32_t id; };
struct ff_mapping { struct vnode *vp; uint32_t id; };
#define FN(vp) ((struct ff_vnode *)(vp)->v_data)
#define NODE(vp) (&FN(vp)->fs->bank->image.nodes[FN(vp)->id])
static struct vop_vector ff_vops, ff_fifoops;
static struct fileops ff_fileops;
static int ff_vget(struct mount *, ino_t, int, struct vnode **);
static ino_t inode(const struct ff_image *im, uint32_t id)
{ return id == im->count ? 1 : id + 2; }

static int ff_access(struct vop_access_args *ap)
{
    struct ff_node *n = NODE(ap->a_vp);
    if ((ap->a_accmode & VWRITE) && ap->a_vp->v_type != VFIFO) return EROFS;
    return vaccess(ap->a_vp->v_type, n->mode, n->uid, n->gid,
        ap->a_accmode, ap->a_cred);
}
static int ff_getattr(struct vop_getattr_args *ap)
{
    struct vnode *vp = ap->a_vp;
    struct ff_node *n = NODE(vp);
    struct vattr *a = ap->a_vap;
    VATTR_NULL(a);
    a->va_type = vp->v_type; a->va_mode = n->mode & ALLPERMS;
    a->va_uid = n->uid; a->va_gid = n->gid; a->va_nlink = n->links;
    a->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
    a->va_fileid = inode(&FN(vp)->fs->bank->image, FN(vp)->id);
    a->va_size = n->type == FF_DIR ? n->entries : n->size;
    a->va_blocksize = PAGE_SIZE; a->va_bytes = roundup2(a->va_size, 512);
    a->va_atime = a->va_mtime = a->va_ctime = a->va_birthtime = (struct timespec){0, 0};
    a->va_gen = 1; a->va_flags = 0; a->va_rdev = NODEV; a->va_filerev = 0;
    return 0;
}
static int ff_pathconf(struct vop_pathconf_args *ap)
{
    switch (ap->a_name) {
    case _PC_LINK_MAX: *ap->a_retval = 32767; return 0;
    case _PC_NAME_MAX: *ap->a_retval = 255; return 0;
    case _PC_FILESIZEBITS: *ap->a_retval = 33; return 0;
    case _PC_SYMLINK_MAX: *ap->a_retval = MAXPATHLEN; return 0;
    case _PC_NO_TRUNC:
    case _PC_CHOWN_RESTRICTED: *ap->a_retval = 1; return 0;
    case _PC_PIPE_BUF:
        if (ap->a_vp->v_type != VDIR && ap->a_vp->v_type != VFIFO) return EINVAL;
        *ap->a_retval = PIPE_BUF; return 0;
    default: return vop_stdpathconf(ap);
    }
}
static int ff_open(struct vop_open_args *ap)
{
    if (ap->a_mode & FWRITE) return EROFS;
    if (ap->a_vp->v_type == VREG) {
        vnode_create_vobject(ap->a_vp, NODE(ap->a_vp)->size, ap->a_td);
        if (ap->a_fp) finit_vnode(ap->a_fp, ap->a_mode, NULL, &ff_fileops);
    }
    return 0;
}
static int ff_read(struct vop_read_args *ap)
{
    struct ff_node *n = NODE(ap->a_vp);
    struct uio *u = ap->a_uio;
    size_t count;
    if (ap->a_vp->v_type != VREG) return EISDIR;
    if (u->uio_offset < 0) return EINVAL;
    if ((uint64_t)u->uio_offset >= n->size) return 0;
    count = MIN(MIN((uint64_t)u->uio_resid, INT_MAX), n->size - (uint64_t)u->uio_offset);
    return uiomove(__DECONST(uint8_t *, FN(ap->a_vp)->fs->bank->image.bytes) + n->off + u->uio_offset, count, u);
}
static int ff_readlink(struct vop_readlink_args *ap)
{
    struct ff_node *n = NODE(ap->a_vp);
    return uiomove(__DECONST(uint8_t *, FN(ap->a_vp)->fs->bank->image.bytes) + n->off,
        MIN(MIN((uint64_t)ap->a_uio->uio_resid, INT_MAX), n->size), ap->a_uio);
}
static int ff_parent(struct mount *mp, void *arg, int flags, struct vnode **vpp)
{ return ff_vget(mp, *(ino_t *)arg, flags, vpp); }
static int ff_lookup_vop(struct vop_cachedlookup_args *ap)
{
    struct vnode *vp = ap->a_dvp;
    struct componentname *cn = ap->a_cnp;
    struct ff_image *im = &FN(vp)->fs->bank->image;
    uint32_t id;
    ino_t ino;
    int error;
    *ap->a_vpp = NULL;
    if ((cn->cn_nameiop == DELETE || cn->cn_nameiop == RENAME) &&
        (cn->cn_flags & ISLASTCN)) return EROFS;
    if (cn->cn_namelen == 1 && cn->cn_nameptr[0] == '.') {
        vref(vp); *ap->a_vpp = vp; return 0;
    }
    if (cn->cn_flags & ISDOTDOT) {
        ino = inode(im, NODE(vp)->parent);
        return vn_vget_ino_gen(vp, ff_parent, &ino, cn->cn_lkflags, ap->a_vpp);
    }
    error = ff_lookup(im, FN(vp)->id, cn->cn_nameptr, cn->cn_namelen, &id);
    if (error == ENOENT && cn->cn_nameiop == CREATE &&
        (cn->cn_flags & ISLASTCN)) return EROFS;
    if (!error) error = ff_vget(vp->v_mount, inode(im, id), cn->cn_lkflags, ap->a_vpp);
    if ((cn->cn_flags & MAKEENTRY) && (error == 0 || error == ENOENT)) cache_enter(vp, *ap->a_vpp, cn);
    return error;
}
static int ff_readdir(struct vop_readdir_args *ap)
{
    struct vnode *vp = ap->a_vp;
    struct ff_image *im = &FN(vp)->fs->bank->image;
    struct ff_node *n = NODE(vp);
    struct uio *u = ap->a_uio;
    struct dirent de;
    uint64_t pos;
    u_long *cookies = NULL;
    int count = 0, capacity = 0, error = 0;
    if (u->uio_offset < 0 || (uint64_t)u->uio_offset > n->size + 2) return EINVAL;
    if (ap->a_ncookies) {
        capacity = MIN(u->uio_resid / (offsetof(struct dirent, d_name) + 1) + 1, n->size + 2);
        cookies = malloc((size_t)capacity * sizeof(*cookies), M_TEMP, M_WAITOK);
        *ap->a_ncookies = 0; *ap->a_cookies = cookies;
    }
    pos = u->uio_offset;
    while (pos < n->size + 2) {
        const char *name;
        uint32_t id;
        memset(&de, 0, sizeof(de));
        if (pos < 2) { id = pos ? n->parent : FN(vp)->id; name = pos ? ".." : "."; }
        else {
            const uint8_t *b = im->bytes + 128 + (n->off + pos - 2) * 16;
            if (!b[0] || b[0] == FF_ATTR) { pos++; continue; }
            id = im->nodes[ff_u32(b + 4)].canonical;
            name = (const char *)im->bytes + ff_u32(b + 8);
        }
        de.d_fileno = inode(im, id);
        de.d_type = IFTODT(im->nodes[id].mode);
        de.d_namlen = strlen(name);
        memcpy(de.d_name, name, de.d_namlen + 1);
        de.d_reclen = GENERIC_DIRSIZ(&de);
        de.d_off = pos + 1;
        if (u->uio_resid < de.d_reclen || (cookies && count == capacity)) break;
        error = uiomove(&de, de.d_reclen, u);
        if (error) break;
        pos++;
        if (cookies) cookies[count++] = pos;
    }
    u->uio_offset = pos;
    if (ap->a_ncookies) *ap->a_ncookies = count;
    if (ap->a_eofflag) *ap->a_eofflag = pos == n->size + 2;
    return error;
}
static int ff_reclaim(struct vop_reclaim_args *ap)
{
    struct vnode *vp = ap->a_vp;
    vnode_destroy_vobject(vp);
    vfs_hash_remove(vp);
    free(vp->v_data, M_FEMTOFS); vp->v_data = NULL;
    return 0;
}
static int ff_setattr(struct vop_setattr_args *ap) { (void)ap; return EROFS; }
static int ff_bmap(struct vop_bmap_args *ap)
{
    /* haspage consumes these even when selecting the VOP_READ fallback. */
    if (ap->a_runp) *ap->a_runp = 0;
    if (ap->a_runb) *ap->a_runb = 0;
    return EOPNOTSUPP;
}
static int ff_getpages_async(struct vop_getpages_async_args *ap)
{
    /* Resident image reads complete synchronously, including sendfile's callback. */
    int error = VOP_GETPAGES(ap->a_vp, ap->a_m, ap->a_count, ap->a_rbehind, ap->a_rahead);
    if (ap->a_iodone) ap->a_iodone(ap->a_arg, ap->a_m, ap->a_count,
        error == VM_PAGER_OK ? 0 : EIO);
    return error;
}

/* The bank wire reference survives transfers between the two managed objects. */
static void ff_return_page(struct ff_mount *fs, uint32_t id, vm_pindex_t offset, vm_page_t page)
{
    vm_object_t bank = fs->bank->object;
    struct ff_node *n = &fs->bank->image.nodes[id];
    vm_pindex_t index = OFF_TO_IDX(n->off) + offset;
    if (fs->clean[n->blob] && fs->clean[n->blob][offset] == page) return;
    KASSERT(index < fs->bank->owned && fs->bank->pages[index] == page,
        ("FemtoFS page does not belong to its image range"));
    VM_OBJECT_WLOCK(bank);
    while (vm_page_insert(page, bank, index)) {
        VM_OBJECT_WUNLOCK(bank);
        vm_radix_wait();
        VM_OBJECT_WLOCK(bank);
    }
    VM_OBJECT_WUNLOCK(bank);
}

static void ff_pager_destroy(vm_object_t object)
{
    struct ff_mapping *mapping = object->handle;
    struct ff_mount *fs = FN(mapping->vp)->fs;
    uint32_t blob = fs->bank->image.nodes[mapping->id].blob;
    vm_page_t page;
    /* All paths take cache_lock before object: never wait for it with object held. */
    VM_OBJECT_WUNLOCK(object);
    sx_xlock(&fs->cache_lock);
    VM_OBJECT_WLOCK(object);
    while ((page = vm_radix_lookup_ge(&object->rtree, 0)) != NULL) {
        vm_pindex_t index = page->pindex;
        vm_page_busy_acquire(page, 0);
        pmap_remove_all(page);
        vm_page_remove_xbusy(page);
        ff_return_page(fs, mapping->id, index, page);
        vm_page_xunbusy(page);
    }
    fs->objects[blob] = NULL;
    object->type = OBJT_DEAD;
    VM_OBJECT_WUNLOCK(object);
    sx_xunlock(&fs->cache_lock);
    vrele(mapping->vp);
    free(mapping, M_FEMTOFS);
    VM_OBJECT_WLOCK(object);
}

static boolean_t ff_pager_haspage(vm_object_t object, vm_pindex_t index, int *before, int *after)
{
    if (before) *before = MIN(index, 4);
    if (after) *after = index < object->size ? MIN(object->size - index - 1, 4) : 0;
    return index < object->size;
}

/* A longer-lived managed cache must not retain public spill for new MAC checks. */
static void ff_mac_clean(struct ff_mount *fs)
{
    sx_assert(&fs->cache_lock, SA_XLOCKED);
    if (!fs->leaking || (!mac_vnode_check_access_enabled() &&
        !mac_vnode_check_lookup_enabled() && !mac_vnode_check_open_enabled() &&
        !mac_vnode_check_mmap_enabled())) return;
    fs->leaking = 0; /* Conservatively retain clean behavior until unmount. */
    for (uint32_t i = 0; i < fs->bank->image.nblobs; i++) {
        size_t size = fs->bank->image.blobs[i].size, tail = size % PAGE_SIZE;
        vm_page_t page;
        vm_object_t object;
        void *address;
        if (!tail || !fs->clean[i] || !(page = fs->clean[i][size / PAGE_SIZE])) continue;
        object = page->object;
        if (object) VM_OBJECT_WLOCK(object);
        vm_page_busy_acquire(page, 0);
        address = (void *)pmap_quick_enter_page(page);
        memset((char *)address + tail, 0, PAGE_SIZE - tail);
        pmap_quick_remove_page((vm_offset_t)address);
        vm_page_xunbusy(page);
        if (object) VM_OBJECT_WUNLOCK(object);
    }
}

static int ff_pager_getpages(vm_object_t object, vm_page_t *pages, int count,
    int *behind, int *ahead)
{
    struct ff_mapping *mapping = object->handle;
    struct ff_mount *fs = FN(mapping->vp)->fs;
    struct ff_node *n = &fs->bank->image.nodes[mapping->id];
    vm_pindex_t first = pages[0]->pindex;
    uint32_t source, copied;
    int error = VM_PAGER_OK;
    if (behind) *behind = 0;
    if (ahead) *ahead = 0;
    sx_xlock(&fs->cache_lock);
    ff_mac_clean(fs);
    VM_OBJECT_WLOCK(object);
    for (int i = 0; i < count; i++) {
        vm_pindex_t index = first + i;
        vm_page_t page;
        int leaking = fs->leaking && !mac_vnode_check_access_enabled() &&
            !mac_vnode_check_lookup_enabled() && !mac_vnode_check_open_enabled() &&
            !mac_vnode_check_mmap_enabled();
        if (ff_page(&fs->bank->image, mapping->id, IDX_TO_OFF(index), PAGE_SIZE,
            leaking, &source, &copied)) { error = VM_PAGER_BAD; break; }
        /* A partial public page may also back another blob: keep one managed owner. */
        if (!copied && IDX_TO_OFF(index + 1) > n->size) copied = PAGE_SIZE;
        if (copied) {
            if (!fs->clean[n->blob]) fs->clean[n->blob] = malloc(
                (size_t)howmany(n->size, PAGE_SIZE) * sizeof(vm_page_t), M_FEMTOFS, M_WAITOK | M_ZERO);
            page = fs->clean[n->blob][index];
            if (!page) {
                void *address;
                page = vm_page_alloc_noobj(VM_ALLOC_WIRED | VM_ALLOC_WAITOK);
                page->oflags &= ~VPO_UNMANAGED;
                pmap_zero_page(page);
                address = (void *)pmap_quick_enter_page(page);
                memcpy(address, fs->bank->image.bytes + source, copied);
                pmap_quick_remove_page((vm_offset_t)address);
                fs->clean[n->blob][index] = page;
                atomic_add_long(&ff_clean_pages, 1);
                atomic_add_long(&ff_clean_bytes, PAGE_SIZE);
            }
            vm_page_busy_acquire(page, 0);
        } else {
            vm_object_t bank = fs->bank->object;
            VM_OBJECT_WLOCK(bank);
            page = vm_page_lookup(bank, source / PAGE_SIZE);
            KASSERT(page != NULL, ("FemtoFS image page has another owner"));
            vm_page_busy_acquire(page, 0);
            vm_page_remove_xbusy(page);
            VM_OBJECT_WUNLOCK(bank);
            atomic_add_long(&ff_direct_faults, 1);
            if (fs->bank->preload) atomic_add_long(&ff_loader_faults, 1);
            if (bootverbose && atomic_cmpset_int(&fs->bank->sampled, 0, 1))
                printf("femtofs: page %s offset %u physical %#jx\n", fs->bank->label,
                    source, (uintmax_t)VM_PAGE_TO_PHYS(page));
        }
        vm_page_replace(page, object, index, pages[i]);
        vm_page_valid(page);
        pages[i] = page;
    }
    VM_OBJECT_WUNLOCK(object);
    sx_xunlock(&fs->cache_lock);
    return error;
}

static struct pagerops ff_pager_ops = {
    .pgo_dealloc = ff_pager_destroy, .pgo_getpages = ff_pager_getpages,
    .pgo_haspage = ff_pager_haspage
};

static vm_object_t ff_mapping_object(struct vnode *vp)
{
    struct ff_mount *fs = FN(vp)->fs;
    struct ff_node *n = NODE(vp);
    vm_object_t object;
    sx_xlock(&fs->cache_lock);
    if (fs->draining) { sx_xunlock(&fs->cache_lock); return NULL; }
    ff_mac_clean(fs);
    object = fs->objects[n->blob];
    if (object) {
        VM_OBJECT_WLOCK(object);
        vm_object_reference_locked(object);
        VM_OBJECT_WUNLOCK(object);
    } else {
        struct ff_mapping *mapping = malloc(sizeof(*mapping), M_FEMTOFS, M_WAITOK);
        mapping->vp = vp; mapping->id = FN(vp)->id; vref(vp);
        object = vm_object_allocate(OBJT_PHYS, howmany(n->size, PAGE_SIZE));
        VM_OBJECT_WLOCK(object);
        object->type = ff_pager_type;
        vm_object_clear_flag(object, OBJ_UNMANAGED);
        vm_object_set_flag(object, OBJ_PG_DTOR);
        object->handle = mapping;
        /* The mount retains the managed file cache across process exits. */
        vm_object_reference_locked(object);
        VM_OBJECT_WUNLOCK(object);
        fs->objects[n->blob] = object;
    }
    sx_xunlock(&fs->cache_lock);
    return object;
}

static int ff_mmap(struct file *fp, vm_map_t map, vm_offset_t *addr, vm_size_t size,
    vm_prot_t prot, vm_prot_t capmax, int flags, vm_ooffset_t off, struct thread *td)
{
    struct vnode *vp = fp->f_vnode;
    vm_object_t obj;
    vm_prot_t maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
    int error;
    /* A stacking filesystem leaves its vnode in f_vnode after forwarding
     * VOP_OPEN.  Let its ordinary vnode object handle that view. */
    if (vp->v_op != &ff_vops)
        return vnops.fo_mmap(fp, map, addr, size, prot, capmax, flags, off, td);
    /* vm_mmap_object adds MAP_COPY_ON_WRITE for private mappings, including
     * initially read-only mappings that later gain write permission. */
    if (!(flags & MAP_SHARED)) {
        maxprot |= VM_PROT_WRITE;
        capmax |= VM_PROT_WRITE; /* As in vn_mmap: private writes need only FREAD. */
    }
    if (!(fp->f_flag & FREAD)) return EACCES;
    if (off > OFF_MAX || size > OFF_MAX - off || (off & PAGE_MASK)) return EINVAL;
    if ((error = vget(vp, LK_SHARED)) != 0) return error;
    if (vp->v_mount->mnt_flag & MNT_NOEXEC) maxprot &= ~VM_PROT_EXECUTE;
    maxprot &= capmax;
    if (prot & ~maxprot) { vput(vp); return EACCES; }
    error = mac_vnode_check_mmap(td->td_ucred, vp, prot, flags);
    if (error) { vput(vp); return error; }
    obj = ff_mapping_object(vp);
    vput(vp);
    if (!obj) return EBUSY;
    error = vm_mmap_object(map, addr, size, prot, maxprot, flags, obj, off, FALSE, td);
    if (error) vm_object_deallocate(obj);
    return error;
}

static int ff_vget(struct mount *mp, ino_t ino, int flags, struct vnode **vpp)
{
    struct ff_mount *fs = mp->mnt_data;
    struct vnode *vp;
    struct ff_vnode *vn;
    uint32_t id;
    int error;
    *vpp = NULL;
    if (ino != 1 && (ino < 2 || ino - 2 >= fs->bank->image.count)) return ESTALE;
    id = ino == 1 ? fs->bank->image.count : ino - 2;
    if (!fs->bank->image.nodes[id].type || fs->bank->image.nodes[id].type == FF_LINK) return ESTALE;
    error = vfs_hash_get(mp, ino, flags, curthread, vpp, NULL, NULL);
    if (error || *vpp) return error;
    error = getnewvnode("femtofs", mp, &ff_vops, &vp);
    if (error) return error;
    vn = malloc(sizeof(*vn), M_FEMTOFS, M_WAITOK | M_ZERO);
    *vn = (struct ff_vnode){vp, fs, id}; vp->v_data = vn;
    vp->v_type = IFTOVT(fs->bank->image.nodes[id].mode);
    if (vp->v_type == VFIFO) vp->v_op = &ff_fifoops;
    if (ino == 1) vp->v_vflag |= VV_ROOT;
    lockmgr(vp->v_vnlock, LK_EXCLUSIVE, NULL);
    if (vp->v_type != VFIFO) VN_LOCK_ASHARE(vp);
    error = insmntque(vp, mp);
    if (error) { free(vn, M_FEMTOFS); return error; }
    vn_set_state(vp, VSTATE_CONSTRUCTED);
    error = vfs_hash_insert(vp, ino, flags, curthread, vpp, NULL, NULL);
    if (error || *vpp) return error;
    *vpp = vp;
    return 0;
}
static int ff_root(struct mount *mp, int flags, struct vnode **vpp)
{ return ff_vget(mp, 1, flags, vpp); }
static int ff_statfs(struct mount *mp, struct statfs *s)
{
    struct ff_mount *fs = mp->mnt_data;
    s->f_bsize = FF_PAGE; s->f_iosize = PAGE_SIZE;
    s->f_blocks = fs->bank->image.size / FF_PAGE;
    s->f_bfree = s->f_bavail = s->f_ffree = 0;
    s->f_files = fs->bank->image.objects + 1;
    s->f_namemax = 255;
    return 0;
}
static void ff_free(struct ff_mount *fs)
{
    uint32_t i, j;
    if (fs->clean) {
        for (i = 0; i < fs->bank->image.nblobs; i++) if (fs->clean[i]) {
            for (j = 0; j < howmany(fs->bank->image.blobs[i].size, PAGE_SIZE); j++)
                if (fs->clean[i][j]) {
                    vm_page_t page = fs->clean[i][j];
                    KASSERT(page->object == NULL, ("FemtoFS clean page still mapped"));
                    vm_page_unwire_noq(page);
                    vm_page_free(page);
                    atomic_subtract_long(&ff_clean_bytes, PAGE_SIZE);
                }
            free(fs->clean[i], M_FEMTOFS);
        }
        free(fs->clean, M_FEMTOFS);
    }
    free(fs->objects, M_FEMTOFS);
    ff_bank_release(fs->bank);
    sx_destroy(&fs->cache_lock);
    free(fs, M_FEMTOFS);
}
static int ff_mount(struct mount *mp)
{
    static const char *opts[] = {"from", "bank", "clean", "leaking", NULL};
    struct ff_mount *fs;
    struct ff_bank *bank;
    char *from, *label;
    int error;
    /* Banks expose privileged loader memory, even when usermount is enabled. */
    if ((error = priv_check(curthread, PRIV_DRIVER))) {
        vfs_mount_error(mp, "administrator privilege required");
        return error;
    }
    if (vfs_filteropt(mp->mnt_optnew, opts)) {
        vfs_mount_error(mp, "unsupported mount option");
        return EINVAL;
    }
    if (vfs_flagopt(mp->mnt_optnew, "clean", NULL, 0) &&
        vfs_flagopt(mp->mnt_optnew, "leaking", NULL, 0)) {
        vfs_mount_error(mp, "clean and leaking are mutually exclusive");
        return EINVAL;
    }
    if (mp->mnt_flag & MNT_UPDATE) {
        if (vfs_flagopt(mp->mnt_optnew, "ro", NULL, 0)) return 0;
        vfs_mount_error(mp, "FemtoFS cannot become writable");
        return EROFS;
    }
    from = vfs_getopts(mp->mnt_optnew, "from", &error);
    if (error) { vfs_mount_error(mp, "missing mount source"); return error; }
    label = vfs_getopts(mp->mnt_optnew, "bank", &error);
    /* Generic mount(8) supplies the label as from; root boot needs empty from
     * to avoid the kernel's device wait and supplies a separate bank option. */
    if (*from) {
        if (error != ENOENT) {
            vfs_mount_error(mp, "conflicting source and bank");
            return EINVAL;
        }
        label = from; error = 0;
    }
    if (error) { vfs_mount_error(mp, "missing bank label"); return error; }
    if ((error = ff_bank_claim(label, &bank))) {
        vfs_mount_error(mp, "cannot claim bank %s", label);
        return error;
    }
    fs = malloc(sizeof(*fs), M_FEMTOFS, M_WAITOK | M_ZERO);
    sx_init(&fs->cache_lock, "femtofs clean pages");
    fs->bank = bank;
    fs->leaking = PAGE_SIZE <= FF_PAGE && vfs_flagopt(mp->mnt_optnew, "leaking", NULL, 0);
    /* Mode-bit visibility cannot authorize spill under an active MAC policy. */
    if (mac_vnode_check_access_enabled() || mac_vnode_check_lookup_enabled() ||
        mac_vnode_check_open_enabled() || mac_vnode_check_mmap_enabled()) fs->leaking = 0;
    fs->clean = malloc((size_t)bank->image.nblobs * sizeof(*fs->clean), M_FEMTOFS, M_WAITOK | M_ZERO);
    fs->objects = malloc((size_t)bank->image.nblobs * sizeof(*fs->objects), M_FEMTOFS, M_WAITOK | M_ZERO);
    mp->mnt_data = fs;
    MNT_ILOCK(mp);
    mp->mnt_flag |= MNT_RDONLY | MNT_LOCAL;
    mp->mnt_kern_flag |= MNTK_LOOKUP_SHARED | MNTK_EXTENDED_SHARED;
    MNT_IUNLOCK(mp);
    vfs_getnewfsid(mp); vfs_mountedfrom(mp, label); ff_statfs(mp, &mp->mnt_stat);
    if (bootverbose) printf("femtofs: bank %s %s %u bytes at %p\n", label,
        bank->preload ? "preload" : "imported", bank->image.size, bank->image.bytes);
    return 0;
}
static int ff_unmount(struct mount *mp, int flags)
{
    struct ff_mount *fs = mp->mnt_data;
    int error;
    /* Forced reclaim could leave mapped pager handles pointing at dead vnodes. */
    if (flags & MNT_FORCE) return EOPNOTSUPP;
    sx_xlock(&fs->cache_lock);
    for (uint32_t i = 0; i < fs->bank->image.nblobs; i++) if (fs->objects[i]) {
        VM_OBJECT_RLOCK(fs->objects[i]);
        int active = fs->objects[i]->ref_count != 1;
        VM_OBJECT_RUNLOCK(fs->objects[i]);
        if (active) { sx_xunlock(&fs->cache_lock); return EBUSY; }
    }
    fs->draining = 1;
    sx_xunlock(&fs->cache_lock);
    for (uint32_t i = 0; i < fs->bank->image.nblobs; i++)
        if (fs->objects[i]) vm_object_deallocate(fs->objects[i]);
    error = vflush(mp, 0, 0, curthread);
    if (error) {
        sx_xlock(&fs->cache_lock); fs->draining = 0; sx_xunlock(&fs->cache_lock);
        return error;
    }
    ff_free(mp->mnt_data); mp->mnt_data = NULL;
    return 0;
}
static int ff_init(struct vfsconf *vfc)
{
    int error;
    (void)vfc;
    ff_pager_type = vm_pager_alloc_dyn_type(&ff_pager_ops, OBJT_PHYS);
    if (ff_pager_type < 0) return ENOSPC;
    ff_fileops = vnops; ff_fileops.fo_mmap = ff_mmap;
    error = ff_banks_init();
    if (error) vm_pager_free_dyn_type(ff_pager_type);
    return error;
}
static int ff_uninit(struct vfsconf *vfc)
{
    int error;
    (void)vfc;
    error = ff_banks_uninit();
    if (!error) vm_pager_free_dyn_type(ff_pager_type);
    return error;
}
static struct vfsops ff_vfsops = {
    .vfs_init = ff_init, .vfs_uninit = ff_uninit, .vfs_mount = ff_mount, .vfs_unmount = ff_unmount,
    .vfs_root = ff_root, .vfs_statfs = ff_statfs, .vfs_vget = ff_vget
};
VFS_SET(ff_vfsops, femtofs, VFCF_READONLY);
MODULE_VERSION(femtofs, 1);
static struct vop_vector ff_vops = {
    .vop_default = &default_vnodeops,
    .vop_access = ff_access, .vop_getattr = ff_getattr, .vop_open = ff_open,
    .vop_lookup = vfs_cache_lookup, .vop_cachedlookup = ff_lookup_vop,
    .vop_read = ff_read, .vop_readlink = ff_readlink, .vop_readdir = ff_readdir,
    .vop_reclaim = ff_reclaim, .vop_inactive = VOP_NULL,
    .vop_getpages = vnode_pager_local_getpages,
    .vop_getpages_async = ff_getpages_async,
    .vop_bmap = ff_bmap,
    .vop_setattr = ff_setattr, .vop_pathconf = ff_pathconf,
    .vop_advise = VOP_NULL, /* The immutable image is already resident. */
};
VFS_VOP_VECTOR_REGISTER(ff_vops);
static struct vop_vector ff_fifoops = {
    .vop_default = &fifo_specops, .vop_access = ff_access, .vop_getattr = ff_getattr,
    .vop_reclaim = ff_reclaim, .vop_inactive = VOP_NULL, .vop_setattr = ff_setattr,
};
VFS_VOP_VECTOR_REGISTER(ff_fifoops);
// END File: ko/femtofs.c
