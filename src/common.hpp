#pragma once

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ezwin {

constexpr const char* kVersion = "0.1.0";
constexpr uint64_t kFat32MaxFile = 0xFFFFFFFFull;          // 4 GiB - 1
constexpr uint64_t kDefaultSplitSize = 3800ull * 1024ull * 1024ull;

struct Options {
    enum class Cmd { Help, Version, List, Flash };

    Cmd cmd{Cmd::Help};
    std::string iso;
    std::string device;
    std::string label{"EZWIN"};
    uint64_t split_size{kDefaultSplitSize};
    bool yes{false};
    bool force{false};
    bool dry_run{false};
    bool verbose{false};
    bool color{true};
    bool list_all{false};
};

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

class Progress {
public:
    explicit Progress(std::string prefix);
    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;
    ~Progress();

    void set_total(uint64_t total);
    void add(uint64_t n);
    void set(uint64_t done, uint64_t total);
    void finish(std::string_view suffix = {});

private:
    void draw(bool force);
    std::string prefix_;
    uint64_t done_{0};
    uint64_t total_{0};
    uint64_t start_ns_{0};
    uint64_t last_draw_ns_{0};
    bool finished_{false};
    bool tty_{false};
};

}  // namespace ezwin
