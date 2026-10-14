#!/bin/sh
# File: units/test-makefemtofs.sh
# Created by Andrea "Nemesi" Cocito on 24/08/2026
# Exercise equivalent directory and archive builds through the public CLI.

set -eu

builder=$1
work_root=$2
case_dir=$(mktemp -d "$work_root/makefemtofs-test.XXXXXX")
trap 'rm -rf "$case_dir"' EXIT HUP INT TERM

mkdir -p "$case_dir/source/bin" "$case_dir/source/etc"
printf 'hello femtoFS\n' > "$case_dir/source/etc/message"
cp "$case_dir/source/etc/message" "$case_dir/source/etc/message-copy"
printf 'same' > "$case_dir/source/etc/same"
printf '#!/bin/sh\necho ok\n' > "$case_dir/source/bin/tool"
chmod 0755 "$case_dir/source/bin/tool"
ln "$case_dir/source/etc/message" "$case_dir/source/etc/message-link"
ln -s ../etc/message "$case_dir/source/bin/message"
mkfifo "$case_dir/source/pipe"

uuid=00112233-4455-6677-8899-aabbccddeeff
"$builder" --temp-dir "$case_dir" --uuid "$uuid" "$case_dir/source" "$case_dir/directory.img"
tar -C "$case_dir/source" -cf "$case_dir/source.tar" .
"$builder" --temp-dir "$case_dir" --uuid "$uuid" "$case_dir/source.tar" "$case_dir/archive.img"
cmp "$case_dir/directory.img" "$case_dir/archive.img"
"$builder" --temp-dir "$case_dir" --verify structure --uuid "$uuid" "$case_dir/source" \
    "$case_dir/structure.img"
cmp "$case_dir/directory.img" "$case_dir/structure.img"

if "$builder" --temp-dir "$case_dir" --uuid "$uuid" "$case_dir/source" "$case_dir/directory.img"; then
    echo "existing output was replaced without --force" >&2
    exit 1
fi

mkdir "$case_dir/empty"
"$builder" --temp-dir "$case_dir" --uuid "$uuid" "$case_dir/empty" "$case_dir/empty.img"
test "$(wc -c < "$case_dir/empty.img")" -eq 4096

mkdir "$case_dir/hard-hash"
while IFS= read -r name
do
    : > "$case_dir/hard-hash/$name"
done < "$(dirname "$0")/dual-hash-names.txt"
"$builder" --temp-dir "$case_dir" --uuid "$uuid" "$case_dir/hard-hash" "$case_dir/hard-hash.img"
root_hash_control=$(od -An -tu1 -j 66 -N 1 "$case_dir/hard-hash.img")
test $((root_hash_control >> 6)) -eq 1
hash2_bytes=$(od -An -tx1 -j 68 -N 4 "$case_dir/hard-hash.img" | tr -d ' \n')
test "$hash2_bytes" != 00000000
tar -C "$case_dir/hard-hash" -cf "$case_dir/hard-hash.tar" .
"$builder" --temp-dir "$case_dir" --uuid "$uuid" "$case_dir/hard-hash.tar" \
    "$case_dir/hard-hash-archive.img"
cmp "$case_dir/hard-hash.img" "$case_dir/hard-hash-archive.img"

mkdir "$case_dir/fifo-hardlinks"
mkfifo "$case_dir/fifo-hardlinks/pipe-a"
ln "$case_dir/fifo-hardlinks/pipe-a" "$case_dir/fifo-hardlinks/pipe-b"
if "$builder" --temp-dir "$case_dir" --dry-run "$case_dir/fifo-hardlinks" \
    "$case_dir/fifo-hardlinks.img"; then
    echo "duplicate FIFO inode was accepted" >&2
    exit 1
fi

# END File: units/test-makefemtofs.sh
