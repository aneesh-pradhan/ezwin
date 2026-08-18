#include "disk.hpp"

#include <libfdisk.h>
#include <libmount.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/fs.h>
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

void wipe_partitioning(const std::string& disk) {
    int fd = open(disk.c_str(), O_RDWR | O_EXCL);
    if (fd < 0) {
        throw Error("cannot open " + disk + " exclusively: " + std::strerror(errno) +
                    " (unmount it, close file managers, then retry)");
    }
    uint64_t size = 0;
    if (ioctl(fd, BLKGETSIZE64, &size) < 0) {
        close(fd);
        throw Error("BLKGETSIZE64: " + std::string(std::strerror(errno)));
    }
    const size_t wipe = 2 * 1024 * 1024;
    std::vector<uint8_t> zeros(wipe, 0);
    if (pwrite(fd, zeros.data(), wipe, 0) < 0) {
        close(fd);
        throw Error("wipe start of " + disk + ": " + std::strerror(errno));
    }
    if (size > wipe) {
        const off_t off = static_cast<off_t>(size - wipe);
        if (pwrite(fd, zeros.data(), wipe, off) < 0) {
            close(fd);
            throw Error("wipe end of " + disk + ": " + std::strerror(errno));
        }
    }
    fsync(fd);
    ioctl(fd, BLKRRPART);
    close(fd);
}

void create_gpt_esp(const std::string& disk, const std::string& part_name) {
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
        fdisk_check(fdisk_create_disklabel(cxt, "gpt"), "create GPT");

        fdisk_partition* pa = fdisk_new_partition();
        if (!pa) {
            throw Error("fdisk_new_partition failed");
        }
        fdisk_partition_start_follow_default(pa, 1);
        fdisk_partition_end_follow_default(pa, 1);
        fdisk_partition_set_partno(pa, 0);

        fdisk_label* lb = fdisk_get_label(cxt, nullptr);
        fdisk_parttype* type = fdisk_label_parse_parttype(lb, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
        if (!type) {
            type = fdisk_label_parse_parttype(lb, "uefi");
        }
        if (!type) {
            fdisk_unref_partition(pa);
            throw Error("cannot resolve EFI System partition type");
        }
        fdisk_partition_set_type(pa, type);
        fdisk_unref_parttype(type);
        if (!part_name.empty()) {
            fdisk_partition_set_name(pa, part_name.c_str());
        }

        size_t partno = 0;
        rc = fdisk_add_partition(cxt, pa, &partno);
        fdisk_unref_partition(pa);
        fdisk_check(rc, "add EFI partition");

        fdisk_check(fdisk_write_disklabel(cxt), "write partition table");
        rc = fdisk_reread_partition_table(cxt);
        if (rc) {
            warn("kernel did not reread the partition table immediately; waiting for udev");
        }
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}

void wait_for_partition(const std::string& part) {
    for (int i = 0; i < 80; ++i) {
        check_stop();
        struct stat st {};
        if (stat(part.c_str(), &st) == 0 && S_ISBLK(st.st_mode)) {
            int fd = open(part.c_str(), O_RDWR);
            if (fd >= 0) {
                close(fd);
                return;
            }
        }
        usleep(100000);
    }
    throw Error("timed out waiting for " + part + " (udev did not create the partition node)");
}

void format_fat32(const std::string& part, const std::string& label) {
    std::string mkfs = which("mkfs.fat");
    if (mkfs.empty()) {
        mkfs = which("mkfs.vfat");
    }
    if (mkfs.empty()) {
        throw Error("mkfs.fat not found; install dosfstools");
    }
    const std::string vol = fat_label(label);
    run_cmd_checked({mkfs, "-F", "32", "-n", vol, part});
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
