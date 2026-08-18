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

| Fedora | Debian / Ubuntu | Arch |
| --- | --- | --- |
| `gcc-c++` `make` `pkgconf` `systemd-devel` `libfdisk-devel` `libmount-devel` `wimlib-devel` `dosfstools` `ntfsprogs` | `g++` `make` `pkg-config` `libudev-dev` `libfdisk-dev` `libmount-dev` `libwim-dev` `dosfstools` `ntfs-3g` | `gcc` `make` `pkgconf` `util-linux` `wimlib` `dosfstools` `ntfs-3g` |

```bash
make
sudo make install   # optional; also installs res/uefi-ntfs.img
```

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
