#!/bin/sh
# File: units/test-femtofsSim.sh
# Created by Andrea "Nemesi" Cocito on 24/08/2026
# Check specification-sensitive simulator page and image accounting.

set -eu

simulator=$1
work_root=$2
case_dir=$(mktemp -d "$work_root/femtofsSim-test.XXXXXX")
trap 'rm -rf "$case_dir"' EXIT HUP INT TERM

cat > "$case_dir/list" <<'EOF'
1 1 drwxr-xr-x 2 root wheel 0 Aug 24 2026 ./
2 10 -r--r--r-- 1 root wheel 5000 Aug 24 2026 ./large
EOF

"$simulator" "$case_dir/list" 4096 --vm-page-size=4096 \
    --no-dual-hash-experiment --no-budgeted-hash-experiment > "$case_dir/report"

grep -Eq '^  specification baseline: +0x0100$' "$case_dir/report"
grep -Eq '^  encoded page-size code: +4$' "$case_dir/report"
grep -Eq '^  public_off: +4096$' "$case_dir/report"
grep -Eq '^  private_off: +12288$' "$case_dir/report"
grep -Eq '^  image_size: +12.00 KiB \(12288 bytes\)$' "$case_dir/report"
grep -Eq '^Structural format-limit result: representable$' "$case_dir/report"

sed 's# \./$# .#; s# \./large$# ././large#' "$case_dir/list" \
    > "$case_dir/normalized-list"
"$simulator" "$case_dir/normalized-list" \
    --no-dual-hash-experiment --no-budgeted-hash-experiment \
    > "$case_dir/normalized-report"
tail -n +2 "$case_dir/report" > "$case_dir/report-body"
tail -n +2 "$case_dir/normalized-report" > "$case_dir/normalized-body"
cmp "$case_dir/report-body" "$case_dir/normalized-body"

"$simulator" "$case_dir/list" 4096 --vm-page-size=16384 \
    --no-dual-hash-experiment --no-budgeted-hash-experiment > "$case_dir/large-vm-report"
grep -Eq '^  effective mmap mode forced: +clean$' "$case_dir/large-vm-report"
grep -Eq '^  leaking guaranteed-aligned: +0$' "$case_dir/large-vm-report"

"$simulator" "$case_dir/list" 256 --vm-page-size=4096 \
    --no-dual-hash-experiment --no-budgeted-hash-experiment > "$case_dir/future-page-report"
grep -Eq '^  encoded page-size code: +0$' "$case_dir/future-page-report"
grep -Fq 'future-format experiment, not a valid 0x0100 image model' \
    "$case_dir/future-page-report"

sed -n '1p' "$case_dir/list" > "$case_dir/root-only-list"
"$simulator" "$case_dir/root-only-list" 2147483648 \
    --no-dual-hash-experiment --no-budgeted-hash-experiment > "$case_dir/max-page-report"
grep -Eq '^  encoded page-size code: +23$' "$case_dir/max-page-report"
grep -Eq '^  image_size: +2.00 GiB \(2147483648 bytes\)$' "$case_dir/max-page-report"
grep -Eq '^Structural format-limit result: representable$' "$case_dir/max-page-report"

if "$simulator" "$case_dir/list" 3000 \
    --no-dual-hash-experiment --no-budgeted-hash-experiment \
    > "$case_dir/invalid-report" 2> "$case_dir/invalid-error"; then
    echo "invalid image page size was accepted" >&2
    exit 1
fi
grep -Fq 'image PAGE_SIZE must be a power of two from 256 through 2^31' \
    "$case_dir/invalid-error"

for path in './missing/file' './file/' './dir/./file' './../file' '/absolute' './.' './..'
do
    sed -n '1p' "$case_dir/list" > "$case_dir/invalid-list"
    printf '2 1 -rw-r--r-- 1 root wheel 1 Aug 24 2026 %s\n' "$path" \
        >> "$case_dir/invalid-list"
    if "$simulator" "$case_dir/invalid-list" \
        --no-dual-hash-experiment --no-budgeted-hash-experiment \
        > "$case_dir/invalid-report" 2> "$case_dir/invalid-error"; then
        echo "invalid pathname was accepted: $path" >&2
        exit 1
    fi
done

cp "$case_dir/list" "$case_dir/invalid-list"
sed -n '2p' "$case_dir/list" >> "$case_dir/invalid-list"
if "$simulator" "$case_dir/invalid-list" \
    --no-dual-hash-experiment --no-budgeted-hash-experiment \
    > "$case_dir/invalid-report" 2> "$case_dir/invalid-error"; then
    echo "duplicate pathname was accepted" >&2
    exit 1
fi

cp "$case_dir/list" "$case_dir/invalid-list"
printf '3 1 -rw-r--r-- 1 root wheel 1 Aug 24 2026 ./large/file\n' \
    >> "$case_dir/invalid-list"
if "$simulator" "$case_dir/invalid-list" \
    --no-dual-hash-experiment --no-budgeted-hash-experiment \
    > "$case_dir/invalid-report" 2> "$case_dir/invalid-error"; then
    echo "non-directory parent was accepted" >&2
    exit 1
fi

long_name=$(printf '%0256d' 0 | tr '0' 'x')
sed -n '1p' "$case_dir/list" > "$case_dir/invalid-list"
printf '2 1 -rw-r--r-- 1 root wheel 1 Aug 24 2026 ./%s\n' "$long_name" \
    >> "$case_dir/invalid-list"
if "$simulator" "$case_dir/invalid-list" \
    --no-dual-hash-experiment --no-budgeted-hash-experiment \
    > "$case_dir/invalid-report" 2> "$case_dir/invalid-error"; then
    echo "overlong filename was accepted" >&2
    exit 1
fi

for size in 4294967292 18446744073709551613 18446744073709551615
do
    sed -n '1p' "$case_dir/list" > "$case_dir/overflow-list"
    printf '2 1 -rw-r--r-- 1 root wheel %s Aug 24 2026 ./file\n' "$size" \
        >> "$case_dir/overflow-list"
    if "$simulator" "$case_dir/overflow-list" \
        --no-dual-hash-experiment --no-budgeted-hash-experiment \
        > "$case_dir/overflow-report" 2> "$case_dir/overflow-error"; then
        echo "unencodable payload size was accepted: $size" >&2
        exit 1
    fi
    grep -Eq '^  payload blobs overflow-skip: +1$' "$case_dir/overflow-report"
    grep -Eq '^Structural format-limit result: INVALID$' "$case_dir/overflow-report"
done

sed -n '1p' "$case_dir/list" > "$case_dir/max-blob-list"
printf '2 1 -rw-r--r-- 1 root wheel 4294967291 Aug 24 2026 ./file\n' \
    >> "$case_dir/max-blob-list"
if "$simulator" "$case_dir/max-blob-list" \
    --no-dual-hash-experiment --no-budgeted-hash-experiment \
    > "$case_dir/max-blob-report" 2> "$case_dir/max-blob-error"; then
    echo "image exceeding 32-bit size was accepted" >&2
    exit 1
fi
grep -Eq '^  payload blobs overflow-skip: +0$' "$case_dir/max-blob-report"
grep -Eq '^  image-size limit status: +EXCEEDED$' "$case_dir/max-blob-report"

sed -n '1p' "$case_dir/list" > "$case_dir/single-list"
inode=2
for name in akbb bbbe bjlm bkhe cblh ebac edgg ffme fmkk hjdh hkgg icci \
    ikde jfab jhjb jkkf kide kkbj kkec leel mbed meie
do
    printf '%s 1 -rw-r--r-- 1 root wheel 0 Aug 24 2026 ./%s\n' "$inode" "$name" \
        >> "$case_dir/single-list"
    inode=$((inode + 1))
done
"$simulator" "$case_dir/single-list" \
    --no-dual-hash-experiment --no-budgeted-hash-experiment \
    > "$case_dir/single-report"
grep -Eq '^  total buckets: +41$' "$case_dir/single-report"
grep -Eq '^  successful lookup comparisons: +23$' "$case_dir/single-report"
grep -Eq '^  avg strcmp per unsuccessful lookup: +0.5610$' "$case_dir/single-report"

sed -n '1p' "$case_dir/list" > "$case_dir/dual-list"
inode=2
while IFS= read -r name
do
    printf '%s 1 -rw-r--r-- 1 root wheel 0 Aug 24 2026 ./%s\n' "$inode" "$name" \
        >> "$case_dir/dual-list"
    inode=$((inode + 1))
done < "$(dirname "$0")/dual-hash-names.txt"
"$simulator" "$case_dir/dual-list" --no-budgeted-hash-experiment \
    > "$case_dir/dual-report"
grep -Eq '^  selected policy: +mixed SINGLE/DUAL$' "$case_dir/dual-report"
grep -Eq '^  avg strcmp per successful lookup: +1.2500$' "$case_dir/dual-report"
grep -Eq '^  successful lookup comparisons: +80$' "$case_dir/dual-report"
grep -Eq '^  lookups with one comparison: +48 / 64$' "$case_dir/dual-report"
grep -Eq '^  avg strcmp per unsuccessful lookup: +1.1277$' "$case_dir/dual-report"

sed '1!G;h;$!d' "$case_dir/dual-list" > "$case_dir/reversed-list"
"$simulator" "$case_dir/reversed-list" --no-budgeted-hash-experiment \
    > "$case_dir/reversed-report"
tail -n +2 "$case_dir/dual-report" > "$case_dir/dual-body"
tail -n +2 "$case_dir/reversed-report" > "$case_dir/reversed-body"
cmp "$case_dir/dual-body" "$case_dir/reversed-body"

# END File: units/test-femtofsSim.sh
