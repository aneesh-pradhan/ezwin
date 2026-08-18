#include "disk.hpp"

#include <libfdisk.h>
#include <libmount.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <linux/blkpg.h>
#include <linux/fs.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ezwin {
namespace {

void fdisk_check(int rc, const char* what) {
    if (rc) {
        const int err = (rc < 0) ? -rc : rc;
        throw Error(std::string(what) + ": " + std::strerror(err));
    }
}

bool is_child_block(const std::string& src, const std::string& disk) {
    if (src == disk) {
        return true;
    }
    if (src.size() <= disk.size() || !src.starts_with(disk)) {
        return false;
    }
    const std::string rest = src.substr(disk.size());
    if (rest[0] == 'p' && rest.size() > 1) {
        return std::all_of(rest.begin() + 1, rest.end(),
                           [](unsigned char c) { return std::isdigit(c); });
    }
    if (std::isdigit(static_cast<unsigned char>(disk.back()))) {
        return false;
    }
    return std::all_of(rest.begin(), rest.end(), [](unsigned char c) { return std::isdigit(c); });
}

void mnt_check(libmnt_context* cxt, int rc, const char* what) {
    if (rc == 0 && mnt_context_get_status(cxt) == 1) {
        return;
    }
    std::string msg = what;
    const int syserr = mnt_context_get_syscall_errno(cxt);
    if (syserr) {
        msg += ": ";
        msg += std::strerror(syserr);
    } else {
        msg += " failed";
    }
    throw Error(msg);
}

constexpr const char* kDosNtfs = "7";     // HPFS/NTFS/exFAT
constexpr const char* kDosFat32 = "c";    // W95 FAT32 (LBA)
constexpr const char* kDosEfi = "ef";     // EFI (FAT-12/16/32)
constexpr const char* kGptMsftData = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
// Microsoft Basic Data "NoDriveLetter" (bit 63). Rufus sets this on the UEFI:NTFS stub.
constexpr uint64_t kGptNoDriveLetter = 1ULL << 63;

std::string sysname_of(const std::string& path) {
    char buf[4096];
    if (realpath(path.c_str(), buf)) {
        const std::string real = buf;
        const auto slash = real.rfind('/');
        return slash == std::string::npos ? real : real.substr(slash + 1);
    }
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string whole_disk_of(const std::string& part) {
    std::string s = part;
    while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    if (!s.empty() && s.back() == 'p') {
        const std::string maybe = s.substr(0, s.size() - 1);
        if (!maybe.empty() && std::isdigit(static_cast<unsigned char>(maybe.back()))) {
            return maybe;
        }
    }
    return s;
}

uint64_t dev_size(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    uint64_t size = 0;
    if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
        size = 0;
    }
    close(fd);
    return size;
}

std::vector<std::string> list_partitions(const std::string& disk) {
    std::vector<std::string> out;
    const std::string name = sysname_of(disk);
    std::error_code ec;
    const std::filesystem::path sys = std::filesystem::path("/sys/block") / name;
    if (std::filesystem::exists(sys, ec)) {
        for (const auto& e : std::filesystem::directory_iterator(sys, ec)) {
            const std::string fn = e.path().filename().string();
            if (fn.size() > name.size() && fn.rfind(name, 0) == 0) {
                out.push_back("/dev/" + fn);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void settle_udev() {
    const std::string udevadm = which("udevadm");
    if (!udevadm.empty()) {
        run_cmd({udevadm, "settle", "--timeout=8"});
    }
}

void try_wipefs(const std::string& path) {
    const std::string wipefs = which("wipefs");
    if (wipefs.empty()) {
        return;
    }
    debug("wipefs " + path);
    run_cmd({wipefs, "--all", "--force", path});
}

void zero_range(int fd, off_t off, size_t len) {
    std::vector<uint8_t> zeros(std::min(len, static_cast<size_t>(1024 * 1024)), 0);
    while (len > 0) {
        const size_t n = std::min(len, zeros.size());
        const ssize_t w = pwrite(fd, zeros.data(), n, off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw Error(std::string("wipe: ") + std::strerror(errno));
        }
        if (w == 0) {
            throw Error("wipe: short write");
        }
        off += w;
        len -= static_cast<size_t>(w);
    }
}

void zero_device_head(const std::string& path, size_t bytes) {
    int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        debug("cannot open " + path + " to wipe signatures: " + std::strerror(errno));
        return;
    }
    uint64_t size = 0;
    ioctl(fd, BLKGETSIZE64, &size);
    if (size > 0) {
        try {
            zero_range(fd, 0, static_cast<size_t>(std::min<uint64_t>(bytes, size)));
            fsync(fd);
        } catch (...) {
            close(fd);
            throw;
        }
    }
    close(fd);
}

int blkpg(int fd, int op, int pno, uint64_t start_bytes, uint64_t length_bytes) {
    struct blkpg_partition part {};
    part.pno = pno;
    part.start = static_cast<long long>(start_bytes);
    part.length = static_cast<long long>(length_bytes);
    struct blkpg_ioctl_arg arg {};
    arg.op = op;
    arg.datalen = static_cast<int>(sizeof(part));
    arg.data = &part;
    return ioctl(fd, BLKPG, &arg);
}

void drop_kernel_partitions(int fd) {
    for (int pno = 1; pno <= 64; ++pno) {
        blkpg(fd, BLKPG_DEL_PARTITION, pno, 0, 0);
    }
}

int open_disk_excl(const std::string& disk) {
    int last_err = EBUSY;
    for (int i = 0; i < 30; ++i) {
        check_stop();
        unmount_disk(disk);
        int fd = open(disk.c_str(), O_RDWR | O_EXCL | O_CLOEXEC);
        if (fd >= 0) {
            return fd;
        }
        last_err = errno;
        usleep(100000);
    }
    throw Error("cannot open " + disk + " exclusively: " + std::strerror(last_err) +
                " (unmount it, close file managers, then retry)");
}

void publish_to_kernel(fdisk_context* cxt) {
    const int fd = fdisk_get_devfd(cxt);
    if (fd < 0) {
        return;
    }
    drop_kernel_partitions(fd);
    if (fdisk_reread_partition_table(cxt) == 0) {
        return;
    }
    debug("BLKRRPART failed; adding partitions with BLKPG");
    fdisk_table* tb = nullptr;
    if (fdisk_get_partitions(cxt, &tb) != 0 || !tb) {
        warn("kernel did not reread the partition table; waiting for udev");
        return;
    }
    unsigned long ss = fdisk_get_sector_size(cxt);
    if (ss == 0) {
        ss = 512;
    }
    fdisk_iter* itr = fdisk_new_iter(FDISK_ITER_FORWARD);
    fdisk_partition* pa = nullptr;
    while (fdisk_table_next_partition(tb, itr, &pa) == 0) {
        if (!pa || fdisk_partition_is_freespace(pa)) {
            continue;
        }
        if (!fdisk_partition_has_partno(pa) || !fdisk_partition_has_start(pa) ||
            !fdisk_partition_has_size(pa)) {
            continue;
        }
        const int pno = static_cast<int>(fdisk_partition_get_partno(pa)) + 1;
        const uint64_t start = fdisk_partition_get_start(pa) * ss;
        const uint64_t length = fdisk_partition_get_size(pa) * ss;
        blkpg(fd, BLKPG_DEL_PARTITION, pno, 0, 0);
        if (blkpg(fd, BLKPG_ADD_PARTITION, pno, start, length) != 0) {
            blkpg(fd, BLKPG_RESIZE_PARTITION, pno, start, length);
        }
    }
    fdisk_free_iter(itr);
    fdisk_unref_table(tb);
}

void add_dos_partition(fdisk_context* cxt, size_t partno, uint64_t size_sectors,
                       const char* type_code, bool bootable, uint64_t start_lba = ~0ull) {
    fdisk_partition* pa = fdisk_new_partition();
    if (!pa) {
        throw Error("fdisk_new_partition failed");
    }
    if (start_lba != ~0ull) {
        fdisk_partition_start_follow_default(pa, 0);
        fdisk_partition_set_start(pa, start_lba);
    } else {
        fdisk_partition_start_follow_default(pa, 1);
    }
    fdisk_partition_set_partno(pa, partno);
    if (size_sectors == 0) {
        fdisk_partition_end_follow_default(pa, 1);
    } else {
        fdisk_partition_end_follow_default(pa, 0);
        fdisk_partition_size_explicit(pa, 1);
        fdisk_partition_set_size(pa, size_sectors);
    }

    fdisk_label* lb = fdisk_get_label(cxt, nullptr);
    fdisk_parttype* type = fdisk_label_parse_parttype(lb, type_code);
    if (!type) {
        fdisk_unref_partition(pa);
        throw Error(std::string("cannot resolve MBR type ") + type_code);
    }
    fdisk_partition_set_type(pa, type);
    fdisk_unref_parttype(type);

    size_t n = partno;
    const int rc = fdisk_add_partition(cxt, pa, &n);
    fdisk_unref_partition(pa);
    fdisk_check(rc, "add partition");

    if (bootable) {
        fdisk_check(fdisk_toggle_partition_flag(cxt, partno, DOS_FLAG_ACTIVE), "set boot flag");
    }
}

void add_gpt_partition(fdisk_context* cxt, size_t partno, uint64_t size_sectors, const char* type_guid,
                       const char* name, uint64_t start_lba = ~0ull) {
    fdisk_partition* pa = fdisk_new_partition();
    if (!pa) {
        throw Error("fdisk_new_partition failed");
    }
    if (start_lba != ~0ull) {
        fdisk_partition_start_follow_default(pa, 0);
        fdisk_partition_set_start(pa, start_lba);
    } else {
        fdisk_partition_start_follow_default(pa, 1);
    }
    fdisk_partition_set_partno(pa, partno);
    if (size_sectors == 0) {
        fdisk_partition_end_follow_default(pa, 1);
    } else {
        fdisk_partition_end_follow_default(pa, 0);
        fdisk_partition_size_explicit(pa, 1);
        fdisk_partition_set_size(pa, size_sectors);
    }

    fdisk_label* lb = fdisk_get_label(cxt, nullptr);
    fdisk_parttype* type = fdisk_label_parse_parttype(lb, type_guid);
    if (!type) {
        fdisk_unref_partition(pa);
        throw Error(std::string("cannot resolve GPT type ") + type_guid);
    }
    fdisk_partition_set_type(pa, type);
    fdisk_unref_parttype(type);
    if (name && name[0]) {
        fdisk_partition_set_name(pa, name);
    }

    size_t n = partno;
    const int rc = fdisk_add_partition(cxt, pa, &n);
    fdisk_unref_partition(pa);
    fdisk_check(rc, "add GPT partition");
}

uint64_t bytes_to_aligned_sectors(fdisk_context* cxt, uint64_t bytes) {
    unsigned long ss = fdisk_get_sector_size(cxt);
    if (ss == 0) {
        ss = 512;
    }
    unsigned long grain = fdisk_get_grain_size(cxt);
    if (grain == 0) {
        grain = 1024 * 1024;
    }
    uint64_t sectors = (bytes + ss - 1) / ss;
    uint64_t grain_sec = grain / ss;
    if (grain_sec == 0) {
        grain_sec = 1;
    }
    return (sectors + grain_sec - 1) / grain_sec * grain_sec;
}

void set_mbr_first_usable(fdisk_context* cxt) {
    unsigned long ss = fdisk_get_sector_size(cxt);
    if (ss == 0) {
        ss = 512;
    }
    // WoeUSB starts at 4 MiB: GRUB post-MBR gap + flash erase-block alignment.
    fdisk_set_first_lba(cxt, (4 * kMiB) / ss);
}

void with_new_mbr(const std::string& disk, const std::function<void(fdisk_context*)>& fn) {
    fdisk_context* cxt = fdisk_new_context();
    if (!cxt) {
        throw Error("fdisk_new_context failed");
    }
    fdisk_disable_dialogs(cxt, 1);

    int rc = fdisk_assign_device(cxt, disk.c_str(), 0);
    if (rc) {
        fdisk_unref_context(cxt);
        fdisk_check(rc, "fdisk_assign_device");
    }

    auto cleanup = [&]() {
        fdisk_deassign_device(cxt, 0);
        fdisk_unref_context(cxt);
    };

    try {
        fdisk_enable_wipe(cxt, 1);
        fdisk_check(fdisk_create_disklabel(cxt, "dos"), "create MBR");
        fn(cxt);
        for (size_t i = 0; i < 4; ++i) {
            if (fdisk_is_partition_used(cxt, i)) {
                fdisk_wipe_partition(cxt, i, 1);
            }
        }
        fdisk_dos_fix_chs(cxt);
        fdisk_check(fdisk_write_disklabel(cxt), "write partition table");
        publish_to_kernel(cxt);
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
    reread_partition_table(disk);
}

void with_new_gpt(const std::string& disk, const std::function<void(fdisk_context*)>& fn) {
    fdisk_context* cxt = fdisk_new_context();
    if (!cxt) {
        throw Error("fdisk_new_context failed");
    }
    fdisk_disable_dialogs(cxt, 1);

    int rc = fdisk_assign_device(cxt, disk.c_str(), 0);
    if (rc) {
        fdisk_unref_context(cxt);
        fdisk_check(rc, "fdisk_assign_device");
    }

    auto cleanup = [&]() {
        fdisk_deassign_device(cxt, 0);
        fdisk_unref_context(cxt);
    };

    try {
        fdisk_enable_wipe(cxt, 1);
        fdisk_check(fdisk_create_disklabel(cxt, "gpt"), "create GPT");
        // New random disk GUID so this stick is not confused with a Microsoft ISO hybrid.
        fdisk_set_disklabel_id(cxt);
        fn(cxt);
        for (size_t i = 0; i < 8; ++i) {
            if (fdisk_is_partition_used(cxt, i)) {
                fdisk_wipe_partition(cxt, i, 1);
            }
        }
        fdisk_check(fdisk_write_disklabel(cxt), "write partition table");
        publish_to_kernel(cxt);
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
    reread_partition_table(disk);
}

}  // namespace

std::string partition_node(const std::string& disk, unsigned n) {
    if (disk.empty()) {
        throw Error("empty disk path");
    }
    if (std::isdigit(static_cast<unsigned char>(disk.back()))) {
        return disk + "p" + std::to_string(n);
    }
    return disk + std::to_string(n);
}

void unmount_disk(const std::string& disk) {
    libmnt_table* tb = mnt_new_table_from_file("/proc/self/mountinfo");
    if (!tb) {
        throw Error("cannot read /proc/self/mountinfo");
    }
    libmnt_iter* itr = mnt_new_iter(MNT_ITER_BACKWARD);
    libmnt_fs* fs = nullptr;
    std::vector<std::string> targets;
    while (mnt_table_next_fs(tb, itr, &fs) == 0) {
        const char* src = mnt_fs_get_srcpath(fs);
        const char* tgt = mnt_fs_get_target(fs);
        if (!src || !tgt) {
            continue;
        }
        char* real = realpath(src, nullptr);
        const std::string resolved = real ? real : src;
        std::free(real);
        if (is_child_block(resolved, disk) || is_child_block(src, disk)) {
            targets.emplace_back(tgt);
        }
    }
    mnt_free_iter(itr);
    mnt_free_table(tb);

    for (const auto& tgt : targets) {
        debug("unmount " + tgt);
        if (umount2(tgt.c_str(), 0) == 0) {
            continue;
        }
        if (umount2(tgt.c_str(), MNT_DETACH) < 0 && errno != EINVAL && errno != ENOENT) {
            warn("could not unmount " + tgt + ": " + std::strerror(errno));
        }
    }
}

uint64_t block_dev_size(const std::string& path) { return dev_size(path); }

void reread_partition_table(const std::string& disk) {
    unmount_disk(disk);
    int fd = open(disk.c_str(), O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        if (ioctl(fd, BLKRRPART) < 0) {
            debug(std::string("BLKRRPART: ") + std::strerror(errno));
            drop_kernel_partitions(fd);
            if (ioctl(fd, BLKRRPART) < 0) {
                debug(std::string("BLKRRPART after drop: ") + std::strerror(errno));
            }
        }
        ioctl(fd, BLKFLSBUF);
        close(fd);
    }
    const std::string partprobe = which("partprobe");
    if (!partprobe.empty()) {
        run_cmd({partprobe, "-s", disk});
    } else {
        const std::string partx = which("partx");
        if (!partx.empty()) {
            run_cmd({partx, "-u", disk});
        }
    }
    settle_udev();
}

void wipe_partitioning(const std::string& disk) {
    unmount_disk(disk);

    const auto parts = list_partitions(disk);
    for (const auto& part : parts) {
        unmount_disk(disk);
        try_wipefs(part);
        zero_device_head(part, 1024 * 1024);
    }
    try_wipefs(disk);

    int fd = open_disk_excl(disk);
    uint64_t size = 0;
    if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
        close(fd);
        throw Error("BLKGETSIZE64: " + std::string(std::strerror(errno)));
    }
    // Hybrid ISOs leave UDF/ISO9660 at the start; GPT keeps a backup header at the end.
    const size_t wipe = 16 * 1024 * 1024;
    try {
        zero_range(fd, 0, static_cast<size_t>(std::min<uint64_t>(wipe, size)));
        if (size > wipe) {
            zero_range(fd, static_cast<off_t>(size - wipe), wipe);
        }
        fsync(fd);
        ioctl(fd, BLKFLSBUF);

        std::vector<uint8_t> probe(4096);
        const ssize_t n = pread(fd, probe.data(), probe.size(), 0);
        if (n == static_cast<ssize_t>(probe.size())) {
            for (uint8_t b : probe) {
                if (b != 0) {
                    close(fd);
                    throw Error(disk +
                                " ignored the wipe (write-protected or failed flash memory)");
                }
            }
        }

        drop_kernel_partitions(fd);
        if (ioctl(fd, BLKRRPART) < 0) {
            drop_kernel_partitions(fd);
            ioctl(fd, BLKRRPART);
        }
    } catch (...) {
        close(fd);
        throw;
    }
    close(fd);

    settle_udev();
    for (int i = 0; i < 50; ++i) {
        check_stop();
        const auto left = list_partitions(disk);
        if (left.empty()) {
            return;
        }
        unmount_disk(disk);
        int fd2 = open(disk.c_str(), O_RDWR | O_CLOEXEC);
        if (fd2 >= 0) {
            drop_kernel_partitions(fd2);
            ioctl(fd2, BLKRRPART);
            close(fd2);
        }
        usleep(100000);
        if (i == 49) {
            throw Error("could not drop old partitions on " + disk + " (" + left.front() +
                        " still present). Unmount it, or unplug the USB and retry");
        }
    }
}

void create_mbr_fat32(const std::string& disk, uint64_t part_bytes) {
    with_new_mbr(disk, [&](fdisk_context* cxt) {
        unsigned long ss = fdisk_get_sector_size(cxt);
        if (ss == 0) {
            ss = 512;
        }
        // MCT: `create partition primary` starts at 1 MiB (LBA 2048 on 512-byte sectors).
        const uint64_t first = kMiB / ss;
        fdisk_set_first_lba(cxt, first);
        uint64_t size_sec = 0;
        if (part_bytes) {
            size_sec = part_bytes / ss;
            const uint64_t last = fdisk_get_last_lba(cxt);
            const uint64_t max_sz = (last + 1 > first) ? (last + 1 - first) : 0;
            if (size_sec > max_sz) {
                size_sec = max_sz;
            }
        }
        add_dos_partition(cxt, 0, size_sec, kDosFat32, true, first);
    });
}

void create_mbr_dual(const std::string& disk, uint64_t fat_bytes) {
    with_new_mbr(disk, [&](fdisk_context* cxt) {
        set_mbr_first_usable(cxt);
        add_dos_partition(cxt, 0, bytes_to_aligned_sectors(cxt, fat_bytes), kDosFat32, true);
        add_dos_partition(cxt, 1, 0, kDosNtfs, false);
    });
}

void create_mbr_uefi_ntfs(const std::string& disk, uint64_t fat_bytes) {
    with_new_mbr(disk, [&](fdisk_context* cxt) {
        unsigned long ss = fdisk_get_sector_size(cxt);
        if (ss == 0) {
            ss = 512;
        }
        // Exact 1 MiB stub: default 1 MiB grain would round it away on some disks.
        fdisk_save_user_grain(cxt, ss);
        const uint64_t first = (4 * kMiB) / ss;
        fdisk_set_first_lba(cxt, first);
        const uint64_t last = fdisk_get_last_lba(cxt);
        const uint64_t stub = std::max((fat_bytes + ss - 1) / ss, kMiB / ss);
        if (last + 1 <= first + stub + 2048) {
            throw Error("USB is too small for the UEFI:NTFS layout");
        }
        const uint64_t ntfs_sz = (last + 1) - first - stub;
        // No BIOS active flag: Windows 11 must boot UEFI. An active NTFS + bootmgr
        // makes firmware CSM-boot Setup, which then sees GPT NVMe as a 2 TB EE partition.
        add_dos_partition(cxt, 0, ntfs_sz, kDosNtfs, false, first);
        add_dos_partition(cxt, 1, stub, kDosEfi, false, first + ntfs_sz);
    });
    disable_csm_mbr(disk);
}

void disable_csm_mbr(const std::string& disk) {
    int fd = open(disk.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        throw Error("open " + disk + " to clear BIOS boot code: " + std::strerror(errno));
    }
    uint8_t mbr[512];
    const ssize_t n = pread(fd, mbr, sizeof(mbr), 0);
    if (n != 512) {
        close(fd);
        throw Error("read MBR of " + disk + ": " + std::strerror(errno));
    }
    // Keep the NT disk signature (440–443) and the partition table (446–510).
    std::memset(mbr, 0, 440);
    for (int i = 0; i < 4; ++i) {
        mbr[446 + i * 16] &= 0x7F;
    }
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    if (pwrite(fd, mbr, sizeof(mbr), 0) != 512) {
        close(fd);
        throw Error("write MBR of " + disk + ": " + std::strerror(errno));
    }
    fsync(fd);
    close(fd);
}

void create_gpt_uefi_ntfs(const std::string& disk, uint64_t fat_bytes) {
    with_new_gpt(disk, [&](fdisk_context* cxt) {
        unsigned long ss = fdisk_get_sector_size(cxt);
        if (ss == 0) {
            ss = 512;
        }
        fdisk_save_user_grain(cxt, ss);
        const uint64_t first = std::max(fdisk_get_first_lba(cxt), (1 * kMiB) / ss);
        fdisk_set_first_lba(cxt, first);
        const uint64_t last = fdisk_get_last_lba(cxt);
        const uint64_t stub = std::max((fat_bytes + ss - 1) / ss, kMiB / ss);
        if (last + 1 <= first + stub + 2048) {
            throw Error("USB is too small for the UEFI:NTFS layout");
        }
        const uint64_t ntfs_sz = (last + 1) - first - stub;
        add_gpt_partition(cxt, 0, ntfs_sz, kGptMsftData, "EZWIN", first);
        add_gpt_partition(cxt, 1, stub, kGptMsftData, "UEFI:NTFS", first + ntfs_sz);
        // Rufus: never mark the stub as an EFI System Partition — Windows Setup
        // cannot handle two ESPs and fails while copying files / next phase.
        fdisk_gpt_set_partition_attrs(cxt, 1, kGptNoDriveLetter);
    });
}

void wait_for_partition(const std::string& part, uint64_t min_bytes, uint64_t max_bytes) {
    uint64_t last_size = 0;
    bool seen = false;
    for (int i = 0; i < 120; ++i) {
        check_stop();
        struct stat st {};
        if (stat(part.c_str(), &st) == 0 && S_ISBLK(st.st_mode)) {
            int fd = open(part.c_str(), O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                uint64_t size = 0;
                ioctl(fd, BLKGETSIZE64, &size);
                close(fd);
                last_size = size;
                seen = true;
                const bool big_enough = min_bytes == 0 || size >= min_bytes;
                const bool small_enough = max_bytes == 0 || size <= max_bytes;
                if (big_enough && small_enough) {
                    return;
                }
            }
        }
        if (i == 0 || i % 10 == 0) {
            reread_partition_table(whole_disk_of(part));
        } else {
            usleep(100000);
        }
    }
    if (seen && (min_bytes || max_bytes)) {
        throw Error("partition " + part + " is " + format_bytes(last_size) +
                    " after rewriting the table (wanted " +
                    (min_bytes ? (">= " + format_bytes(min_bytes)) : std::string()) +
                    (min_bytes && max_bytes ? " and " : "") +
                    (max_bytes ? ("<= " + format_bytes(max_bytes)) : std::string()) +
                    "). The kernel still has the old layout — unplug the USB and retry");
    }
    throw Error("timed out waiting for " + part + " (udev did not create the partition node)");
}

void format_fat32(const std::string& part, const std::string& label, bool mct_style) {
    std::string mkfs = which("mkfs.fat");
    if (mkfs.empty()) {
        mkfs = which("mkfs.vfat");
    }
    if (mkfs.empty()) {
        throw Error("mkfs.fat not found; install dosfstools");
    }
    const std::string vol = fat_label(label);
    auto run = [&](const std::vector<std::string>& extra) {
        std::vector<std::string> argv{mkfs, "-F", "32", "-n", vol};
        argv.insert(argv.end(), extra.begin(), extra.end());
        argv.push_back(part);
        run_cmd_checked(argv);
    };
    if (!mct_style) {
        run({});
        return;
    }
    // Windows `format /FS:FAT32` on the 32 GiB MCT volume: 32 KiB clusters, BIOS
    // drive 0x80, H/S 255/63, OEM codepage 437, ~8196 reserved sectors.
    std::vector<std::string> extra{"-s", "64", "-D", "128", "-g", "255/63", "--codepage=437"};
    if (block_dev_size(part) >= 30 * kGiB) {
        extra.insert(extra.end(), {"-R", "8196"});
    }
    try {
        run(extra);
    } catch (const Error& e) {
        warn(std::string("Windows-like FAT32 format failed (") + e.what() +
             "); retrying with mkfs.fat defaults");
        run({});
    }
}

void format_ntfs(const std::string& part, const std::string& label) {
    std::string mkfs = which("mkfs.ntfs");
    if (mkfs.empty()) {
        mkfs = which("mkntfs");
    }
    if (mkfs.empty()) {
        throw Error("mkfs.ntfs not found; install ntfsprogs (Fedora) or ntfs-3g (Debian)");
    }
    run_cmd_checked({mkfs, "-Q", "-F", "-L", ntfs_label(label), part});
}

void write_raw_image(const std::string& part, const std::string& image_path) {
    int in = open(image_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        throw Error("open " + image_path + ": " + std::strerror(errno));
    }
    int out = open(part.c_str(), O_RDWR | O_CLOEXEC);
    if (out < 0) {
        close(in);
        throw Error("open " + part + ": " + std::strerror(errno));
    }
    std::vector<uint8_t> buf(1024 * 1024);
    while (true) {
        const ssize_t n = read(in, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(in);
            close(out);
            throw Error("read " + image_path + ": " + std::strerror(errno));
        }
        if (n == 0) {
            break;
        }
        ssize_t off = 0;
        while (off < n) {
            const ssize_t w = write(out, buf.data() + off, static_cast<size_t>(n - off));
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(in);
                close(out);
                throw Error("write " + part + ": " + std::strerror(errno));
            }
            off += w;
        }
    }
    fsync(out);
    close(in);
    close(out);
}

TempDir::TempDir() {
    auto tmpl = (std::filesystem::temp_directory_path() / "ezwin.XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) {
        throw Error(std::string("mkdtemp: ") + std::strerror(errno));
    }
    path_ = buf.data();
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

Mount::~Mount() {
    try {
        umount();
    } catch (...) {
    }
}

void Mount::mount_fs(const std::string& source, const std::filesystem::path& target,
                     const char* fstype, const char* options) {
    std::filesystem::create_directories(target);
    libmnt_context* cxt = mnt_new_context();
    if (!cxt) {
        throw Error("mnt_new_context failed");
    }
    mnt_context_set_source(cxt, source.c_str());
    mnt_context_set_target(cxt, target.c_str());
    if (fstype) {
        mnt_context_set_fstype(cxt, fstype);
    }
    if (options) {
        mnt_context_set_options(cxt, options);
    }
    const int rc = mnt_context_mount(cxt);
    try {
        mnt_check(cxt, rc, ("mount " + source).c_str());
    } catch (...) {
        mnt_free_context(cxt);
        throw;
    }
    mnt_free_context(cxt);
    target_ = target;
    active_ = true;
}

void Mount::mount_iso(const std::string& iso, const std::filesystem::path& target) {
    std::string last;
    const char* types[] = {nullptr, "udf", "iso9660"};
    for (const char* t : types) {
        try {
            mount_fs(iso, target, t, "loop,ro");
            return;
        } catch (const Error& e) {
            last = e.what();
            umount();
        }
    }
    throw Error("cannot mount ISO (UDF/ISO9660): " + last);
}

void Mount::mount_vfat(const std::string& device, const std::filesystem::path& target) {
    mount_fs(device, target, "vfat", "rw,flush,utf8,shortname=mixed");
}

void Mount::mount_ntfs(const std::string& device, const std::filesystem::path& target) {
    std::string last;
    const struct {
        const char* type;
        const char* options;
    } attempts[] = {
        {"ntfs3", "rw"},
        {"ntfs-3g", "rw,windows_names"},
        {"ntfs", "rw"},
    };
    for (const auto& a : attempts) {
        try {
            mount_fs(device, target, a.type, a.options);
            return;
        } catch (const Error& e) {
            last = e.what();
            umount();
        }
    }
    throw Error("cannot mount NTFS (" + last + "); install ntfs-3g or use a kernel with ntfs3");
}

void Mount::sync() const {
    if (!active_) {
        return;
    }
    int fd = open(target_.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        syncfs(fd);
        close(fd);
    } else {
        ::sync();
    }
}

void Mount::umount() {
    if (!active_) {
        return;
    }
    libmnt_context* cxt = mnt_new_context();
    if (cxt) {
        mnt_context_set_target(cxt, target_.c_str());
        mnt_context_enable_lazy(cxt, 1);
        mnt_context_umount(cxt);
        mnt_free_context(cxt);
    }
    umount2(target_.c_str(), MNT_DETACH);
    active_ = false;
}

}  // namespace ezwin
