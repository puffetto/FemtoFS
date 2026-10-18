#!/bin/sh
# File: ko/test-boot-compressed-guest.sh
# Created by Andrea "Nemesi" Cocito on 18/10/2026
# Acceptance checks after stock rc and serial-console login in the boot guest.
set -eu
export PATH=/sbin:/bin:/usr/sbin:/usr/bin
echo 'FEMTOFS_COMPRESSED console login after stock rc'
test "$(id -u)" = 0
test "$(hostname)" = bootme
mount -p
mount -p | awk '$2 == "/" && $3 == "femtofs" && $4 ~ /(^|,)ro(,|$)/ {ok=1} END {exit !ok}'
test -c /dev/null
touch /var/run/femtofs-writable /tmp/femtofs-writable
if touch /femtofs-must-stay-readonly 2>/dev/null; then exit 1; fi
if mount -uw / 2>/dev/null; then exit 1; fi
while read -r digest name; do
    test "$(sha256 -q "$name")" = "$digest"
done < /root/checksums
for service in syslogd cron sshd; do
    service "$service" onestatus
done
test "$(su -m nobody -c 'id -u')" = 65534
test "$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/tmp/known_hosts \
    -i /root/.ssh/id_ed25519 root@127.0.0.1 'id -u')" = 0
logger -p user.notice FEMTOFS_COMPRESSED_SYSLOG
sleep 1
grep -q FEMTOFS_COMPRESSED_SYSLOG /var/log/messages
femtoctl list
test "$(sysctl -n vfs.femtofs.imported_bytes)" = 0
test "$(sysctl -n vfs.femtofs.loader_bytes)" = "$(sysctl -n vfs.femtofs.loader_image_bytes)"
echo "FEMTOFS_COMPRESSED loader_bytes=$(sysctl -n vfs.femtofs.loader_bytes) imported_bytes=0"
echo 'FEMTOFS_COMPRESSED ALL TESTS PASSED'
# END File: ko/test-boot-compressed-guest.sh
