#!/bin/bash
# Cross-builds ScarlettX for Windows x64 on a Mac, with a checksum-pinned LLVM-MinGW toolchain.
#
#   tools/macos_cross_build.sh bootstrap   download and verify the toolchain (once)
#   tools/macos_cross_build.sh build       configure, build, and write the CTest inventory
#   tools/macos_cross_build.sh all         both
#
# Output: build/macos-win64/ (EXEs, shim DLLs, packages/) and build/macos-win64/ctest-inventory.json,
# ready for tools/sx_regress.py:
#
#   tools/sx_regress.py regress --build build/macos-win64 \
#       --inventory build/macos-win64/ctest-inventory.json --out out/regress
#
# This is the quick local route. The MSVC build on a Windows runner stays the reference build:
# this one uses Clang with the MinGW runtime (UCRT, statically linked C++ runtime), so its PE
# layout and import counts differ from the MSVC build's.
#
# SCARLETTX_BUILD_DIR overrides the build folder, SCARLETTX_TOOLCHAIN_DIR the toolchain cache.
# Set SCARLETTX_VICTORY_SHADER_BRIDGE=ON for the GPL research bridge; it is
# deliberately disabled for the normal portable runner artifact.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build_dir="${SCARLETTX_BUILD_DIR:-$root/build/macos-win64}"
cache="${SCARLETTX_TOOLCHAIN_DIR:-$root/build/toolchains}"

# https://github.com/mstorsjo/llvm-mingw/releases/tag/20261117
toolchain_name=llvm-mingw-20261117-ucrt-macos-universal
toolchain_url="https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/$toolchain_name.tar.xz"
toolchain_sha256=98d340bbb8d46b09ad6886ffba88bd9d5415e131c316de48e6bd7647377446aa
toolchain="$cache/$toolchain_name"

die() { printf 'macos_cross_build: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null || die "missing $1 (install it first)"; }

check_sha256() {
    local got
    got=$(shasum -a 256 "$1" | cut -d' ' -f1)
    [[ "$got" == "$2" ]] || die "SHA-256 mismatch for $1: got $got, expected $2"
}

bootstrap() {
    need curl; need shasum; need tar
    mkdir -p "$cache"
    local archive="$cache/$toolchain_name.tar.xz"
    if [[ ! -f "$archive" ]]; then
        curl --fail --location --retry 3 --output "$archive.download" "$toolchain_url"
        check_sha256 "$archive.download" "$toolchain_sha256"
        mv "$archive.download" "$archive"
    fi
    check_sha256 "$archive" "$toolchain_sha256"
    if [[ ! -x "$toolchain/bin/x86_64-w64-mingw32-clang++" ]]; then
        tar -xf "$archive" -C "$cache"
    fi
    [[ -x "$toolchain/bin/x86_64-w64-mingw32-clang++" ]] || die "toolchain did not unpack to $toolchain"
    printf 'toolchain ready: %s\n' "$toolchain"
}

build() {
    need cmake; need ninja
    [[ -x "$toolchain/bin/x86_64-w64-mingw32-clang++" ]] || die "toolchain missing; run: $0 bootstrap"
    export PATH="$toolchain/bin:$PATH"
    cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
        -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ \
        -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
        -DSCARLETTX_VICTORY_SHADER_BRIDGE="${SCARLETTX_VICTORY_SHADER_BRIDGE:-OFF}"
    cmake --build "$build_dir" --parallel
    ctest --test-dir "$build_dir" --show-only=json-v1 > "$build_dir/ctest-inventory.json"
    {
        printf 'commit=%s\n' "$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)"
        printf 'dirty_files=%s\n' "$(git -C "$root" status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
        printf 'toolchain=%s sha256=%s\n' "$toolchain_name" "$toolchain_sha256"
        printf 'compiler=%s\n' "$(x86_64-w64-mingw32-clang++ --version | head -1)"
        printf 'cmake=%s ninja=%s\n' "$(cmake --version | head -1)" "$(ninja --version)"
        printf 'victory_shader_bridge=%s\n' "${SCARLETTX_VICTORY_SHADER_BRIDGE:-OFF}"
        if [[ "${SCARLETTX_VICTORY_SHADER_BRIDGE:-OFF}" == ON ]]; then
            printf 'victory_gitlink=%s\n' "$(git -C "$root" rev-parse HEAD:third_party/victory 2>/dev/null || echo unknown)"
            printf 'victory_checkout=%s dirty_files=%s\n' \
                "$(git -C "$root/third_party/victory" rev-parse HEAD 2>/dev/null || echo unknown)" \
                "$(git -C "$root/third_party/victory" status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
            printf 'bridge_cargo_lock_sha256=%s\n' \
                "$(shasum -a 256 "$root/third_party/scarlettx-victory-bridge/Cargo.lock" | cut -d' ' -f1)"
            printf 'rustc=%s\n' "$(cd "$root/third_party/scarlettx-victory-bridge" && rustc --version)"
            printf 'cargo=%s\n' "$(cd "$root/third_party/scarlettx-victory-bridge" && cargo --version)"
        fi
    } > "$build_dir/build-info.txt"
    cat "$build_dir/build-info.txt"
    printf 'built: %s\n' "$build_dir"
}

case "${1:-help}" in
    bootstrap) bootstrap ;;
    build) build ;;
    all) bootstrap; build ;;
    *) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//' ;;
esac
