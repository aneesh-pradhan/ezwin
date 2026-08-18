#include "flash.hpp"

#include "devices.hpp"
#include "disk.hpp"

#include <wimlib.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace ezwin {
namespace fs = std::filesystem;

namespace {

constexpr uint64_t kCopyBuf = 1024 * 1024;
constexpr uint64_t kSlack = 64ull * 1024ull * 1024ull;

struct WimHandle {
    WIMStruct* p{nullptr};
    ~WimHandle() {
        if (p) {
            wimlib_free(p);
        }
    }
};

struct WimLib {
    WimLib() {
        const int rc = wimlib_global_init(0);
        if (rc) {
            throw Error(std::string("wimlib_global_init: ") +
                        wimlib_get_error_string(static_cast<wimlib_error_code>(rc)));
        }
    }
    ~WimLib() { wimlib_global_cleanup(); }
};

std::string wim_err(int rc) {
    return wimlib_get_error_string(static_cast<wimlib_error_code>(rc));
}

fs::path child_ci(const fs::path& dir, std::string_view name) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return {};
    }
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (iequals(e.path().filename().string(), std::string(name))) {
            return e.path();
        }
    }
    return {};
}

bool has_efi_bootloader(const fs::path& root) {
    const fs::path efi = child_ci(root, "efi");
    if (efi.empty()) {
        return false;
    }
    const fs::path boot = child_ci(efi, "boot");
    if (boot.empty()) {
        return false;
    }
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(boot, ec)) {
        const std::string n = ascii_lower(e.path().filename().string());
        if (n == "bootx64.efi" || n == "bootia32.efi" || n == "bootaa64.efi") {
            return true;
        }
    }
    return false;
}

void validate_windows_iso(const fs::path& root) {
    const fs::path sources = child_ci(root, "sources");
    if (sources.empty()) {
        throw Error("ISO does not look like Windows install media (no sources/ directory)");
    }
    const bool boot = !child_ci(sources, "boot.wim").empty() || !child_ci(sources, "boot.esd").empty();
    const bool install = !child_ci(sources, "install.wim").empty() ||
                         !child_ci(sources, "install.esd").empty() ||
                         !child_ci(sources, "install.swm").empty();
    if (!boot) {
        throw Error("ISO is missing sources/boot.wim (not a Windows installer image)");
    }
    if (!install) {
        throw Error("ISO is missing sources/install.wim (not a Windows installer image)");
    }
    if (!has_efi_bootloader(root)) {
        throw Error("ISO is missing EFI/BOOT/bootx64.efi (needed for UEFI boot)");
    }
}

struct CopyItem {
    fs::path src;
    fs::path rel;
    uint64_t size{0};
    bool split{false};
};

struct Plan {
    std::vector<CopyItem> files;
    uint64_t copy_bytes{0};
    uint64_t payload_bytes{0};
    uint64_t bytes_needed{0};
};

Plan plan_copy(const fs::path& iso_root) {
    Plan plan;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(
             iso_root, fs::directory_options::skip_permission_denied, ec)) {
        check_stop();
        if (e.is_symlink()) {
            continue;
        }
        if (e.is_directory()) {
            continue;
        }
        if (!e.is_regular_file()) {
            continue;
        }
        CopyItem item;
        item.src = e.path();
        item.rel = fs::relative(e.path(), iso_root);
        item.size = e.file_size();
        const bool payload = is_install_payload_name(item.src.filename().string());
        if (item.size > kFat32MaxFile) {
            if (!payload) {
                throw Error(item.rel.string() + " is " + format_bytes(item.size) +
                            ", which exceeds the FAT32 4 GiB file limit and cannot be split");
            }
            item.split = true;
            plan.payload_bytes += item.size;
        } else {
            plan.copy_bytes += item.size;
        }
        plan.files.push_back(std::move(item));
    }
    plan.bytes_needed = plan.copy_bytes + plan.payload_bytes;
    return plan;
}

void copy_file(const fs::path& src, const fs::path& dst, Progress& progress) {
    fs::create_directories(dst.parent_path());
    int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        throw Error("open " + src.string() + ": " + std::strerror(errno));
    }
    int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        close(in);
        throw Error("create " + dst.string() + ": " + std::strerror(errno));
    }
    posix_fadvise(in, 0, 0, POSIX_FADV_SEQUENTIAL);

    std::vector<uint8_t> buf(kCopyBuf);
    while (true) {
        check_stop();
        const ssize_t n = read(in, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(in);
            close(out);
            throw Error("read " + src.string() + ": " + std::strerror(errno));
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
                throw Error("write " + dst.string() + ": " + std::strerror(errno));
            }
            off += w;
        }
        progress.add(static_cast<uint64_t>(n));
    }
    fsync(out);
    close(in);
    close(out);
}

enum wimlib_progress_status wim_progress(enum wimlib_progress_msg msg,
                                         union wimlib_progress_info* inf, void* ctx) {
    auto* p = static_cast<Progress*>(ctx);
    if (g_stop.load(std::memory_order_relaxed)) {
        return WIMLIB_PROGRESS_STATUS_ABORT;
    }
    if (!inf) {
        return WIMLIB_PROGRESS_STATUS_CONTINUE;
    }
    if (msg == WIMLIB_PROGRESS_MSG_WRITE_STREAMS) {
        p->set(inf->write_streams.completed_bytes, inf->write_streams.total_bytes);
    } else if (msg == WIMLIB_PROGRESS_MSG_SPLIT_BEGIN_PART ||
               msg == WIMLIB_PROGRESS_MSG_SPLIT_END_PART) {
        p->set(inf->split.completed_bytes, inf->split.total_bytes);
    }
    return WIMLIB_PROGRESS_STATUS_CONTINUE;
}

uint64_t available_space(const fs::path& dir) {
    struct statvfs v {};
    if (statvfs(dir.c_str(), &v) < 0) {
        return 0;
    }
    return static_cast<uint64_t>(v.f_bavail) * static_cast<uint64_t>(v.f_frsize);
}

fs::path make_temp_wim(uint64_t need) {
    fs::path base = "/var/tmp";
    std::error_code ec;
    if (!fs::exists(base, ec)) {
        base = fs::temp_directory_path();
    }
    const uint64_t avail = available_space(base);
    if (avail && avail < need) {
        throw Error("need about " + format_bytes(need) + " in " + base.string() +
                    " to recompress a solid ESD, but only " + format_bytes(avail) + " is free");
    }
    auto tmpl = (base / "ezwin-XXXXXX.wim").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    const int fd = mkstemps(buf.data(), 4);
    if (fd < 0) {
        throw Error(std::string("mkstemps: ") + std::strerror(errno));
    }
    close(fd);
    return buf.data();
}

void split_wim_file(const fs::path& src, const fs::path& dest_swm, uint64_t part_size) {
    fs::create_directories(dest_swm.parent_path());
    WimHandle wim;
    int rc = wimlib_open_wim(src.c_str(), 0, &wim.p);
    if (rc) {
        throw Error("open " + src.filename().string() + ": " + wim_err(rc));
    }

    auto do_split = [&](WIMStruct* w) {
        Progress prog("splitting " + src.filename().string());
        wimlib_register_progress_function(w, wim_progress, &prog);
        const int s = wimlib_split(w, dest_swm.c_str(), part_size, 0);
        if (s == 0) {
            prog.finish();
        }
        return s;
    };

    rc = do_split(wim.p);
    if (rc == 0) {
        return;
    }
    if (rc != WIMLIB_ERR_UNSUPPORTED) {
        throw Error("split " + src.filename().string() + ": " + wim_err(rc));
    }

    info("image uses solid compression; recompressing before split (needs temporary disk space)");
    const uint64_t src_size = fs::file_size(src);
    const fs::path tmp = make_temp_wim(src_size + (256ull << 20));
    struct TmpDel {
        fs::path p;
        ~TmpDel() {
            std::error_code ec;
            fs::remove(p, ec);
        }
    } guard{tmp};

    Progress recompress("recompressing " + src.filename().string());
    wimlib_register_progress_function(wim.p, wim_progress, &recompress);
    rc = wimlib_write(wim.p, tmp.c_str(), WIMLIB_ALL_IMAGES, 0, 0);
    if (rc) {
        throw Error("recompress " + src.filename().string() + ": " + wim_err(rc));
    }
    recompress.finish();
    wimlib_free(wim.p);
    wim.p = nullptr;

    rc = wimlib_open_wim(tmp.c_str(), 0, &wim.p);
    if (rc) {
        throw Error("open recompressed WIM: " + wim_err(rc));
    }
    rc = do_split(wim.p);
    if (rc) {
        throw Error("split recompressed WIM: " + wim_err(rc));
    }
}

bool confirm_erase(const Device& dev) {
    std::cerr << '\n';
    warn("this will ERASE all data on " + dev.path);
    std::cerr << "    " << format_bytes(dev.size_bytes);
    if (!dev.vendor.empty() || !dev.model.empty()) {
        std::cerr << "  " << dev.vendor;
        if (!dev.vendor.empty() && !dev.model.empty()) {
            std::cerr << ' ';
        }
        std::cerr << dev.model;
    }
    if (dev.usb) {
        std::cerr << "  (usb)";
    }
    std::cerr << "\n    Type the device path to continue: " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) {
        return false;
    }
    return line == dev.path;
}

}  // namespace

void flash_windows_iso(const Options& opt) {
    if (geteuid() != 0 && !opt.dry_run) {
        throw Error("flashing requires root — re-run with sudo");
    }

    std::error_code ec;
    const fs::path iso = fs::absolute(opt.iso, ec);
    if (ec || !fs::exists(iso) || !fs::is_regular_file(iso)) {
        throw Error("ISO not found: " + opt.iso);
    }
    if (fs::file_size(iso) < 100ull * 1024ull * 1024ull) {
        throw Error(opt.iso + " is too small to be a Windows 11 ISO");
    }

    const Device dev = inspect_device(opt.device);
    if (!dev.whole_disk) {
        throw Error(dev.path + " is a partition — pass the whole disk (for example /dev/sdb)");
    }
    if (!dev.usb && !opt.force) {
        throw Error(dev.path +
                    " does not look like a USB disk. Refusing to touch it without --force");
    }
    if (dev.size_bytes < 8ull * 1000ull * 1000ull * 1000ull) {
        warn("this USB is smaller than 8 GB; Windows 11 media often needs more space");
    }

    info("mounting ISO (UDF) to inspect the installer");
    TempDir tmp;
    Mount iso_mnt;
    iso_mnt.mount_iso(iso.string(), tmp.path() / "iso");
    validate_windows_iso(iso_mnt.root());
    const Plan plan = plan_copy(iso_mnt.root());

    info("ISO looks like Windows install media");
    debug("copy " + format_bytes(plan.copy_bytes) + ", payload " + format_bytes(plan.payload_bytes));

    bool will_split = false;
    for (const auto& f : plan.files) {
        if (f.split) {
            will_split = true;
            info(f.rel.string() + " is " + format_bytes(f.size) +
                 " (> 4 GiB) — will split to install.swm");
        }
    }

    if (dev.size_bytes < plan.bytes_needed + kSlack) {
        throw Error("USB is too small: need about " + format_bytes(plan.bytes_needed + kSlack) +
                    ", device is " + format_bytes(dev.size_bytes));
    }

    std::cerr << '\n';
    std::cerr << "    ISO     " << iso.string() << '\n';
    std::cerr << "    Target  " << dev.path << "  " << format_bytes(dev.size_bytes);
    if (!dev.model.empty()) {
        std::cerr << "  " << dev.model;
    }
    std::cerr << '\n';
    std::cerr << "    Layout  GPT + FAT32 EFI System partition (UEFI, Secure Boot friendly)\n";
    std::cerr << "    Payload " << format_bytes(plan.bytes_needed);
    if (will_split) {
        std::cerr << "  (split WIM under FAT32 4 GiB limit)";
    }
    std::cerr << "\n\n";

    if (opt.dry_run) {
        info("dry-run: no changes written");
        return;
    }
    if (!opt.yes && !confirm_erase(dev)) {
        throw Error("aborted");
    }

    WimLib wimlib;
    const std::string label = fat_label(opt.label);

    info("unmounting " + dev.path);
    unmount_disk(dev.path);

    info("wiping old partition tables (clears leftover hybrid ISO signatures)");
    wipe_partitioning(dev.path);

    info("creating GPT with a single EFI System partition");
    create_gpt_esp(dev.path, label);
    const std::string part = partition_node(dev.path, 1);
    wait_for_partition(part);
    unmount_disk(dev.path);

    info("formatting " + part + " as FAT32 (" + label + ")");
    format_fat32(part, label);
    unmount_disk(dev.path);

    Mount usb_mnt;
    usb_mnt.mount_vfat(part, tmp.path() / "usb");

    {
        Progress prog("copying installer files");
        prog.set_total(plan.copy_bytes);
        for (const auto& f : plan.files) {
            if (f.split) {
                continue;
            }
            copy_file(f.src, usb_mnt.root() / f.rel, prog);
        }
        prog.finish(format_bytes(plan.copy_bytes));
    }

    for (const auto& f : plan.files) {
        if (!f.split) {
            continue;
        }
        const fs::path dest = usb_mnt.root() / f.rel.parent_path() / "install.swm";
        split_wim_file(f.src, dest, opt.split_size);
    }

    info("syncing");
    usb_mnt.sync();
    info("Windows 11 USB is ready — you can unplug " + dev.path + " after the activity light stops");
}

}  // namespace ezwin
