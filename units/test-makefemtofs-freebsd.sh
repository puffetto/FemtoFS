#!/bin/sh
# File: units/test-makefemtofs-freebsd.sh
# Created by Andrea "Nemesi" Cocito on 14/10/2026
# Opt-in raw-provider integration using only test-owned vnode memory disks.
set -eu

if [ "$(uname -s)" != FreeBSD ]; then
    echo 'SKIP: raw-provider integration requires FreeBSD' >&2
    exit 77
fi
if [ "$(id -u)" -ne 0 ]; then
    echo 'SKIP: allocating a disposable md provider requires root' >&2
    exit 77
fi
builder=$1
work_root=$2
case_dir=$(mktemp -d "$work_root/makefemtofs-device.XXXXXX")
unit=
cleanup() {
    result=$?
    trap - EXIT HUP INT TERM
    if [ -n "$unit" ] && ! mdconfig -d -u "$unit"; then
        echo "Failed to detach owned $unit; retaining $case_dir" >&2
        exit 1
    fi
    rm -rf "$case_dir"
    exit "$result"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
mkdir "$case_dir/source"
printf 'public payload\n' > "$case_dir/source/public"
printf 'private payload\n' > "$case_dir/source/private"
chmod 0600 "$case_dir/source/private"
uuid=00112233-4455-6677-8899-aabbccddeeff
"$builder" --temp-dir "$case_dir" --uuid "$uuid" \
    "$case_dir/source" "$case_dir/reference.0fs"
image_size=$(wc -c < "$case_dir/reference.0fs" | tr -d ' ')
image_pages=$((image_size / 4096))
uname -a
getconf PAGESIZE

for sector in 512 4096 8192; do
    for capacity in small exact larger; do
        # The nondividing-sector case only needs a sufficiently large provider.
        if [ "$sector" -eq 8192 ] && [ "$capacity" != larger ]; then continue; fi
        case "$capacity" in
            small) media_size=$((image_size - 4096)) ;;
            exact) media_size=$image_size ;;
            larger) media_size=$(((image_size + 16383) / 8192 * 8192)) ;;
        esac
        python3 - "$case_dir/backing" "$media_size" <<'PY'
from pathlib import Path
import sys
Path(sys.argv[1]).write_bytes(b'\xa5' * int(sys.argv[2]))
PY
        unit=$(mdconfig -a -t vnode -f "$case_dir/backing" -S "$sector")
        unit_number=${unit#md}
        case "$unit_number" in
            ''|*[!0-9]*) echo "Unexpected mdconfig identity: $unit" >&2; exit 1 ;;
        esac
        printf 'provider=%s sector=%s capacity=%s\n' "$unit" "$sector" "$capacity"
        if [ "$capacity" = small ] || [ "$sector" -eq 8192 ]; then
            if "$builder" --device --temp-dir "$case_dir" --uuid "$uuid" \
                "$case_dir/source" "/dev/$unit" > "$case_dir/result" 2>&1; then
                echo 'invalid provider was accepted' >&2
                exit 1
            fi
            if [ "$capacity" = small ]; then
                grep -Fq 'smaller than the image' "$case_dir/result"
            else
                grep -Fq 'sector size does not divide 4096' "$case_dir/result"
            fi
        else
            "$builder" --device --temp-dir "$case_dir" --uuid "$uuid" \
                "$case_dir/source" "/dev/$unit"
            dd if="/dev/$unit" of="$case_dir/readback.0fs" bs=4096 \
                count="$image_pages" 2> "$case_dir/dd.log"
            cmp "$case_dir/reference.0fs" "$case_dir/readback.0fs"
        fi
        mdconfig -d -u "$unit"
        unit=
        python3 - "$case_dir/backing" "$case_dir/reference.0fs" "$capacity" "$sector" <<'PY'
from pathlib import Path
import sys
backing = Path(sys.argv[1]).read_bytes()
reference = Path(sys.argv[2]).read_bytes()
if sys.argv[3] == 'small' or sys.argv[4] == '8192':
    assert backing == b'\xa5' * len(backing), 'rejected provider was modified'
else:
    assert backing[:len(reference)] == reference, 'provider differs from regular output'
    assert backing[len(reference):] == b'\xa5' * (len(backing) - len(reference)), 'tail erased'
PY
    done
done
echo 'PASS: provider capacity, sector, image equivalence, and untouched-tail checks'
echo 'NOT TESTED: mounted refusal, consumer lookup/mmap, and real-provider fault injection'
# END File: units/test-makefemtofs-freebsd.sh
