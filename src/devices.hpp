#pragma once

#include "common.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ezwin {

struct Device {
    std::string path;
    std::string sysname;
    uint64_t size_bytes{0};
    std::string model;
    std::string vendor;
    std::string serial;
    std::string bus;
    bool usb{false};
    bool removable{false};
    bool whole_disk{true};
};

std::vector<Device> list_block_disks();
Device inspect_device(const std::string& path);
std::string default_usb_disk();
void print_device_table(const std::vector<Device>& disks, bool usb_only);

}  // namespace ezwin
