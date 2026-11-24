# ezwin

[![LLVM build](https://github.com/aneesh-pradhan/ezwin/actions/workflows/build.yml/badge.svg)](https://github.com/aneesh-pradhan/ezwin/actions/workflows/build.yml)

ezwin is a native Linux terminal tool that writes a UEFI-bootable Windows 11 installer onto a USB drive from the hybrid `.iso` files Microsoft publishes.

## Why

Copying that ISO with `dd` does not produce a stick firmware will boot. Microsoft’s images are UDF hybrids, not raw disk images, and FAT32 cannot hold a `sources/install.wim` larger than 4 GiB. ezwin creates the layout Microsoft’s Media Creation Tool uses, documented from a working stick in [`docs/microsoft-mct-usb.md`](docs/microsoft-mct-usb.md):

- **MBR**, one primary partition, type `0x0c` (FAT32 LBA), **active**, starting at **1 MiB**
- **32 GiB FAT32** on USBs larger than that (Windows’ FAT32 cap); the rest of the stick is left empty
- Volume label **`ESD-USB`**
- Oversized `install.wim` split to `install.swm` / `install2.swm` at 3800 MiB
- Firmware boots `\EFI\BOOT\bootx64.efi` from that same volume — no second partition, no UEFI:NTFS stub

ezwin wipes GPT leftovers more thoroughly than MCT did on the inspected stick. It does **not** copy Microsoft `bootsect` MBR/VBR code; UEFI loads the EFI bootloader from the filesystem the same way MCT does.

Rufus-style GPT+NTFS and WoeUSB-style NTFS+UEFI:NTFS remain available as `--layout ntfs` and `--layout mbr`.

## Warnings

**Flashing a USB drive destroys every partition and file on the target device.** `ezwin flash` repartitions and formats the whole disk. If several drives are plugged in, or if you pass the wrong path, this can erase an internal disk. Run `ezwin list`, then `sudo ezwin flash --dry-run Win11.iso`, and read the device path before typing `yes`. There is no undo.

**The first build downloads a pinned LLVM 23.1.2 toolchain of about 2 GB** (the archive is 1920 MiB compressed and about 12 GB unpacked). Allow about 14 GB of free space while it installs. `scripts/install-llvm.sh` checks the archive against [`toolchains/SHA256SUMS`](toolchains/SHA256SUMS) and extracts it into `.toolchains/llvm-23.1.2`. It does not replace the system compiler. On x86-64 it also downloads Ubuntu’s ICU 70 compatibility libraries for the official LLD binary and keeps them inside that directory.

## Demo

Captured on Ubuntu 24.04.5 (x86-64) after the Debian install commands above, `make toolchain`, and `make test`. No USB device was written. `list` prints the block devices udev exposes on the machine that ran it; this host had no USB disk.

```text
$ ctest --preset llvm
Test project /workspace/build/llvm
    Start 1: cli-help
1/3 Test #1: cli-help .........................   Passed    0.00 sec
    Start 2: cli-version
2/3 Test #2: cli-version ......................   Passed    0.00 sec
    Start 3: llvm-runtime
3/3 Test #3: llvm-runtime .....................   Passed    0.00 sec

100% tests passed, 0 tests failed out of 3

Total Test time (real) =   0.01 sec

$ python3 tests/test_toolchain.py
.........
----------------------------------------------------------------------
Ran 9 tests in 1.272s

OK

$ ./ezwin --version
ezwin 0.5.0

$ ./ezwin list
DEVICE          SIZE      BUS     USB     MODEL
(no USB disks found — plug one in, or pass --all)

$ ./ezwin list --all
DEVICE          SIZE      BUS     USB     MODEL
/dev/vda        256.0 GiB -       no
/dev/vdb        256.0 GiB -       no
/dev/vdc        256.0 GiB -       no
```

Configure on that run found `wimlib` 1.14.4 once `ntfs-3g-dev` and `libfuse-dev` were installed (`-- Found wimlib, version 1.14.4-1.1build2`).

## Prerequisites

The project needs **CMake 3.30 or newer**, Ninja, GNU Make, Bash, curl, tar, xz, zstd, pkg-config, Python 3, and `readelf` (from binutils). The C++ compiler is the pinned LLVM Clang. Linux’s C library and the system libraries below are still required. Clang implements a subset of C++26; turning the mode on does not mean every C++26 feature is available.

`libwim-dev` / `wimlib-devel` ships a `wimlib.pc` with `Requires.private: libntfs-3g fuse`. CMake’s pkg-config check fails unless those two modules are installed. On Debian and Ubuntu they come from **`ntfs-3g-dev`** and **`libfuse-dev`** (FUSE 2). `libfuse3-dev` provides `fuse3.pc` and does **not** satisfy `wimlib.pc`.

### Debian and Ubuntu

Verified on Ubuntu 24.04.5. Ubuntu 24.04’s apt CMake is 3.28.3, which is too old, so CMake 4.3.0 is installed with pip (the same version [CI](.github/workflows/build.yml) uses). `--break-system-packages` is required on stock Ubuntu, where the system Python is externally managed; the flag is accepted when that marker is absent.

Build and test:

```bash
sudo apt-get update
sudo apt-get install -y \
  make ninja-build pkg-config curl ca-certificates xz-utils zstd \
  python3 python3-pip binutils \
  libc6-dev libstdc++6 zlib1g liblzma5 \
  libudev-dev libfdisk-dev libmount-dev libwim-dev \
  ntfs-3g-dev libfuse-dev
python3 -m pip install --user --break-system-packages 'cmake==4.3.0'
export PATH="$HOME/.local/bin:$PATH"
cmake --version   # 4.3.0, or any CMake >= 3.30
```

Tools used only when flashing a stick (`make` and `make test` do not need them):

```bash
sudo apt-get install -y dosfstools ntfs-3g util-linux parted udev
```

`ntfs-3g-dev` already depends on `ntfs-3g` (`mkfs.ntfs` and the `ntfs-3g` mount helper). `dosfstools` provides `mkfs.fat`. `util-linux` provides `wipefs` and `partx`. `parted` provides `partprobe` (ezwin falls back to `partx` when `partprobe` is missing). `udev` provides `udevadm`.

### Fedora

Package names below match Fedora’s published provides: `wimlib-devel` requires `pkgconfig(libntfs-3g)` and `pkgconfig(fuse)`, which `ntfs-3g-devel` and `fuse-devel` (FUSE 2) provide. Confirm `cmake --version` is 3.30 or newer before building.

```bash
sudo dnf install -y \
  cmake ninja-build make pkgconf \
  glibc-devel libstdc++ zlib xz xz-libs curl zstd \
  systemd-devel libfdisk-devel libmount-devel \
  wimlib-devel ntfs-3g-devel fuse-devel \
  dosfstools ntfsprogs util-linux parted \
  python3 binutils
```

### Arch Linux

Arch’s `wimlib` 1.14.5 `.pc` file has the same `Requires.private: libntfs-3g fuse` line. `libntfs-3g` ships `libntfs-3g.pc`. The module named `fuse` is FUSE 2, from `fuse2` (`fuse.pc`). `fuse3` does not provide it, even though the `wimlib` shared library itself links `libfuse3`.

```bash
sudo pacman -S --needed \
  cmake ninja make pkgconf \
  glibc gcc-libs zlib xz curl zstd \
  systemd util-linux \
  wimlib libntfs-3g fuse2 \
  dosfstools ntfsprogs parted \
  python binutils
```

## Build, test, and run

From a clean clone, after the install command for your distribution:

```bash
git clone https://github.com/aneesh-pradhan/ezwin.git
cd ezwin
export PATH="$HOME/.local/bin:$PATH"   # Debian/Ubuntu pip CMake
make toolchain    # downloads and verifies the ~2 GB LLVM archive
make
make test
python3 tests/test_toolchain.py
./ezwin --version
./ezwin --help
./ezwin list
```

`make test` runs help, version, and an LLVM runtime smoke test. It does not open a disk device. `tests/test_toolchain.py` checks the linked runtimes and rejects unsupported compilers. GCC rejection checks run only when `g++` is already installed. The script needs Python 3, `ldd`, and `readelf`.

Direct CMake/Ninja, which is what Make wraps:

```bash
cmake --preset llvm
cmake --build --preset llvm    # or: ninja -C build/llvm
ctest --preset llvm
./build/llvm/ezwin --version
```

`make` also copies the executable to `./ezwin`. Build products and the downloaded toolchain are gitignored. The toolchain policy, presets, and checksums are tracked. Ninja regenerates when the command line changes. After changing optimization flags in the environment, reconfigure with `cmake --fresh --preset llvm`. Toolchain-selection overrides are rejected. `PREFIX` and `DESTDIR` work for installation; build before `sudo make install`. `make uninstall` removes the installed files.

```bash
sudo make install
ezwin --version
```

The executable finds the pinned LLVM shared runtimes through an embedded runtime search path. Keep that LLVM installation in place after installing ezwin. For a system install, point `LLVM_ROOT` at a stable, administrator-owned tree such as `/opt/llvm-23.1.2` and rebuild before installing:

```bash
cmake --preset llvm -DLLVM_ROOT=/opt/llvm-23.1.2
```

Moving the binary to another machine requires those runtimes at the configured path, or a separate packaging step. The compiler executables use the host libstdc++. ezwin itself is compiled and linked against LLVM’s libc++, libc++abi, compiler-rt, and libunwind. System C libraries (for example libudev) may load `libgcc_s` transitively. The ezwin executable does not directly link libgcc or libstdc++.

[GitHub CI](.github/workflows/build.yml) installs the Debian build packages above, including `ntfs-3g-dev` and `libfuse-dev`, then installs the checksum-pinned toolchain, builds through Make, runs the smoke tests, checks direct Ninja use, validates the linked runtimes, and tests rejection of unsupported toolchains. The **LLVM / C++26** job is the status check named on the badge above.

To upgrade LLVM, update `.llvm-version`, replace the archive checksums with those from the [official release](https://github.com/llvm/llvm-project/releases), update the version references here, install the new toolchain, and configure a fresh build. Versions stay pinned on purpose.

## Usage

`flash` erases the target USB drive. Start with `list` and `--dry-run`.

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

If several USB disks are plugged in, pass the device path from `ezwin list`.

## Layouts

| `--layout` | What it writes |
| --- | --- |
| `fat32` (default) | MBR · one FAT32 partition (32 GiB on large USBs, `ESD-USB`) · oversized WIM split to `install.swm` |
| `ntfs` | GPT · NTFS (full ISO, first partition) · 1 MiB UEFI:NTFS stub (Basic Data, no drive letter) |
| `mbr` | MBR · NTFS first · 1 MiB UEFI:NTFS stub (WoeUSB-style; pick the UEFI boot entry) |
| `dual` | MBR · FAT32 boot · NTFS payload |

`res/uefi-ntfs.img` comes from [Rufus](https://github.com/pbatard/rufus) / [UEFI:NTFS](https://github.com/pbatard/uefi-ntfs); see `third_party/uefi-ntfs/NOTICE`. Needed only for `--layout ntfs` / `mbr`.

## License

[MIT](LICENSE). Copyright (c) 2026 Aneesh Pradhan.
