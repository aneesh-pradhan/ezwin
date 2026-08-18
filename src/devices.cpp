#include "devices.hpp"

#include <libudev.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace ezwin {
namespace {

struct Udev {
    udev* u{nullptr};
    Udev() {
        u = udev_new();
        if (!u) {
            throw Error("udev_new failed");
        }
    }
    ~Udev() { udev_unref(u); }
    Udev(const Udev&) = delete;
    Udev& operator=(const Udev&) = delete;
};

bool skip_sysname(const char* name) {
    if (!name) {
        return true;
    }
    static const char* prefixes[] = {"loop", "ram", "zram", "sr", "fd", "md", "dm-", "nbd",
                                     "virtual", "zram"};
    for (const char* p : prefixes) {
        if (std::strncmp(name, p, std::strlen(p)) == 0) {
            return true;
        }
    }
    return false;
}

std::string prop(udev_device* d, const char* key) {
    const char* v = udev_device_get_property_value(d, key);
    return v ? v : "";
}

std::string attr(udev_device* d, const char* key) {
    const char* v = udev_device_get_sysattr_value(d, key);
    return v ? v : "";
}

uint64_t sys_size_bytes(udev_device* d) {
    const char* s = udev_device_get_sysattr_value(d, "size");
    if (!s) {
        return 0;
    }
    unsigned long long sectors = std::strtoull(s, nullptr, 10);
    return sectors * 512ull;
}

bool is_usb_disk(udev_device* d) {
    udev_device* usb = udev_device_get_parent_with_subsystem_devtype(d, "usb", "usb_device");
    if (usb) {
        return true;
    }
    const char* bus = udev_device_get_property_value(d, "ID_BUS");
    if (bus && std::strcmp(bus, "usb") == 0) {
        return true;
    }
    const char* tran = udev_device_get_property_value(d, "ID_USB_DRIVER");
    return tran != nullptr;
}

Device from_udev(udev_device* d) {
    Device out;
    const char* node = udev_device_get_devnode(d);
    const char* sysname = udev_device_get_sysname(d);
    out.path = node ? node : "";
    out.sysname = sysname ? sysname : "";
    out.size_bytes = sys_size_bytes(d);
    out.model = prop(d, "ID_MODEL");
    if (out.model.empty()) {
        out.model = attr(d, "device/model");
    }
    out.vendor = prop(d, "ID_VENDOR");
    out.serial = prop(d, "ID_SERIAL_SHORT");
    if (out.serial.empty()) {
        out.serial = prop(d, "ID_SERIAL");
    }
    out.bus = prop(d, "ID_BUS");
    out.usb = is_usb_disk(d);
    out.removable = attr(d, "removable") == "1";
    const char* devtype = udev_device_get_devtype(d);
    out.whole_disk = devtype && std::strcmp(devtype, "disk") == 0;
    for (char& ch : out.model) {
        if (ch == '_') {
            ch = ' ';
        }
    }
    return out;
}

}  // namespace

std::vector<Device> list_block_disks() {
    Udev udev;
    udev_enumerate* en = udev_enumerate_new(udev.u);
    if (!en) {
        throw Error("udev_enumerate_new failed");
    }
    udev_enumerate_add_match_subsystem(en, "block");
    udev_enumerate_add_match_property(en, "DEVTYPE", "disk");
    udev_enumerate_scan_devices(en);

    std::vector<Device> out;
    udev_list_entry* entry = udev_enumerate_get_list_entry(en);
    udev_list_entry* item;
    udev_list_entry_foreach(item, entry) {
        const char* path = udev_list_entry_get_name(item);
        udev_device* d = udev_device_new_from_syspath(udev.u, path);
        if (!d) {
            continue;
        }
        const char* sysname = udev_device_get_sysname(d);
        if (skip_sysname(sysname) || !udev_device_get_devnode(d)) {
            udev_device_unref(d);
            continue;
        }
        out.push_back(from_udev(d));
        udev_device_unref(d);
    }
    udev_enumerate_unref(en);

    std::sort(out.begin(), out.end(), [](const Device& a, const Device& b) { return a.path < b.path; });
    return out;
}

Device inspect_device(const std::string& path) {
    char* resolved = realpath(path.c_str(), nullptr);
    if (!resolved) {
        throw Error("cannot resolve " + path + ": " + std::strerror(errno));
    }
    std::string real(resolved);
    std::free(resolved);

    struct stat st {};
    if (stat(real.c_str(), &st) < 0) {
        throw Error("cannot stat " + real + ": " + std::strerror(errno));
    }
    if (!S_ISBLK(st.st_mode)) {
        throw Error(real + " is not a block device");
    }

    Udev udev;
    udev_device* d = udev_device_new_from_devnum(udev.u, 'b', st.st_rdev);
    if (!d) {
        throw Error("udev has no record for " + real);
    }
    Device dev = from_udev(d);
    const char* devtype = udev_device_get_devtype(d);
    if (devtype && std::strcmp(devtype, "partition") == 0) {
        dev.whole_disk = false;
    }
    udev_device_unref(d);
    if (dev.path.empty()) {
        dev.path = real;
    }
    return dev;
}

std::string default_usb_disk() {
    std::vector<Device> usb;
    for (const auto& d : list_block_disks()) {
        if (d.usb && d.whole_disk) {
            usb.push_back(d);
        }
    }
    if (usb.empty()) {
        throw Error("no USB disk found — plug one in, or pass the device path (ezwin list)");
    }
    if (usb.size() > 1) {
        print_device_table(usb, false);
        throw Error("multiple USB disks — pass the device (for example /dev/sdb)");
    }
    info("using " + usb[0].path + "  " + format_bytes(usb[0].size_bytes) +
         (usb[0].model.empty() ? "" : "  " + usb[0].model));
    return usb[0].path;
}

void print_device_table(const std::vector<Device>& disks, bool usb_only) {
    bool any = false;
    std::cout << std::left << std::setw(16) << "DEVICE" << std::setw(10) << "SIZE" << std::setw(8)
              << "BUS" << std::setw(8) << "USB" << "MODEL\n";
    for (const auto& d : disks) {
        if (usb_only && !d.usb) {
            continue;
        }
        any = true;
        std::cout << std::left << std::setw(16) << d.path << std::setw(10)
                  << format_bytes(d.size_bytes) << std::setw(8)
                  << (d.bus.empty() ? "-" : d.bus) << std::setw(8) << (d.usb ? "yes" : "no")
                  << (d.vendor.empty() ? "" : d.vendor + " ") << d.model << '\n';
    }
    if (!any) {
        if (usb_only) {
            std::cout << "(no USB disks found — plug one in, or pass --all)\n";
        } else {
            std::cout << "(no disks found)\n";
        }
    }
}

}  // namespace ezwin
