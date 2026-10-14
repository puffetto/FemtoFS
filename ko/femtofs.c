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
#include <sys/vnode.h>
#include <geom/geom.h>
#include <geom/geom_vfs.h>
#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vnode_pager.h>
#include <security/mac/mac_framework.h>
#include <machine/atomic.h>
#include "image.h"

#if __FreeBSD_version < 1501000 || __FreeBSD_version >= 1502000
#error "This module targets FreeBSD 15.1"
#endif

static MALLOC_DEFINE(M_FEMTOFS, "femtofs", "femtofs mount and vnode data");
static u_long ff_direct_faults, ff_clean_pages;
SYSCTL_NODE(_vfs, OID_AUTO, femtofs, CTLFLAG_RD | CTLFLAG_MPSAFE, 0, "femtofs pager");
SYSCTL_ULONG(_vfs_femtofs, OID_AUTO, direct_faults, CTLFLAG_RD, &ff_direct_faults, 0,
    "Faults resolved by aliasing immutable image pages");
SYSCTL_ULONG(_vfs_femtofs, OID_AUTO, clean_pages, CTLFLAG_RD, &ff_clean_pages, 0,
    "Clean pages populated since module load");
struct ff_mount {
    struct ff_image image;
    struct vnode *device;
    struct g_consumer *consumer;
    struct sx cache_lock;
    vm_offset_t **clean; /* Per stored blob, indexed by file-relative page. */
    int leaking;
};
struct ff_vnode { struct vnode *vp; struct ff_mount *fs; uint32_t id; };
#define FN(vp) ((struct ff_vnode *)(vp)->v_data)
#define NODE(vp) (&FN(vp)->fs->image.nodes[FN(vp)->id])
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
    a->va_fileid = inode(&FN(vp)->fs->image, FN(vp)->id);
    a->va_size = n->type == FF_DIR ? n->entries : n->size;
    a->va_blocksize = PAGE_SIZE; a->va_bytes = roundup2(a->va_size, 512);
    a->va_atime = a->va_mtime = a->va_ctime = a->va_birthtime = (struct timespec){0, 0};
    a->va_gen = 1; a->va_flags = 0; a->va_rdev = NODEV; a->va_filerev = 0;
    return 0;
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
    return uiomove(__DECONST(uint8_t *, FN(ap->a_vp)->fs->image.bytes) + n->off + u->uio_offset, count, u);
}
static int ff_readlink(struct vop_readlink_args *ap)
{
    struct ff_node *n = NODE(ap->a_vp);
    return uiomove(__DECONST(uint8_t *, FN(ap->a_vp)->fs->image.bytes) + n->off,
        MIN(MIN((uint64_t)ap->a_uio->uio_resid, INT_MAX), n->size), ap->a_uio);
}
static int ff_parent(struct mount *mp, void *arg, int flags, struct vnode **vpp)
{ return ff_vget(mp, *(ino_t *)arg, flags, vpp); }
static int ff_lookup_vop(struct vop_cachedlookup_args *ap)
{
    struct vnode *vp = ap->a_dvp;
    struct componentname *cn = ap->a_cnp;
    struct ff_image *im = &FN(vp)->fs->image;
    uint32_t id;
    ino_t ino;
    int error;
    *ap->a_vpp = NULL;
    if (cn->cn_nameiop != LOOKUP && (cn->cn_flags & ISLASTCN)) return EROFS;
    if (cn->cn_namelen == 1 && cn->cn_nameptr[0] == '.') {
        vref(vp); *ap->a_vpp = vp; return 0;
    }
    if (cn->cn_flags & ISDOTDOT) {
        ino = inode(im, NODE(vp)->parent);
        return vn_vget_ino_gen(vp, ff_parent, &ino, cn->cn_lkflags, ap->a_vpp);
    }
    error = ff_lookup(im, FN(vp)->id, cn->cn_nameptr, cn->cn_namelen, &id);
    if (!error) error = ff_vget(vp->v_mount, inode(im, id), cn->cn_lkflags, ap->a_vpp);
    if ((cn->cn_flags & MAKEENTRY) && (error == 0 || error == ENOENT)) cache_enter(vp, *ap->a_vpp, cn);
    return error;
}
static int ff_readdir(struct vop_readdir_args *ap)
{
    struct vnode *vp = ap->a_vp;
    struct ff_image *im = &FN(vp)->fs->image;
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
    /* The resident snapshot completes synchronously, including sendfile's callback. */
    int error = VOP_GETPAGES(ap->a_vp, ap->a_m, ap->a_count, ap->a_rbehind, ap->a_rahead);
    if (ap->a_iodone) ap->a_iodone(ap->a_arg, ap->a_m, ap->a_count,
        error == VM_PAGER_OK ? 0 : EIO);
    return error;
}

/* Pager references keep the vnode, mount snapshot, and cached clean pages alive. */
static int ff_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
    vm_ooffset_t off, struct ucred *cred, u_short *color)
{
    struct ff_vnode *n = handle;
    (void)size; (void)off; (void)cred;
    if (prot & VM_PROT_WRITE) return EACCES;
    vref(n->vp); *color = 0;
    return 0;
}
static void ff_pager_dtor(void *handle) { vrele(((struct ff_vnode *)handle)->vp); }
static int ff_fault(vm_object_t object, vm_ooffset_t offset, int prot, vm_page_t *mres)
{
    struct ff_vnode *vn = object->handle;
    struct ff_mount *fs = vn->fs;
    struct ff_node *n = &fs->image.nodes[vn->id];
    uint32_t source, copied;
    vm_offset_t address;
    vm_page_t page;
    (void)prot;
    if (ff_page(&fs->image, vn->id, offset, PAGE_SIZE, fs->leaking, &source, &copied)) return VM_PAGER_BAD;
    VM_OBJECT_WUNLOCK(object);
    address = (vm_offset_t)fs->image.bytes + source;
    if (copied) {
        uint32_t index = offset / PAGE_SIZE;
        sx_xlock(&fs->cache_lock);
        if (!fs->clean[n->blob]) fs->clean[n->blob] = malloc(
            (size_t)howmany(n->size, PAGE_SIZE) * sizeof(vm_offset_t), M_FEMTOFS, M_WAITOK | M_ZERO);
        if (!fs->clean[n->blob][index]) {
            fs->clean[n->blob][index] = (vm_offset_t)kmem_malloc(PAGE_SIZE, M_WAITOK | M_ZERO);
            memcpy((void *)fs->clean[n->blob][index], (void *)address, copied);
            atomic_add_long(&ff_clean_pages, 1);
        }
        address = fs->clean[n->blob][index];
        sx_xunlock(&fs->cache_lock);
    } else atomic_add_long(&ff_direct_faults, 1);
    page = vm_page_getfake(pmap_kextract(address), VM_MEMATTR_DEFAULT);
    VM_OBJECT_WLOCK(object);
    vm_page_replace(page, object, (*mres)->pindex, *mres);
    *mres = page;
    vm_page_valid(page);
    return VM_PAGER_OK;
}
static const struct cdev_pager_ops ff_pager = {
    .cdev_pg_ctor = ff_pager_ctor, .cdev_pg_dtor = ff_pager_dtor, .cdev_pg_fault = ff_fault
};

static int ff_mmap(struct file *fp, vm_map_t map, vm_offset_t *addr, vm_size_t size,
    vm_prot_t prot, vm_prot_t capmax, int flags, vm_ooffset_t off, struct thread *td)
{
    struct vnode *vp = fp->f_vnode;
    vm_object_t obj;
    vm_prot_t maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
    int error;
    if (prot & VM_PROT_WRITE) return EACCES;
    if (!(fp->f_flag & FREAD)) return EACCES;
    if (off > OFF_MAX || size > OFF_MAX - off || (off & PAGE_MASK)) return EINVAL;
    if ((error = vget(vp, LK_SHARED)) != 0) return error;
    if (vp->v_mount->mnt_flag & MNT_NOEXEC) maxprot &= ~VM_PROT_EXECUTE;
    maxprot &= capmax;
    if (prot & ~maxprot) { vput(vp); return EACCES; }
    error = mac_vnode_check_mmap(td->td_ucred, vp, prot, flags);
    if (error) { vput(vp); return error; }
    obj = cdev_pager_allocate(FN(vp), OBJT_DEVICE, &ff_pager, size, prot, off, td->td_ucred);
    vput(vp);
    if (!obj) return ENOMEM;
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
    if (ino != 1 && (ino < 2 || ino - 2 >= fs->image.count)) return ESTALE;
    id = ino == 1 ? fs->image.count : ino - 2;
    if (!fs->image.nodes[id].type || fs->image.nodes[id].type == FF_LINK) return ESTALE;
    error = vfs_hash_get(mp, ino, flags, curthread, vpp, NULL, NULL);
    if (error || *vpp) return error;
    error = getnewvnode("femtofs", mp, &ff_vops, &vp);
    if (error) return error;
    vn = malloc(sizeof(*vn), M_FEMTOFS, M_WAITOK | M_ZERO);
    *vn = (struct ff_vnode){vp, fs, id}; vp->v_data = vn;
    vp->v_type = IFTOVT(fs->image.nodes[id].mode);
    if (vp->v_type == VFIFO) vp->v_op = &ff_fifoops;
    if (ino == 1) vp->v_vflag |= VV_ROOT;
    lockmgr(vp->v_vnlock, LK_EXCLUSIVE, NULL);
    error = insmntque(vp, mp);
    if (error) { free(vn, M_FEMTOFS); return error; }
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
    s->f_blocks = fs->image.size / FF_PAGE;
    s->f_bfree = s->f_bavail = s->f_ffree = 0;
    s->f_files = fs->image.objects + 1;
    return 0;
}
static void ff_free(struct ff_mount *fs)
{
    uint32_t i, j;
    if (fs->clean) {
        for (i = 0; i < fs->image.nblobs; i++) if (fs->clean[i]) {
            for (j = 0; j < howmany(fs->image.blobs[i].size, PAGE_SIZE); j++)
                if (fs->clean[i][j]) kmem_free((void *)fs->clean[i][j], PAGE_SIZE);
            free(fs->clean[i], M_FEMTOFS);
        }
        free(fs->clean, M_FEMTOFS);
    }
    if (fs->image.bytes) kmem_free(__DECONST(uint8_t *, fs->image.bytes), round_page(fs->image.size));
    ff_destroy(&fs->image);
    if (fs->consumer) {
        g_topology_lock(); g_vfs_close(fs->consumer); g_topology_unlock();
    }
    if (fs->device) vrele(fs->device);
    sx_destroy(&fs->cache_lock);
    free(fs, M_FEMTOFS);
}
static int ff_mount(struct mount *mp)
{
    static const char *opts[] = {"from", "clean", "leaking", NULL};
    struct nameidata nd;
    struct ff_mount *fs;
    struct buf *bp = NULL;
    struct vnode *vp;
    char *from;
    uint32_t off, count;
    int error;
    if (mp->mnt_flag & MNT_UPDATE) return EOPNOTSUPP;
    if (vfs_filteropt(mp->mnt_optnew, opts)) return EINVAL;
    if (vfs_flagopt(mp->mnt_optnew, "clean", NULL, 0) && vfs_flagopt(mp->mnt_optnew, "leaking", NULL, 0)) return EINVAL;
    from = vfs_getopts(mp->mnt_optnew, "from", &error);
    if (error) return error;
    NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, from);
    if ((error = namei(&nd))) return error;
    NDFREE_PNBUF(&nd); vp = nd.ni_vp;
    if (!vn_isdisk_error(vp, &error)) { vput(vp); return error; }
    error = VOP_ACCESS(vp, VREAD, curthread->td_ucred, curthread);
    if (error) error = priv_check(curthread, PRIV_VFS_MOUNT_PERM);
    if (error) { vput(vp); return error; }
    fs = malloc(sizeof(*fs), M_FEMTOFS, M_WAITOK | M_ZERO);
    sx_init(&fs->cache_lock, "femtofs clean pages");
    fs->device = vp;
    fs->leaking = PAGE_SIZE <= FF_PAGE && vfs_flagopt(mp->mnt_optnew, "leaking", NULL, 0);
    /* Mode-bit visibility cannot authorize spill under an active MAC policy. */
    if (mac_vnode_check_access_enabled() || mac_vnode_check_lookup_enabled() ||
        mac_vnode_check_open_enabled() || mac_vnode_check_mmap_enabled()) fs->leaking = 0;
    g_topology_lock(); error = g_vfs_open(vp, &fs->consumer, "femtofs", 0); g_topology_unlock();
    VOP_UNLOCK(vp);
    if (error) goto fail;
    if (!fs->consumer->provider->sectorsize || FF_PAGE % fs->consumer->provider->sectorsize ||
        fs->consumer->provider->mediasize < FF_PAGE) { error = EINVAL; goto fail; }
    error = bread(vp, 0, FF_PAGE, NOCRED, &bp);
    if (error) goto fail;
    error = ff_header((const uint8_t *)bp->b_data, fs->consumer->provider->mediasize, &fs->image);
    brelse(bp); bp = NULL;
    if (error) goto fail;
    fs->image.bytes = (uint8_t *)kmem_malloc(round_page(fs->image.size), M_NOWAIT | M_ZERO);
    if (!fs->image.bytes) { error = ENOMEM; goto fail; }
    for (off = 0; off < fs->image.size; off += count) {
        count = MIN(FF_PAGE, fs->image.size - off);
        error = bread(vp, off / DEV_BSIZE, count, NOCRED, &bp);
        if (error) goto fail;
        memcpy(__DECONST(uint8_t *, fs->image.bytes) + off, bp->b_data, count);
        brelse(bp); bp = NULL;
    }
    error = ff_validate(&fs->image);
    if (error) goto fail;
    fs->clean = malloc((size_t)fs->image.nblobs * sizeof(*fs->clean), M_FEMTOFS, M_WAITOK | M_ZERO);
    mp->mnt_data = fs;
    MNT_ILOCK(mp); mp->mnt_flag |= MNT_RDONLY | MNT_LOCAL; MNT_IUNLOCK(mp);
    vfs_getnewfsid(mp); vfs_mountedfrom(mp, from); ff_statfs(mp, &mp->mnt_stat);
    return 0;
fail:
    if (bp) brelse(bp);
    ff_free(fs);
    return error;
}
static int ff_unmount(struct mount *mp, int flags)
{
    int error;
    /* Forced reclaim could leave device-pager handles pointing at dead vnodes. */
    if (flags & MNT_FORCE) return EOPNOTSUPP;
    error = vflush(mp, 0, 0, curthread);
    if (error) return error;
    ff_free(mp->mnt_data); mp->mnt_data = NULL;
    return 0;
}
static int ff_init(struct vfsconf *vfc)
{ (void)vfc; ff_fileops = vnops; ff_fileops.fo_mmap = ff_mmap; return 0; }
static struct vfsops ff_vfsops = {
    .vfs_init = ff_init, .vfs_mount = ff_mount, .vfs_unmount = ff_unmount,
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
    .vop_setattr = ff_setattr, .vop_pathconf = vop_stdpathconf,
    .vop_advise = VOP_NULL, /* The immutable snapshot is already resident. */
};
VFS_VOP_VECTOR_REGISTER(ff_vops);
static struct vop_vector ff_fifoops = {
    .vop_default = &fifo_specops, .vop_access = ff_access, .vop_getattr = ff_getattr,
    .vop_reclaim = ff_reclaim, .vop_inactive = VOP_NULL, .vop_setattr = ff_setattr,
};
VFS_VOP_VECTOR_REGISTER(ff_fifoops);
// END File: ko/femtofs.c
