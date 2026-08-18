# ezwin

A native Linux terminal tool that writes a **UEFI-bootable Windows 11 USB installer** from the hybrid `.iso` files Microsoft publishes.

`dd`, Etcher-style raw flash, and similar tools fail here for two separate reasons:

1. **Hybrid layout.** Windows ISOs are UDF/ISO9660 images with a hybrid MBR meant for optical media. Writing the image byte-for-byte to a USB stick does not produce a disk that firmware will boot as a hard drive (no GPT, no EFI System partition).
2. **The 4 GiB FAT32 limit.** UEFI firmware is required to read FAT32. `sources/install.wim` on current Windows 11 ISOs is larger than 4 GiB, so it cannot live on FAT32 as a single file.

ezwin does what Microsoft’s own media tooling does, without leaving Linux: GPT + a single FAT32 EFI System partition, copy the installer files, and **split** an oversized `install.wim` / `install.esd` into `install.swm` parts that Windows Setup understands natively. That path stays Secure Boot friendly (the Microsoft-signed `bootx64.efi` from the ISO is used as-is).

## Build

Dependencies:

| Fedora | Debian / Ubuntu | Arch |
| --- | --- | --- |
| `gcc-c++` `make` `pkgconf` `systemd-devel` `libfdisk-devel` `libmount-devel` `wimlib-devel` `dosfstools` | `g++` `make` `pkg-config` `libudev-dev` `libfdisk-dev` `libmount-dev` `libwim-dev` `dosfstools` | `gcc` `make` `pkgconf` `util-linux` `wimlib` `dosfstools` |

```bash
make
sudo make install   # optional, PREFIX=/usr/local
```

## Usage

```bash
# list USB disks
ezwin list

# inspect an ISO and device without writing
sudo ezwin flash --dry-run Win11.iso /dev/sdb

# write the installer (will prompt you to type the device path)
sudo ezwin flash Win11.iso /dev/sdb

# scripts
sudo ezwin flash -y Win11.iso /dev/sdb
```

`ezwin Win11.iso /dev/sdb` is accepted as shorthand for `flash`.

You can pass a stable by-id path:

```bash
sudo ezwin flash Win11.iso /dev/disk/by-id/usb-SanDisk_...
```

Non-USB targets are refused unless you pass `--force`.

## What it writes

| Piece | Choice |
| --- | --- |
| Partition table | GPT |
| Partition | One disk-sized EFI System partition |
| Filesystem | FAT32 (`mkfs.fat`) |
| Oversized WIM/ESD | `wimlib` split to `sources/install.swm` (+ `install2.swm`, …) |

BIOS/CSM boot is not a goal. Windows 11 wants UEFI.

## Status

Private early tree. Treat it as a sharp disk tool: it **erases** the target device.
