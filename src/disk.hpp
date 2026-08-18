#pragma once

#include "common.hpp"

#include <filesystem>
#include <string>

namespace ezwin {

std::string partition_node(const std::string& disk, unsigned n);

void unmount_disk(const std::string& disk);
void wipe_partitioning(const std::string& disk);
void create_gpt_esp(const std::string& disk, const std::string& part_name);
void wait_for_partition(const std::string& part);
void format_fat32(const std::string& part, const std::string& label);

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
