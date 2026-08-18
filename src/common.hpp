#pragma once

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ezwin {

constexpr const char* kVersion = "0.5.0";
constexpr uint64_t kFat32MaxFile = 0xFFFFFFFFull;  // 4 GiB - 1
constexpr uint64_t kDefaultSplitSize = 3800ull * 1024ull * 1024ull;
constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;
// Microsoft Media Creation Tool: `create partition primary size=32768` (32 GiB).
constexpr uint64_t kMctFat32Bytes = 32ull * kGiB;

enum class Layout {
    Fat32,     // MBR single FAT32, MCT-style; split oversized WIM
    UefiNtfs,  // GPT NTFS first + UEFI:NTFS stub (Rufus Win11 default)
    MbrNtfs,   // MBR NTFS first + UEFI:NTFS stub (WoeUSB-style)
    Dual,      // MBR FAT32 boot + NTFS payload
};

struct Options {
    enum class Cmd { Help, Version, List, Flash };

    Cmd cmd{Cmd::Help};
    Layout layout{Layout::Fat32};
    std::string iso;
    std::string device;
    std::string label;  // empty: ESD-USB for Fat32, EZWIN otherwise
    uint64_t split_size{kDefaultSplitSize};
    bool yes{false};
    bool force{false};
    bool dry_run{false};
    bool verbose{false};
    bool color{true};
    bool list_all{false};
};

inline uint64_t align_up(uint64_t n, uint64_t a) {
    if (a == 0) {
        return n;
    }
    return (n + a - 1) / a * a;
}

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

extern std::atomic<bool> g_stop;

void init_tty(bool color_enabled);
void set_verbose(bool v);
void install_signal_handlers();
void check_stop();

void debug(std::string_view msg);
void info(std::string_view msg);
void warn(std::string_view msg);
void error(std::string_view msg);

bool stdout_is_tty();
bool stderr_is_tty();

std::string format_bytes(uint64_t n);
std::string ascii_lower(std::string s);
bool iequals(std::string_view a, std::string_view b);
bool is_install_payload_name(std::string_view filename);

std::string which(const std::string& name);
int run_cmd(const std::vector<std::string>& argv, std::string* err_out = nullptr);
void run_cmd_checked(const std::vector<std::string>& argv);

std::string fat_label(std::string label);
std::string ntfs_label(std::string label);
std::string default_volume_label(Layout layout);
std::string find_bundled_file(const std::string& name);
const char* layout_name(Layout layout);

class Progress {
public:
    explicit Progress(std::string prefix);
    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;
    ~Progress();

    void set_total(uint64_t total);
    void set_file_count(uint64_t total);
    void set_item(std::string name);
    void add(uint64_t n);
    void set(uint64_t done, uint64_t total);
    void finish(std::string_view suffix = {});
    void undraw();
    void redraw();

private:
    void draw(bool force);
    void hide_cursor(bool hide);
    int term_width() const;
    std::string make_bar(int width, double frac) const;
    std::string prefix_;
    std::string item_;
    uint64_t done_{0};
    uint64_t total_{0};
    uint64_t files_done_{0};
    uint64_t files_total_{0};
    uint64_t start_ns_{0};
    uint64_t last_draw_ns_{0};
    uint64_t last_sample_ns_{0};
    uint64_t last_sample_done_{0};
    double ema_bps_{0};
    int drawn_lines_{0};
    bool finished_{false};
    bool tty_{false};
    bool utf8_{false};
    bool cursor_hidden_{false};
};

}  // namespace ezwin
