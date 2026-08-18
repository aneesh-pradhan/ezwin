#pragma once

#include "common.hpp"

#include <filesystem>
#include <string>

namespace ezwin {

std::string partition_node(const std::string& disk, unsigned n);

uint64_t block_dev_size(const std::string& path);
void unmount_disk(const std::string& disk);
void wipe_partitioning(const std::string& disk);
void reread_partition_table(const std::string& disk);
// part_bytes 0 = remainder of the disk after a 1 MiB gap (MCT uses 32 GiB when larger).
void create_mbr_fat32(const std::string& disk, uint64_t part_bytes = 0);
void create_mbr_dual(const std::string& disk, uint64_t fat_bytes);
void create_mbr_uefi_ntfs(const std::string& disk, uint64_t fat_bytes);
void create_gpt_uefi_ntfs(const std::string& disk, uint64_t fat_bytes);
void disable_csm_mbr(const std::string& disk);
void wait_for_partition(const std::string& part, uint64_t min_bytes = 0, uint64_t max_bytes = 0);
void format_fat32(const std::string& part, const std::string& label, bool mct_style = false);
void format_ntfs(const std::string& part, const std::string& label);
void write_raw_image(const std::string& part, const std::string& image_path);

class TempDir {
public:
    TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir();

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class Mount {
public:
    Mount() = default;
    Mount(const Mount&) = delete;
    Mount& operator=(const Mount&) = delete;
    ~Mount();

    void mount_iso(const std::string& iso, const std::filesystem::path& target);
    void mount_vfat(const std::string& device, const std::filesystem::path& target);
    void mount_ntfs(const std::string& device, const std::filesystem::path& target);
    void sync() const;
    void umount();
    const std::filesystem::path& root() const { return target_; }

private:
    void mount_fs(const std::string& source, const std::filesystem::path& target,
                  const char* fstype, const char* options);

    std::filesystem::path target_;
    bool active_{false};
};

}  // namespace ezwin
