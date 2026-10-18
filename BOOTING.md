# Booting a compressed `.0fs` root image

Use FreeBSD 15.1 amd64 with a `femtofs.ko` built against the running kernel's
matching sources (see [module build instructions](ko/README.md#build-and-use)).
The loader and kernel stay on a normal bootable filesystem; the appliance root
lives in a named resident bank. No md device or kernel patch is required.

Prepare a trimmed FreeBSD root tree containing stock `/sbin/init`, `/bin/sh`,
their libraries, `/etc/rc`, the required base utilities and configuration.
Include `/dev`, `/var`, `/tmp`, and the `mount_femtofs` helper in `/sbin`.
Keep any modules needed after root mounting in the tree's `/boot/kernel`.
The format allows at most 32,767 non-root objects; an untrimmed base installation
with development files and documentation can exceed that limit.

For a read-only root, put these settings in the **image's** `/etc/rc.conf`:

```sh
hostname="appliance"
root_rw_mount="NO"
varmfs="YES"
populate_var="YES"
tmpmfs="YES"
entropy_boot_file="NO"
entropy_file="/var/db/entropy-file"
entropy_dir="/var/db/entropy"
dumpdev="NO"
```

Use this root entry in the image's `/etc/fstab` (the last zero disables fsck):

```text
root / femtofs ro,clean 0 0
```

Do not also configure `/var` and `/tmp` in fstab when rc creates them using
`varmfs`/`tmpmfs`; their contents are volatile. Provision a unique `/etc/hostid`
and matching `/etc/machine-id`, accounts and any enabled SSH host keys before
building. Set `/etc/motd` to a symlink to `/var/run/motd` and provide
`/etc/motd.template`, so stock rc need not rewrite `/etc`. Select services
appropriate to the appliance and arrange writable storage for their state.

Build and compress the prepared tree, preserving its intended numeric ownership
and permissions:

```sh
sudo build/makefemtofs --temp-dir "$PWD/tmp" appliance-root bootme.0fs
gzip -n -9 -c bootme.0fs > bootme.0fs.gz
```

Install `bootme.0fs.gz` as `/boot/bootme.0fs.gz` on the **boot filesystem** and
install the matching module as `/boot/kernel/femtofs.ko`. In that filesystem's
`/boot/loader.conf`, use:

```text
femtofs_load="YES"
bootme_load="YES"
bootme_type="femtofs_image"
bootme_name="/boot/bootme.0fs"
femtofs.bank.root="/boot/bootme.0fs"
vfs.root.mountfrom="femtofs:"
vfs.root.mountfrom.options="ro,clean,bank=root"
```

**Omit `.gz` in the loader settings.** FreeBSD's loader opens the `.gz` file
through its transparent decompressor when asked for the unsuffixed name.
Explicitly requesting `bootme.0fs.gz` bypasses that decompression path. Leave
only the compressed file at this location to avoid selecting a stale
uncompressed image. The bank mapping uses the loader-recorded unsuffixed name;
`root` is the bank label, not an image filename. The settings use the stock
[`loader.conf` module directives](https://docs.freebsd.org/en/books/handbook/boot/).

The equivalent loader-prompt commands, after loading the matching kernel, are:

```text
load /boot/kernel/femtofs.ko
load -t femtofs_image /boot/bootme.0fs
set femtofs.bank.root=/boot/bootme.0fs
set vfs.root.mountfrom="femtofs:"
set vfs.root.mountfrom.options="ro,clean,bank=root"
boot
```

The loader decompresses the image into RAM. FemtoFS validates and adopts those
pages without allocating another full-image copy. Compression saves boot-medium
space; RAM must still accommodate the entire decompressed bank, kernel, runtime
state, and writable filesystems. Clean mappings may allocate shared tail pages,
and private writes allocate COW pages.

The disposable acceptance test uses a real trimmed base tree, stock loader and
stock rc, with only `bootme.0fs.gz` on the boot disk:

```sh
sudo python3 ko/test-boot-compressed.py build/makefemtofs /usr/src tmp/compressed-boot
```

It requires matching installed FreeBSD sources/binaries and QEMU. The work
directory must be new; its console log and image artifacts are retained.
Its automatic root console login and generated test keys are isolated test
fixtures, not deployment settings.

Verified on 18 October 2026 with stock FreeBSD **15.1-RELEASE-p4 amd64**, QEMU
TCG/BIOS, two CPUs and 1,536 MiB RAM: a **339,423,232-byte** root image stored
as **109,361,667 bytes** of gzip data boots through stock rc and console login.
File checksums, read-only root enforcement, writable `/var` and `/tmp`,
syslogd/cron/sshd, loopback SSH and `su` pass. The boot disk is never mounted;
all userspace runs from FemtoFS. Bank counters report the full decompressed
size as loader-owned storage and zero imported bytes. Shutdown syncs buffers
and powers off; the existing forced-unmount limitation still prints
`unmount of / failed (45)`. UEFI and physical hardware have not been tested.
