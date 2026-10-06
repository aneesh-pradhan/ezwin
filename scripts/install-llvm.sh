#!/usr/bin/env bash
# Install the official release locally; never change the host's system compiler.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=$(<"$repo/.llvm-version")
[[ $(uname -s) == Linux ]] || { echo 'ezwin requires Linux.' >&2; exit 1; }
case $(uname -m) in
    x86_64) platform=X64 ;;
    aarch64|arm64) platform=ARM64 ;;
    *) echo 'No pinned LLVM binary for this architecture.' >&2; exit 1 ;;
esac
archive="LLVM-$version-Linux-$platform.tar.xz"
checksum=$(awk -v archive="$archive" '$2 == archive { print $1 }' "$repo/toolchains/SHA256SUMS")
[[ $checksum =~ ^[0-9a-f]{64}$ ]] || { echo "Missing checksum for $archive" >&2; exit 1; }
destination="$repo/.toolchains/llvm-$version"
mkdir -p "$repo/.toolchains"
staging=$(mktemp -d "$repo/.toolchains/.install-XXXXXXXX")
trap 'rm -rf -- "$staging"' EXIT
if [[ ! -e $destination ]]; then
    echo "Downloading $archive (approximately 2 GB; about 12 GB unpacked)..."
    curl --fail --location --retry 3 --output "$staging/$archive" \
        "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$archive"
    (cd "$staging" && printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status)
    mkdir "$staging/toolchain"
    tar -xJf "$staging/$archive" --strip-components=1 -C "$staging/toolchain"
    "$staging/toolchain/bin/clang++" --version
    mv -- "$staging/toolchain" "$destination"
fi

# The official X64 LLD binary depends on Ubuntu 22.04's ICU 70 ABI. Keep
# compatibility libraries local, including their license; never install a .deb.
if [[ $platform == X64 && ! -d $destination/host-libs ]]; then
    icu_archive=libicu70_70.1-2_amd64.deb
    icu_checksum=$(awk -v archive="$icu_archive" '$2 == archive { print $1 }' "$repo/toolchains/SHA256SUMS")
    [[ $icu_checksum =~ ^[0-9a-f]{64}$ ]] || { echo 'Missing ICU checksum.' >&2; exit 1; }
    curl --fail --location --retry 3 --output "$staging/$icu_archive" \
        "https://archive.ubuntu.com/ubuntu/pool/main/i/icu/$icu_archive"
    (cd "$staging" && printf '%s  %s\n' "$icu_checksum" "$icu_archive" | sha256sum --check --status)
    (cd "$staging" && "$destination/bin/llvm-ar" x "$icu_archive")
    mkdir "$staging/host-libs"
    tar -xf "$staging"/data.tar.* -C "$staging/host-libs"
    mv -- "$staging/host-libs" "$destination/host-libs"
fi
echo "Installed LLVM $version in $destination"
