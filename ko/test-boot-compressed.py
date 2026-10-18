#!/usr/bin/env python3
# File: ko/test-boot-compressed.py
# Created by Andrea "Nemesi" Cocito on 18/10/2026
# Boot a compressed real FreeBSD appliance tree through stock loader and rc.
import gzip
import hashlib
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import uuid


def run(*args, **kw):
    return subprocess.run([str(arg) for arg in args], check=True, **kw)


if platform.system() != 'FreeBSD' or len(sys.argv) != 4:
    sys.exit('usage on FreeBSD: test-boot-compressed.py builder matching-src new-work-directory')
if os.geteuid() != 0:
    sys.exit('run as root to give the staged base tree and private keys their intended ownership')
builder, src, work = [Path(p).resolve() for p in sys.argv[1:]]
repo = Path(__file__).resolve().parent.parent
work.mkdir()
root, stage, build = (work / name for name in ('root', 'stage', 'module'))
for directory in (root, stage, build):
    directory.mkdir()

# Use stock installed binaries and rc scripts, not a rescue shell/custom rc.
# Copy only generic configuration: never host accounts, keys or local rc.conf.
for name in ('bin', 'sbin', 'lib', 'libexec', 'usr/bin', 'usr/sbin', 'usr/libexec',
             'usr/share/misc', 'usr/share/locale', 'usr/share/zoneinfo',
             'etc/defaults', 'etc/rc.d', 'etc/pam.d', 'etc/mtree'):
    shutil.copytree('/' + name, root/name, symlinks=True, copy_function=shutil.copy)
(root/'usr/lib').mkdir(exist_ok=True)
for path in Path('/usr/lib').glob('*.so*'):
    shutil.copy(path, root/'usr/lib'/path.name, follow_symlinks=False)
for name in ('rc', 'rc.subr', 'rc.shutdown', 'rc.initdiskless', 'network.subr',
             'login.conf', 'login.access', 'gettytab', 'protocols', 'services',
             'netconfig', 'nsswitch.conf'):
    shutil.copy('/etc/' + name, root/'etc'/name)
for name in ('group', 'master.passwd', 'shells'):
    shutil.copy(src/'etc'/name, root/'etc'/name)
run('pwd_mkdb', '-p', '-d', root/'etc', root/'etc/master.passwd')
for name in ('dev', 'tmp', 'var', 'root', 'etc/ssh', 'root/.ssh', 'boot/kernel'):
    (root/name).mkdir(parents=True, exist_ok=True)
(root/'tmp').chmod(0o1777)
(root/'root').chmod(0o700)
(root/'root/.ssh').chmod(0o700)
(root/'etc/master.passwd').chmod(0o600)

env = dict(os.environ, MAKEOBJDIR=str(build))
run('make', '-C', repo/'ko', f'SYSDIR={src}/sys', env=env)
for name in ('mount_femtofs', 'femtoctl'):
    run('cc', '-Wall', '-Wextra', '-Werror', repo/f'ko/{name}.c', '-o', root/'sbin'/name)
shutil.copy(build/'femtofs.ko', root/'boot/kernel/femtofs.ko')

hostid = str(uuid.uuid4())
(root/'etc/hostid').write_text(hostid + '\n')
(root/'etc/machine-id').write_text(hostid.replace('-', '') + '\n')
(root/'etc/hosts').write_text('127.0.0.1 localhost bootme\n::1 localhost bootme\n')
(root/'etc/fstab').write_text('root / femtofs ro,clean 0 0\n')
(root/'etc/rc.conf').write_text('''hostname="bootme"
root_rw_mount="NO"
varmfs="YES"
varsize="32m"
populate_var="YES"
tmpmfs="YES"
tmpsize="16m"
entropy_boot_file="NO"
entropy_file="/var/db/entropy-file"
entropy_dir="/var/db/entropy"
dumpdev="NO"
sendmail_enable="NONE"
syslogd_enable="YES"
syslogd_flags="-ss"
cron_enable="YES"
sshd_enable="YES"
sshd_rsa_enable="NO"
sshd_ecdsa_enable="NO"
devmatch_enable="NO"
kldxref_enable="NO"
background_fsck="NO"
''')
(root/'etc/syslog.conf').write_text('*.notice;authpriv.* /var/log/messages\n')
for name in ('sysctl.conf', 'devfs.conf', 'crontab', 'motd.template'):
    (root/'etc'/name).write_text('')
(root/'etc/motd').symlink_to('/var/run/motd')
# No network adapter is attached; SSH is tested only over guest loopback.
for key in ('etc/ssh/ssh_host_ed25519_key', 'root/.ssh/id_ed25519'):
    run('ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', root/key)
shutil.copy(root/'root/.ssh/id_ed25519.pub', root/'root/.ssh/authorized_keys')
(root/'root/.ssh/authorized_keys').chmod(0o600)
(root/'etc/ssh/sshd_config').write_text('''ListenAddress 127.0.0.1
HostKey /etc/ssh/ssh_host_ed25519_key
PermitRootLogin prohibit-password
PasswordAuthentication no
UsePAM yes
''')
# A real getty/login starts the checks only after stock rc has returned to init.
# Passwordless console root is strictly a disposable, network-isolated fixture.
(root/'etc/ttys').write_text('console none unknown off secure\n'
    'ttyu0 "/usr/libexec/getty -a root 3wire" vt100 on secure\n')
shutil.copy(repo/'ko/test-boot-compressed-guest.sh', root/'root/check-boot.sh')
(root/'root/.profile').write_text('''/bin/sh /root/check-boot.sh
result=$?
echo "FEMTOFS_COMPRESSED GUEST_STATUS=$result"
/sbin/shutdown -p now
''')
probes = ('bin/sh', 'sbin/init', 'etc/rc', 'usr/bin/su', 'usr/sbin/sshd')
(root/'root/checksums').write_text(''.join(
    hashlib.sha256((root/name).read_bytes()).hexdigest() + ' /' + name + '\n'
    for name in probes))

# The boot disk has only the compressed image. All userspace comes from FemtoFS.
(stage/'boot/kernel').mkdir(parents=True)
for name in ('loader', 'loader_lua', 'lua', 'defaults'):
    source = Path('/boot')/name
    if source.is_dir():
        shutil.copytree(source, stage/'boot'/name, copy_function=shutil.copy)
    else:
        shutil.copy(source, stage/'boot'/name)
shutil.copy('/boot/kernel/kernel', stage/'boot/kernel/kernel')
shutil.copy(build/'femtofs.ko', stage/'boot/kernel/femtofs.ko')
(stage/'boot/loader.conf').write_text('''console="comconsole"
comconsole_speed="115200"
boot_serial="YES"
boot_verbose="YES"
autoboot_delay="-1"
femtofs_load="YES"
bootme_load="YES"
bootme_type="femtofs_image"
bootme_name="/boot/bootme.0fs"
femtofs.bank.root="/boot/bootme.0fs"
vfs.root.mountfrom="femtofs:"
vfs.root.mountfrom.options="ro,clean,bank=root"
''')
image = work/'bootme.0fs'
run(builder, '--root-uid', '0', '--root-gid', '0', '--temp-dir', work, root, image)
image_size = image.stat().st_size
packed = stage/'boot/bootme.0fs.gz'
with image.open('rb') as source, packed.open('wb') as target:
    with gzip.GzipFile(fileobj=target, mode='wb', mtime=0) as compressed:
        shutil.copyfileobj(source, compressed)
with gzip.open(packed, 'rb') as compressed:
    digest = hashlib.file_digest(compressed, 'sha256').hexdigest()
with image.open('rb') as source:
    assert digest == hashlib.file_digest(source, 'sha256').hexdigest()
assert not (stage/'boot/bootme.0fs').exists()
print(f'IMAGE bytes={image_size} gzip_bytes={packed.stat().st_size} sha256={digest}', flush=True)
print('HOST ' + run('uname', '-a', capture_output=True, text=True).stdout, flush=True)
run('makefs', '-t', 'ffs', '-s', '256m', '-o', 'version=2', work/'boot.ufs', stage)
run('mkimg', '-s', 'gpt', '-b', '/boot/pmbr', '-p', 'freebsd-boot:=/boot/gptboot',
    '-p', f'freebsd-ufs:={work}/boot.ufs', '-o', work/'guest.img')
command = ['qemu-system-x86_64', '-accel', 'tcg', '-m', '1536', '-smp', '2',
           '-display', 'none', '-monitor', 'none', '-serial', 'stdio', '-nic', 'none',
           '-device', 'virtio-rng-pci', '-drive',
           f'file={work}/guest.img,format=raw,if=ide,snapshot=on', '-no-reboot']
with (work/'console.log').open('w') as log:
    result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=log,
                            stderr=subprocess.STDOUT, timeout=900)
output = (work/'console.log').read_text(errors='replace')
assert result.returncode == 0, work/'console.log'
assert 'FEMTOFS_COMPRESSED ALL TESTS PASSED' in output, work/'console.log'
assert 'FEMTOFS_COMPRESSED GUEST_STATUS=0' in output
assert re.search(r'femtofs: preload root </boot/bootme\.0fs> ' + str(image_size) + r' bytes', output)
assert re.search(r'femtofs: bank root preload ' + str(image_size) + r' bytes', output)
assert f'FEMTOFS_COMPRESSED loader_bytes={image_size} imported_bytes=0' in output
for failure in ('panic:', 'lock order reversal:', 'startup aborted', 'Traceback'):
    assert failure not in output, failure
print(f'PASS: gzip loader, FemtoFS root, stock rc, login, SSH, su, services, shutdown; {work}/console.log')
# END File: ko/test-boot-compressed.py
