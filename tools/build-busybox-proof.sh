#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    printf 'usage: %s OUTPUT_DIRECTORY WORK_DIRECTORY\n' "$0" >&2
    exit 2
fi

output_dir=$(realpath -m "$1")
work_dir=$(realpath -m "$2")
repository_root=$(git rev-parse --show-toplevel)

busybox_version=1.38.0
busybox_archive="busybox-${busybox_version}.tar.bz2"
busybox_url="https://busybox.net/downloads/${busybox_archive}"
busybox_sha256=34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2
musl_version=1.2.6
musl_archive="musl-${musl_version}.tar.gz"
musl_upstream_url="https://musl.libc.org/releases/${musl_archive}"
musl_url="https://sources.buildroot.net/musl/${musl_archive}"
musl_sha256=d585fd3b613c66151fc3249e8ed44f77020cb5e6c1e635a616d3f9f82460512a

rm -rf "$output_dir" "$work_dir"
mkdir -p "$output_dir" "$work_dir/downloads" "$work_dir/source" \
    "$work_dir/musl-build" "$work_dir/musl-install"

curl --fail --location --proto '=https' --tlsv1.2 \
    "$busybox_url" -o "$work_dir/downloads/$busybox_archive"
curl --fail --location --proto '=https' --tlsv1.2 \
    "$musl_url" -o "$work_dir/downloads/$musl_archive"

printf '%s  %s\n' "$busybox_sha256" \
    "$work_dir/downloads/$busybox_archive" | sha256sum --check --strict
printf '%s  %s\n' "$musl_sha256" \
    "$work_dir/downloads/$musl_archive" | sha256sum --check --strict

tar --extract --bzip2 --file "$work_dir/downloads/$busybox_archive" \
    --directory "$work_dir/source" --no-same-owner --no-same-permissions
tar --extract --gzip --file "$work_dir/downloads/$musl_archive" \
    --directory "$work_dir/source" --no-same-owner --no-same-permissions

musl_source="$work_dir/source/musl-${musl_version}"
busybox_source="$work_dir/source/busybox-${busybox_version}"
musl_cflags='-Os -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-tree-vectorize -fno-ident -Wno-return-local-addr'

(
    cd "$work_dir/musl-build"
    "$musl_source/configure" \
        --prefix="$work_dir/musl-install" \
        --disable-shared \
        CC=gcc \
        CFLAGS="$musl_cflags"
    make --jobs=2
    make install
)

cp "$repository_root/userspace/busybox/busybox-miniconfig" \
    "$work_dir/busybox-miniconfig"
(
    cd "$busybox_source"
    make allnoconfig
    while IFS= read -r setting; do
        case "$setting" in
            CONFIG_*=n)
                symbol=${setting%%=*}
                replacement="# $symbol is not set"
                ;;
            CONFIG_*=*)
                symbol=${setting%%=*}
                replacement=$setting
                ;;
            *)
                printf 'invalid BusyBox miniconfig line: %s\n' "$setting" >&2
                exit 1
                ;;
        esac
        if grep -Eq "^${symbol}=|^# ${symbol} is not set$" .config; then
            sed -i \
                -e "s|^${symbol}=.*$|${replacement}|" \
                -e "s|^# ${symbol} is not set$|${replacement}|" \
                .config
        else
            printf '%s\n' "$replacement" >>.config
        fi
    done <"$work_dir/busybox-miniconfig"
    make silentoldconfig
    test "$(grep -Ec '^CONFIG_[A-Z0-9_]+=y$' .config)" -ge 1
    grep -Fxq 'CONFIG_BUSYBOX=y' .config
    grep -Fxq 'CONFIG_STATIC=y' .config
    grep -Fxq 'CONFIG_ECHO=y' .config
    grep -Fxq '# CONFIG_FEATURE_FANCY_ECHO is not set' .config
    make --jobs=2 \
        CC="$work_dir/musl-install/bin/musl-gcc" \
        HOSTCFLAGS='-Wno-unused-result'
)

busybox_binary="$busybox_source/busybox"
cp "$busybox_binary" "$output_dir/busybox"
cp "$busybox_source/.config" "$output_dir/busybox.config"
cp "$busybox_source/LICENSE" "$output_dir/BUSYBOX-LICENSE"
cp "$musl_source/COPYRIGHT" "$output_dir/MUSL-COPYRIGHT"
cp "$work_dir/downloads/$busybox_archive" "$output_dir/$busybox_archive"
cp "$work_dir/downloads/$musl_archive" "$output_dir/$musl_archive"

busybox_size=$(stat --format=%s "$output_dir/busybox")
test "$busybox_size" -le $((2 * 1024 * 1024))
test $(((busybox_size + 4095) / 4096)) -le 512
test "$(readelf -W -h "$output_dir/busybox" | awk '/Type:/{print $2}')" = EXEC
test "$(readelf -W -h "$output_dir/busybox" | awk '/Number of program headers:/{print $5}')" -le 8
test "$(readelf -W -l "$output_dir/busybox" | grep -Ec '^[[:space:]]+LOAD')" -le 4
! readelf -W -l "$output_dir/busybox" | grep -Eq \
    '^[[:space:]]+(INTERP|DYNAMIC)|LOAD[[:space:]].*RWE'
! readelf -W -r "$output_dir/busybox" | grep -Eq 'R_X86_64_'
! readelf -W -d "$output_dir/busybox" | grep -Eq '\(NEEDED\)|\(TEXTREL\)'

readelf -W -h "$output_dir/busybox" >"$output_dir/elf-header.txt"
readelf -W -l "$output_dir/busybox" >"$output_dir/elf-program-headers.txt"
readelf -W -r "$output_dir/busybox" >"$output_dir/elf-relocations.txt"
readelf -W -d "$output_dir/busybox" >"$output_dir/elf-dynamic.txt" || true
objdump -d --no-show-raw-insn "$output_dir/busybox" \
    >"$output_dir/busybox-disassembly.txt"
forbidden_instructions=$(grep -Ei \
    '%(xmm|ymm|zmm|mm|k)[0-9]+|^[[:space:]]*[0-9a-f]+:[[:space:]]+(f[a-z0-9]+|emms|fxsave|fxrstor|ldmxcsr|stmxcsr|v[a-z0-9]+)([[:space:]]|$)' \
    "$output_dir/busybox-disassembly.txt" \
    | grep -Ev '[[:space:]](verr|verw)[[:space:]]' || true)
test -z "$forbidden_instructions" || {
    printf 'BusyBox loaded text contains floating-point or vector instructions:\n%s\n' \
        "$forbidden_instructions" >&2
    exit 1
}
file "$output_dir/busybox" >"$output_dir/file.txt"
sha256sum "$output_dir/busybox" "$output_dir/busybox.config" \
    "$output_dir/$busybox_archive" "$output_dir/$musl_archive" \
    "$output_dir/BUSYBOX-LICENSE" "$output_dir/MUSL-COPYRIGHT" \
    >"$output_dir/SHA256SUMS"

stdout_file="$output_dir/stdout.txt"
stderr_file="$output_dir/stderr.txt"
trace_file="$output_dir/syscall-trace.txt"
env -i strace --argv0=busybox --quiet=all --string-limit=256 \
    --output="$trace_file" "$output_dir/busybox" echo SAPOTE \
    >"$stdout_file" 2>"$stderr_file"
printf 'SAPOTE\n' | cmp --silent - "$stdout_file"
test ! -s "$stderr_file"

{
    printf 'BusyBox version: %s\n' "$busybox_version"
    printf 'BusyBox source SHA-256: %s\n' "$busybox_sha256"
    printf 'musl version: %s\n' "$musl_version"
    printf 'musl upstream URL: %s\n' "$musl_upstream_url"
    printf 'musl byte-identical mirror URL: %s\n' "$musl_url"
    printf 'musl source SHA-256: %s\n' "$musl_sha256"
    printf 'host gcc: %s\n' "$(gcc --version | head -n 1)"
    printf 'host binutils: %s\n' "$(ld --version | head -n 1)"
    printf 'binary bytes: %s\n' "$busybox_size"
    printf 'FAT16 data clusters at 4096 bytes: %s\n' \
        "$(((busybox_size + 4095) / 4096))"
    printf 'binary SHA-256: %s\n' \
        "$(sha256sum "$output_dir/busybox" | awk '{print toupper($1)}')"
    printf 'configuration SHA-256: %s\n' \
        "$(sha256sum "$output_dir/busybox.config" | awk '{print toupper($1)}')"
} >"$output_dir/build-record.txt"
