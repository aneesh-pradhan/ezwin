# Microsoft Media Creation Tool USB (inspected)

Captured **2026-08-17** from the Samsung Type-C stick (`/dev/sda`, 119.51 GiB, serial `0377322070002672`) after it was written with Microsoft’s Windows USB / Media Creation Tool and confirmed to boot Setup on a **desktop**. This file is that stick, not a guess from docs.

ezwin did **not** modify the stick while measuring it.

## What Microsoft actually writes

One sentence: **MBR + one 32 GiB FAT32 volume, BIOS-bootable, UEFI-bootable, `install.wim` split into LZMS `.swm` parts.**

| Piece | Value on this stick |
| --- | --- |
| Partition table Linux uses | **DOS/MBR** (`PTTYPE=dos`, disk id `0xcecb9de2`) |
| Partitions | **One** primary: type `0x0c` (W95 FAT32 LBA), **active** (`0x80`) |
| Start | LBA **2048** (1 MiB) |
| Size | **67,108,864** sectors = **32 GiB exactly** (`create partition primary size=32768`) |
| Rest of the 128 GB stick | Unpartitioned (~87.5 GiB) |
| Filesystem | FAT32, OEM `MSDOS5.0`, **32 KiB** clusters (64×512) |
| Volume label | Directory label **`ESD-USB`**; BPB label is `NO NAME` |
| UUID | `F2AF-B0E4` |
| Installer payload | `sources/boot.wim` + `sources/install.swm` + `sources/install2.swm` |
| `install.wim` | **Absent** |

That matches the usual MCT `diskpart` recipe:

```
clean
convert mbr
create partition primary size=32768
format fs=fat32 quick label=ESD-USB
active
assign
```

Windows’ inbox FAT32 formatter still caps at 32 GiB, so MCT never uses the rest of a larger USB.

## MBR bootstrap (BIOS)

`file` reports **MS-MBR (Windows 7 English)**. Bytes 0–439 are Microsoft’s real bootstrap, not zeros and not GRUB. Strings:

- `Invalid partition table`
- `Error loading operating system`
- `Missing operating system`

Partition 1 is marked **active**. The FAT32 VBR (jmp `EB 58`, OEM `MSDOS5.0`) loads **`BOOTMGR`** (`Disk error` / `Press any key to restart`). That is `bootsect /nt60` behavior.

So this stick is **intentionally BIOS-bootable** as well as UEFI-bootable. If firmware prefers “USB HDD” / CSM, Setup runs in BIOS mode (`PEFirmwareType = 0x1`). BIOS-mode Setup is what turns a GPT internal disk into a fake **2 TB** protective-MBR view. This MCT stick working on one desktop does **not** prove it is safe on a UEFI+GPT laptop whose firmware CSM-boots USB.

## UEFI boot files

FAT32 is the removable-media ESP. Firmware loads `\EFI\BOOT\bootx64.efi`.

On this stick, `\bootmgfw.efi` and `\EFI\BOOT\bootx64.efi` are **identical** (SHA-256 `490d08f9…`, 3,086,848 bytes). `\bootmgr.efi` is a different binary (3,069,432 bytes). `\bootmgr` is the BIOS boot manager (473,364 bytes).

There is **no** second partition, **no** EFI System type, **no** UEFI:NTFS stub.

## FAT geometry (Windows `format`, not `mkfs.fat`)

| BPB field | Value |
| --- | --- |
| Bytes/sector | 512 |
| Sectors/cluster | 64 (32 KiB) |
| Reserved sectors | 8196 |
| FATs | 2 |
| Sectors/FAT | 8190 |
| Hidden sectors | 2048 (matches partition start) |
| Total sectors | 67108864 |
| FS info sector | 1 |
| Backup boot sector | 6 |
| Root cluster | 2 |
| BIOS drive | `0x80` |

`mkfs.fat` on Linux typically uses ~32 reserved sectors and a different cluster choice. Functionally Setup only needs a valid FAT32; the reserved-sector count is a Windows formatter fingerprint, not a Setup requirement.

Used space: **6.5 GiB** / 32 GiB (986 files, 6,899,824,570 bytes).

## Installer payload (not a copy of the consumer ISO)

This USB is **not** the `Win11_25H2_English_x64_v2.iso` in `~/Downloads`. MCT downloaded a newer **25H2 refresh**.

| | This USB (MCT) | Local consumer ISO |
| --- | --- | --- |
| Provenance / ISO date | Build `26200.9168` (`25h2_ge_release_svc_refresh`, 2026-08-09) | Volume `CCCOMA_X64FRE_EN-US_DV9`, files 2026-03-06, CDIMAGE 2.56 |
| `boot.wim` | 609,554,347 bytes, LZX (`flags 0x40082`), 2 images | 614,809,152 bytes |
| `bootmgfw.efi` | 3,086,848 bytes | 3,008,968 bytes |
| Install image | Split **LZMS** SWM, 7 images | Single **LZX** `install.wim` 7,578,075,168 bytes |
| WebSetup stamp | `sources/ws.dat`: ClientVersion `10.0.26100.7019` | (none) |

`install.swm` / `install2.swm`:

| File | Bytes | WIM header |
| --- | --- | --- |
| `sources/install.swm` | 3,955,506,056 (~3772 MiB) | part **1**/2, 7 images, `flags 0x8008a` = compressed + spanned + RP fix + **LZMS** |
| `sources/install2.swm` | 2,060,309,718 | part **2**/2, same GUID |

Part 1 is just under ezwin’s default `--split-size 3800` MiB (3,984,588,800). Same DISM-style split Microsoft documents (`/Split-Image /FileSize:3800`).

Images in the SWM:

1. Windows 11 Home (`Core`)
2. Home N
3. Home Single Language
4. Education
5. Education N
6. Pro
7. Pro N

All `en-US`, install build **26200.9168**. WinPE/Setup in `boot.wim` reports **26100.9168** (normal: PE SKU vs client SKU).

WIM version: install SWM `0xe00`, `boot.wim` `0x10d00`.

MCT extras vs a raw ISO copy:

- `provenance/provenance.json` (supply-chain / Azure DevOps build `26200.9168.amd64fre.25h2_ge_release_svc_refresh.260809-0632.en-US`)
- `_manifest/spdx_2.2/` (also present on newer ISOs)
- `sources/product.ini` (generic/GVLK product keys and `staged=` edition list — **not** copied from the consumer ISO)
- `sources/ws.dat` (`[WebSetup]` instance id from MCT)
- `System Volume Information/` (Windows volume metadata)

Root layout otherwise looks like the ISO: `autorun.inf`, `setup.exe`, `boot/`, `efi/`, `sources/`, `support/`.

`autorun.inf` is CRLF, `[AutoRun.Amd64]` → `setup.exe`, `[AutoRun]` → `sources\SetupError.exe x64`.

## Leftover GPT from an earlier ezwin flash

This disk was previously a GPT ezwin image. MCT did **not** fully erase that.

`wipefs` still sees:

- MBR signature at `0x1fe`
- GPT header at **LBA 1** (`0x200`, `"EFI PART"`)
- Backup GPT header at the **last sector** (`0x1de0840a00`)

Linux `fdisk` still says `Disklabel type: dos` because slot 1 is type `0x0c`, not protective `0xEE`.

The GPT **partition arrays are all zeros** (primary at LBA 2 and the backup array). Header CRC therefore does not match. Firmware that trusts a valid GPT would ignore this; firmware that only sniffs `"EFI PART"` might still get confused.

`diskpart clean` is supposed to wipe partition metadata. On this stick it left both GPT headers. ezwin’s 16 MiB head/tail zeroing is stricter than what MCT did here.

A blank USB that MCT formats from factory would likely **not** have those GPT signatures. Do not treat leftover GPT as part of Microsoft’s recipe.

## How this compares to ezwin

ezwin **0.5+** defaults to this recipe (`--layout fat32` / `mct`). It matches the partition table, 32 GiB cap, `ESD-USB` label, 1 MiB start, active flag, and split-WIM file names. Differences that remain:

| | Microsoft MCT (this stick) | ezwin `--layout fat32` |
| --- | --- | --- |
| Table | MBR | MBR |
| Partitions | 1× FAT32, 32 GiB | 1× FAT32, 32 GiB (whole disk only if the installer is larger) |
| First usable | 1 MiB | 1 MiB |
| Active flag | **Yes** | Yes |
| MBR/VBR bootstrap | Microsoft `bootsect` (`BOOTMGR`) | libfdisk + `mkfs.fat` (UEFI still loads `\EFI\BOOT\bootx64.efi`) |
| FAT formatter | Windows `format` (OEM `MSDOS5.0`, 32 KiB clusters, 8196 reserved, BPB label `NO NAME`) | `mkfs.fat` with 32 KiB clusters, drive `0x80`, 255/63, 8196 reserved on 32 GiB volumes |
| `install.wim` | LZMS split `.swm` (MCT downloaded an ESD-style image) | Split of whatever the ISO contains (usually LZX) |
| GPT leftovers | MCT left invalid `"EFI PART"` headers on this stick | 16 MiB wipe of disk head and tail |

`--layout ntfs` is the older Rufus-style GPT+NTFS+UEFI:NTFS default. Use it when you want firmware **unable** to BIOS-boot the stick.

## Implications

1. **Split WIM is what Microsoft ships** on official USB media. `install.swm` + `install2.swm` on FAT32 is not a hack; it is the MCT path. If Setup on a machine still says “install a driver to show hardware”, the cause is more likely “Windows only sees partition 1” / wrong boot mode / GPT USB, not the mere presence of `.swm` files.
2. **Matching MCT on Linux** is ezwin’s default: MBR, one FAT32 partition (32 GiB cap), `ESD-USB`, copy tree, split oversized WIM to ≤3800 MiB `.swm`. Microsoft `bootsect` MBR/VBR is not redistributed; UEFI boot does not need it.
3. **Matching MCT BIOS boot** (active + MS-MBR) is what lets firmware CSM-boot Setup — the 2 TB GPT-disk bug. ezwin sets the active flag like MCT but does not install Microsoft HDD boot code, so “USB HDD” / BIOS entries should fail and UEFI remain the working path.
4. **Do not assume the ISO and MCT USB are the same build.** This pair differed by months and by compression (LZX WIM vs LZMS SWM).

## Re-inspect

```bash
lsblk -o NAME,SIZE,FSTYPE,LABEL,PARTTYPE,PARTFLAGS /dev/sda
sudo fdisk -l /dev/sda
sudo sfdisk -d /dev/sda
sudo wipefs /dev/sda /dev/sda1
sudo file -s /dev/sda /dev/sda1
# mount is usually /run/media/$USER/ESD-USB
findmnt -T /run/media/$USER/ESD-USB
```
