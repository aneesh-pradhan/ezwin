# ezwin

A native Linux terminal tool that writes a **UEFI-bootable Windows 11 USB installer** from the hybrid `.iso` files Microsoft publishes.

`dd` fails here because Windows ISOs are UDF hybrids, not disk images. FAT32 also cannot hold a >4 GiB `install.wim`.

The default layout follows **Microsoft’s Media Creation Tool**, documented from a working stick in [`docs/microsoft-mct-usb.md`](docs/microsoft-mct-usb.md):

- **MBR**, one primary partition, type `0x0c` (FAT32 LBA), **active**, starting at **1 MiB**
- **32 GiB FAT32** on USBs larger than that (Windows’ FAT32 cap); the rest of the stick is left empty
- Volume label **`ESD-USB`**
- Oversized `install.wim` split to `install.swm` / `install2.swm` at 3800 MiB
- Firmware boots `\EFI\BOOT\bootx64.efi` from that same volume — no second partition, no UEFI:NTFS stub

ezwin wipes GPT leftovers more thoroughly than MCT did on the inspected stick. It does **not** copy Microsoft `bootsect` MBR/VBR code; UEFI loads the EFI bootloader from the filesystem the same way MCT does.

Rufus-style GPT+NTFS and WoeUSB-style NTFS+UEFI:NTFS remain available as `--layout ntfs` and `--layout mbr`.

## Build

The required toolchain is **LLVM 23.1.2**, pinned in [`.llvm-version`](.llvm-version):
Clang with **ISO C++26** (`-std=c++2c` / `-std=c++26`), LLD, libc++, libc++abi,
compiler-rt and LLVM libunwind. LLVM's archiver and binary utilities are pinned too.
GCC, libstdc++, GNU linkers, other LLVM versions and older C++ modes are rejected.
Clang implements a subset of C++26; enabling the mode does not imply support for
every C++26 feature. Linux's C library and the system libraries below remain required.

Install **CMake 3.30+**, Ninja, Make, Bash, curl, tar, xz and zstd, plus these dependencies:

| Fedora | Debian / Ubuntu | Arch |
| --- | --- | --- |
| `cmake` `ninja-build` `make` `pkgconf` `glibc-devel` `libstdc++` `zlib` `xz-libs` `systemd-devel` `libfdisk-devel` `libmount-devel` `wimlib-devel` `dosfstools` `ntfsprogs` | `cmake` (3.30+) `ninja-build` `make` `pkg-config` `libc6-dev` `libstdc++6` `zlib1g` `liblzma5` `libudev-dev` `libfdisk-dev` `libmount-dev` `libwim-dev` `dosfstools` `ntfs-3g` | `cmake` `ninja` `make` `pkgconf` `glibc` `gcc-libs` `zlib` `xz` `systemd` `util-linux` `wimlib` `dosfstools` `ntfs-3g` |

```bash
make toolchain      # download and verify the pinned LLVM release (~2 GB archive)
make
make test          # help/version and LLVM runtime smoke tests; no device writes
sudo make install  # install the already-built program and res/uefi-ntfs.img
```

The installer supports Linux x86-64 and AArch64 and verifies official archives
against committed SHA-256 checksums in [`toolchains/SHA256SUMS`](toolchains/SHA256SUMS).
It installs into `.toolchains/llvm-23.1.2` without changing system compilers.
Allow about 14 GB of free space during installation (12 GB after extraction).
The x86-64 archive's LLD requires ICU 70; the installer also verifies and extracts
Ubuntu's ICU compatibility libraries locally, with their license. Build launchers
expose those libraries only to the LLVM tools. The compiler executables themselves
use the host's libstdc++ runtime; ezwin is compiled and linked against LLVM runtimes.
System C dependencies (for example Fedora's libudev) can also load libgcc_s
transitively. The ezwin executable does not directly link libgcc or libstdc++.
An existing complete installation of the exact same release can be selected using
`cmake --preset llvm -DLLVM_ROOT=/absolute/path/to/llvm-23.1.2`.

Make is a convenience wrapper around the shared CMake/Ninja build. Direct use:

```bash
cmake --preset llvm
cmake --build --preset llvm   # or: ninja -C build/llvm
ctest --preset llvm
./build/llvm/ezwin --version
```

`make` also copies the executable to `./ezwin`. Build products and downloaded
toolchains are ignored by Git; the toolchain policy, presets and checksums are tracked.
Ninja tracks command-line changes and regenerates when build configuration changes.
After changing environment optimization flags, use `cmake --fresh --preset llvm`;
CMake caches those flags at initial configuration. Toolchain-selection overrides
are not supported. `PREFIX` and `DESTDIR` are supported for installation; build
before running `sudo make install`. `make uninstall` removes the installed files.

The executable uses the pinned LLVM shared runtimes through an embedded runtime
search path. Keep that LLVM installation available after installing ezwin; for a
system installation, use a stable, administrator-owned `LLVM_ROOT` (such as
`/opt/llvm-23.1.2`) and rebuild before installing. Moving binaries to another machine
requires those runtimes at the configured path or a separate packaging step.

[GitHub CI](.github/workflows/build.yml) uses the same checksum-pinned installation,
builds through Make, checks direct Ninja use, runs safe smoke tests, validates the
linked runtimes and tests rejection of unsupported toolchains. Run the policy checks
locally with `python3 tests/test_toolchain.py` after building (requires Python 3,
`ldd` and `readelf`; GCC rejection checks run when GCC is already installed).
Configure the **LLVM / C++26** status check as required in the GitHub branch rules
to enforce it before merging; committing a workflow alone does not enable that rule.

To upgrade LLVM, update `.llvm-version`, replace the archive checksums with those
from the [official release](https://github.com/llvm/llvm-project/releases), update
the version references here, install the new toolchain, and configure a fresh build.
Versions are deliberately pinned instead of following a moving `latest` download.

## Usage

```bash
ezwin list
sudo ezwin flash --dry-run Win11.iso            # uses the only plugged-in USB
sudo ezwin flash Win11.iso                      # same, then type yes to erase
sudo ezwin flash Win11.iso /dev/sdb

sudo ezwin flash --layout fat32 Win11.iso       # default (Microsoft USB tool)
sudo ezwin flash --layout ntfs Win11.iso        # Rufus-style GPT + UEFI:NTFS
sudo ezwin flash --layout mbr Win11.iso /dev/sdb   # WoeUSB-style MBR
sudo ezwin flash --layout dual Win11.iso /dev/sdb
```

If several USB disks are plugged in, pass the device path (`ezwin list`).

## Layouts

| `--layout` | What it writes |
| --- | --- |
| `fat32` (default) | MBR · one FAT32 partition (32 GiB on large USBs, `ESD-USB`) · oversized WIM split to `install.swm` |
| `ntfs` | GPT · NTFS (full ISO, first partition) · 1 MiB UEFI:NTFS stub (Basic Data, no drive letter) |
| `mbr` | MBR · NTFS first · 1 MiB UEFI:NTFS stub (WoeUSB-style; pick the UEFI boot entry) |
| `dual` | MBR · FAT32 boot · NTFS payload |

`res/uefi-ntfs.img` comes from [Rufus](https://github.com/pbatard/rufus) / [UEFI:NTFS](https://github.com/pbatard/uefi-ntfs); see `third_party/uefi-ntfs/NOTICE`. Needed only for `--layout ntfs` / `mbr`.

## Status

Private early tree. Treat it as a sharp disk tool: it **erases** the target device.
