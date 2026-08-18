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
#include <optional>
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

void copy_file_to(const fs::path& src, const std::vector<fs::path>& dests, Progress& progress) {
    if (dests.empty()) {
        return;
    }
    int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        throw Error("open " + src.string() + ": " + std::strerror(errno));
    }
    posix_fadvise(in, 0, 0, POSIX_FADV_SEQUENTIAL);

    std::vector<int> outs;
    outs.reserve(dests.size());
    auto fail_close = [&]() {
        for (int fd : outs) {
            close(fd);
        }
        close(in);
    };
    for (const auto& dst : dests) {
        fs::create_directories(dst.parent_path());
        int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (out < 0) {
            fail_close();
            throw Error("create " + dst.string() + ": " + std::strerror(errno));
        }
        outs.push_back(out);
    }

    std::vector<uint8_t> buf(kCopyBuf);
    while (true) {
        check_stop();
        const ssize_t n = read(in, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail_close();
            throw Error("read " + src.string() + ": " + std::strerror(errno));
        }
        if (n == 0) {
            break;
        }
        for (size_t i = 0; i < outs.size(); ++i) {
            ssize_t off = 0;
            while (off < n) {
                const ssize_t w = write(outs[i], buf.data() + off, static_cast<size_t>(n - off));
                if (w < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    const int err = errno;
                    fail_close();
                    std::string extra;
                    if (err == ENOSPC) {
                        extra = " (destination filesystem ran out of space — the USB partition "
                                "is smaller than the installer)";
                    }
                    throw Error("write " + dests[i].string() + ": " + std::strerror(err) + extra);
                }
                off += w;
            }
        }
        progress.add(static_cast<uint64_t>(n));
    }
    for (int out : outs) {
        fsync(out);
        close(out);
    }
    close(in);
}

bool goes_on_esp(const CopyItem& item) {
    if (is_install_payload_name(item.src.filename().string())) {
        return false;
    }
    return item.size <= kFat32MaxFile;
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
    } else if (msg == WIMLIB_PROGRESS_MSG_SPLIT_BEGIN_PART) {
        p->set_item("part " + std::to_string(inf->split.cur_part_number) + " of " +
                    std::to_string(inf->split.total_parts));
        p->set(inf->split.completed_bytes, inf->split.total_bytes);
    } else if (msg == WIMLIB_PROGRESS_MSG_SPLIT_END_PART) {
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

void require_mount_space(const fs::path& dir, uint64_t need, const std::string& part) {
    const uint64_t avail = available_space(dir);
    const uint64_t part_sz = block_dev_size(part);
    info(part + " is " + format_bytes(part_sz) + ", " + format_bytes(avail) + " free");
    if (avail < need) {
        throw Error("mounted " + part + " has only " + format_bytes(avail) +
                    " free but the installer needs " + format_bytes(need) +
                    " — the kernel likely kept an old smaller partition. Unplug the USB and retry");
    }
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
    std::cerr << "\n    Type yes or the device path to continue: " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) {
        return false;
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line == dev.path || ascii_lower(line) == "yes" || line == "y";
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

    const std::string device = opt.device.empty() ? default_usb_disk() : opt.device;
    const Device dev = inspect_device(device);
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

    uint64_t esp_bytes = 0;
    bool will_split = false;
    for (const auto& f : plan.files) {
        if (opt.layout == Layout::Dual && goes_on_esp(f)) {
            esp_bytes += f.size;
        }
        if (f.split) {
            will_split = true;
            if (opt.layout == Layout::Fat32) {
                info(f.rel.string() + " is " + format_bytes(f.size) +
                     " (> 4 GiB) — will split to install.swm");
            } else {
                info(f.rel.string() + " is " + format_bytes(f.size) +
                     " (> 4 GiB) — keeping intact on NTFS");
            }
        }
        const std::string fname = ascii_lower(f.src.filename().string());
        if (opt.layout == Layout::Dual && (fname == "boot.wim" || fname == "boot.esd") &&
            f.size > kFat32MaxFile) {
            throw Error("boot.wim exceeds the FAT32 4 GiB limit; use --layout ntfs");
        }
    }

    if (opt.layout == Layout::Dual) {
        warn("dual FAT32+NTFS: Windows only sees the first partition on many USB sticks; "
             "the default layout is a single FAT32 volume (Microsoft USB tool)");
    }

    const uint64_t efi_part_bytes =
        align_up(std::max(esp_bytes + 128 * kMiB, 256 * kMiB), kMiB);
    uint64_t need = plan.bytes_needed + kSlack;
    if (opt.layout == Layout::Dual) {
        need += efi_part_bytes;
    }

    const std::string volume =
        opt.label.empty() ? default_volume_label(opt.layout) : opt.label;
    uint64_t fat32_part_bytes = 0;
    if (opt.layout == Layout::Fat32) {
        const uint64_t avail = dev.size_bytes > kMiB ? dev.size_bytes - kMiB : 0;
        fat32_part_bytes = std::min(kMctFat32Bytes, avail);
        if (fat32_part_bytes < need) {
            fat32_part_bytes = avail;
            if (avail > kMctFat32Bytes) {
                warn("installer needs more than Microsoft's 32 GiB FAT32 partition; "
                     "using the whole USB");
            }
        }
        if (fat32_part_bytes < need) {
            throw Error("USB is too small: need about " + format_bytes(need) +
                        ", FAT32 area is " + format_bytes(fat32_part_bytes));
        }
    } else if (dev.size_bytes < need) {
        throw Error("USB is too small: need about " + format_bytes(need) + ", device is " +
                    format_bytes(dev.size_bytes));
    }

    std::string uefi_ntfs_img;
    uint64_t uefi_ntfs_img_size = 0;
    if (opt.layout == Layout::UefiNtfs || opt.layout == Layout::MbrNtfs) {
        uefi_ntfs_img = find_bundled_file("uefi-ntfs.img");
        uefi_ntfs_img_size = fs::file_size(uefi_ntfs_img);
        debug("UEFI:NTFS image " + uefi_ntfs_img + " (" + format_bytes(uefi_ntfs_img_size) + ")");
    }

    std::cerr << '\n';
    std::cerr << "    ISO     " << iso.string() << '\n';
    std::cerr << "    Target  " << dev.path << "  " << format_bytes(dev.size_bytes);
    if (!dev.model.empty()) {
        std::cerr << "  " << dev.model;
    }
    std::cerr << '\n';
    std::cerr << "    Layout  " << layout_name(opt.layout) << '\n';
    if (opt.layout == Layout::Fat32) {
        std::cerr << "    Volume  " << fat_label(volume) << "  "
                  << format_bytes(fat32_part_bytes) << " FAT32";
        if (fat32_part_bytes < (dev.size_bytes > kMiB ? dev.size_bytes - kMiB : 0)) {
            std::cerr << "  (rest of USB left empty, as Microsoft does)";
        }
        std::cerr << '\n';
    }
    std::cerr << "    Payload " << format_bytes(plan.bytes_needed);
    if (opt.layout == Layout::Fat32 && will_split) {
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

    const std::string fat_name = fat_label(volume);
    const std::string ntfs_name = ntfs_label(volume);

    info("unmounting " + dev.path);
    unmount_disk(dev.path);

    info("resetting " + dev.path + " (signatures, partition table, kernel mappings)");
    wipe_partitioning(dev.path);

    std::optional<WimLib> wimlib;
    if (opt.layout == Layout::Fat32 && will_split) {
        wimlib.emplace();
    }

    if (opt.layout == Layout::Dual) {
        info("creating MBR: FAT32 boot (" + format_bytes(efi_part_bytes) + ") + NTFS payload");
        create_mbr_dual(dev.path, efi_part_bytes);
        const std::string esp = partition_node(dev.path, 1);
        const std::string data = partition_node(dev.path, 2);
        wait_for_partition(esp, efi_part_bytes - (16 * kMiB), efi_part_bytes + (16 * kMiB));
        wait_for_partition(data, plan.bytes_needed + kSlack);
        unmount_disk(dev.path);

        info("formatting " + esp + " as FAT32 (EZWINBOOT)");
        format_fat32(esp, "EZWINBOOT");
        info("formatting " + data + " as NTFS (" + ntfs_name + ")");
        format_ntfs(data, ntfs_name);
        unmount_disk(dev.path);

        Mount esp_mnt;
        Mount data_mnt;
        esp_mnt.mount_vfat(esp, tmp.path() / "esp");
        data_mnt.mount_ntfs(data, tmp.path() / "ntfs");
        require_mount_space(esp_mnt.root(), esp_bytes, esp);
        require_mount_space(data_mnt.root(), plan.bytes_needed, data);

        {
            Progress prog("copying installer files");
            prog.set_total(plan.bytes_needed);
            prog.set_file_count(plan.files.size());
            for (const auto& f : plan.files) {
                prog.set_item(f.rel.generic_string());
                std::vector<fs::path> dests{data_mnt.root() / f.rel};
                if (goes_on_esp(f)) {
                    dests.push_back(esp_mnt.root() / f.rel);
                }
                copy_file_to(f.src, dests, prog);
            }
            prog.finish();
        }

        info("syncing");
        data_mnt.sync();
        esp_mnt.sync();
    } else if (opt.layout == Layout::UefiNtfs || opt.layout == Layout::MbrNtfs) {
        if (opt.layout == Layout::UefiNtfs) {
            info("creating GPT: NTFS payload first + 1 MiB UEFI:NTFS stub (Rufus-style)");
            create_gpt_uefi_ntfs(dev.path, uefi_ntfs_img_size);
        } else {
            info("creating MBR: NTFS payload first + 1 MiB UEFI:NTFS stub (WoeUSB-style)");
            create_mbr_uefi_ntfs(dev.path, uefi_ntfs_img_size);
        }
        const std::string data = partition_node(dev.path, 1);
        const std::string esp = partition_node(dev.path, 2);
        wait_for_partition(data, plan.bytes_needed + kSlack);
        wait_for_partition(esp, uefi_ntfs_img_size, 16 * kMiB);
        unmount_disk(dev.path);

        info("formatting " + data + " as NTFS (" + ntfs_name + ")");
        format_ntfs(data, ntfs_name);
        info("writing bundled UEFI:NTFS image to " + esp);
        write_raw_image(esp, uefi_ntfs_img);
        unmount_disk(dev.path);

        Mount data_mnt;
        data_mnt.mount_ntfs(data, tmp.path() / "ntfs");
        require_mount_space(data_mnt.root(), plan.bytes_needed, data);
        {
            Progress prog("copying installer files");
            prog.set_total(plan.bytes_needed);
            prog.set_file_count(plan.files.size());
            for (const auto& f : plan.files) {
                prog.set_item(f.rel.generic_string());
                copy_file_to(f.src, {data_mnt.root() / f.rel}, prog);
            }
            prog.finish();
        }
        info("syncing");
        data_mnt.sync();
    } else {
        const std::string part = partition_node(dev.path, 1);
        if (fat32_part_bytes == kMctFat32Bytes) {
            info("creating MBR: 32 GiB FAT32 (Media Creation Tool-style)");
        } else {
            info("creating MBR: FAT32 partition " + format_bytes(fat32_part_bytes));
        }
        create_mbr_fat32(dev.path, fat32_part_bytes);
        const uint64_t slop = 16 * kMiB;
        wait_for_partition(part, fat32_part_bytes > slop ? fat32_part_bytes - slop : 0,
                           fat32_part_bytes + slop);
        unmount_disk(dev.path);

        info("formatting " + part + " as FAT32 (" + fat_name + ")");
        format_fat32(part, fat_name, true);
        unmount_disk(dev.path);

        Mount usb_mnt;
        usb_mnt.mount_vfat(part, tmp.path() / "usb");
        require_mount_space(usb_mnt.root(), plan.bytes_needed, part);
        {
            Progress prog("copying installer files");
            uint64_t nfiles = 0;
            for (const auto& f : plan.files) {
                if (!f.split) {
                    ++nfiles;
                }
            }
            prog.set_total(plan.copy_bytes);
            prog.set_file_count(nfiles);
            for (const auto& f : plan.files) {
                if (f.split) {
                    continue;
                }
                prog.set_item(f.rel.generic_string());
                copy_file_to(f.src, {usb_mnt.root() / f.rel}, prog);
            }
            prog.finish();
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
    }

    info("Windows 11 USB is ready — unplug " + dev.path + " after the activity light stops");
    if (opt.layout == Layout::Fat32) {
        info("restart and choose the USB in the firmware boot menu (UEFI)");
    } else if (opt.layout == Layout::UefiNtfs) {
        info("restart and choose the USB in the firmware boot menu (UEFI)");
    } else if (opt.layout == Layout::MbrNtfs) {
        info("in the firmware boot menu pick the UEFI USB / UEFI:NTFS entry, not USB HDD");
    }
}

}  // namespace ezwin
