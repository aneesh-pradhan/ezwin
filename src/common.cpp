#include "common.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace ezwin {
namespace {

bool g_color = false;
bool g_verbose = false;
bool g_tty_err = false;
bool g_tty_out = false;
Progress* g_live = nullptr;

constexpr const char* kReset = "\033[0m";
constexpr const char* kBold = "\033[1m";
constexpr const char* kDim = "\033[2m";
constexpr const char* kRed = "\033[31m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kCyan = "\033[36m";

const char* c(const char* code) { return g_color ? code : ""; }

uint64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
    if (g_tty_err) {
        const char show[] = "\033[?25h";
        (void)!write(STDERR_FILENO, show, sizeof(show) - 1);
    }
}

bool locale_is_utf8() {
    for (const char* key : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        const char* v = std::getenv(key);
        if (v && (std::strstr(v, "UTF-8") || std::strstr(v, "utf8") || std::strstr(v, "UTF8"))) {
            return true;
        }
    }
    return false;
}

std::string format_rate(double bps) {
    if (bps < 0) {
        bps = 0;
    }
    return format_bytes(static_cast<uint64_t>(bps)) + "/s";
}

std::string format_duration(double seconds) {
    if (seconds < 0 || !std::isfinite(seconds)) {
        return "--";
    }
    const int s = static_cast<int>(seconds + 0.5);
    char buf[32];
    if (s < 60) {
        std::snprintf(buf, sizeof(buf), "%ds", s);
    } else if (s < 3600) {
        std::snprintf(buf, sizeof(buf), "%dm %02ds", s / 60, s % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%dh %02dm", s / 3600, (s % 3600) / 60);
    }
    return buf;
}

std::string ellipsize(std::string s, std::size_t max, bool utf8) {
    if (s.size() <= max) {
        return s;
    }
    if (max < 5) {
        return s.substr(0, max);
    }
    const std::string mark = utf8 ? "…" : "...";
    const std::size_t mark_len = utf8 ? 1 : 3;
    const std::size_t keep = (max - mark_len) / 2;
    return s.substr(0, keep) + mark + s.substr(s.size() - (max - keep - mark_len));
}

void pause_live_bar() {
    if (g_live) {
        g_live->undraw();
    }
}

}  // namespace

std::atomic<bool> g_stop{false};

void init_tty(bool color_enabled) {
    g_tty_out = isatty(STDOUT_FILENO) == 1;
    g_tty_err = isatty(STDERR_FILENO) == 1;
    const char* no_color = std::getenv("NO_COLOR");
    g_color = color_enabled && g_tty_err && (no_color == nullptr || no_color[0] == '\0');
}

void set_verbose(bool v) { g_verbose = v; }

void install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

void check_stop() {
    if (g_stop.load(std::memory_order_relaxed)) {
        throw Error("interrupted");
    }
}

void debug(std::string_view msg) {
    if (!g_verbose) {
        return;
    }
    pause_live_bar();
    std::cerr << c(kDim) << "    " << msg << c(kReset) << '\n';
    if (g_live) {
        g_live->redraw();
    }
}

void info(std::string_view msg) {
    pause_live_bar();
    std::cerr << c(kBold) << c(kCyan) << " * " << c(kReset) << msg << '\n';
    if (g_live) {
        g_live->redraw();
    }
}

void warn(std::string_view msg) {
    pause_live_bar();
    std::cerr << c(kBold) << c(kYellow) << " ! " << c(kReset) << msg << '\n';
    if (g_live) {
        g_live->redraw();
    }
}

void error(std::string_view msg) {
    pause_live_bar();
    std::cerr << c(kBold) << c(kRed) << " x " << c(kReset) << msg << '\n';
    if (g_live) {
        g_live->redraw();
    }
}

bool stdout_is_tty() { return g_tty_out; }
bool stderr_is_tty() { return g_tty_err; }

std::string format_bytes(uint64_t n) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    size_t u = 0;
    while (v >= 1024.0 && u + 1 < 5) {
        v /= 1024.0;
        ++u;
    }
    char buf[64];
    if (u == 0) {
        std::snprintf(buf, sizeof(buf), "%llu %s", static_cast<unsigned long long>(n), units[u]);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    }
    return buf;
}

std::string ascii_lower(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool is_install_payload_name(std::string_view filename) {
    const std::string n = ascii_lower(std::string(filename));
    if (n == "install.wim" || n == "install.esd") {
        return true;
    }
    return n.starts_with("install") && n.ends_with(".swm");
}

std::string which(const std::string& name) {
    if (name.find('/') != std::string::npos) {
        if (access(name.c_str(), X_OK) == 0) {
            return name;
        }
        return {};
    }
    std::string path;
    if (const char* p = std::getenv("PATH")) {
        path = p;
    }
    path += ":/usr/sbin:/sbin:/usr/local/sbin";
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) {
            continue;
        }
        std::string cand = dir + "/" + name;
        if (access(cand.c_str(), X_OK) == 0) {
            return cand;
        }
    }
    return {};
}

int run_cmd(const std::vector<std::string>& argv, std::string* err_out) {
    if (argv.empty()) {
        throw Error("internal: empty command");
    }
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        throw Error(std::string("pipe: ") + std::strerror(errno));
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        throw Error(std::string("fork: ") + std::strerror(errno));
    }
    if (pid == 0) {
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) {
            cargv.push_back(const_cast<char*>(a.c_str()));
        }
        cargv.push_back(nullptr);
        execv(cargv[0], cargv.data());
        _exit(127);
    }

    close(pipefd[1]);
    std::string err;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        err.append(buf, static_cast<size_t>(n));
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            throw Error(std::string("waitpid: ") + std::strerror(errno));
        }
    }
    if (err_out) {
        *err_out = std::move(err);
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 1;
}

void run_cmd_checked(const std::vector<std::string>& argv) {
    std::string err;
    int rc = run_cmd(argv, &err);
    if (rc == 127) {
        throw Error("cannot execute " + argv[0] + " (not found?)");
    }
    if (rc != 0) {
        if (!err.empty() && err.back() == '\n') {
            err.pop_back();
        }
        throw Error(argv[0] + " failed: " + (err.empty() ? "exit " + std::to_string(rc) : err));
    }
}

std::string fat_label(std::string label) {
    std::string out;
    out.reserve(11);
    for (unsigned char ch : label) {
        if (out.size() >= 11) {
            break;
        }
        if (ch <= 32 || std::strchr("*?/\\|,;:+=<>[]\"", ch)) {
            continue;
        }
        out.push_back(static_cast<char>(std::toupper(ch)));
    }
    if (out.empty()) {
        out = "ESD-USB";
    }
    return out;
}

std::string ntfs_label(std::string label) {
    while (!label.empty() && (label.back() == ' ' || label.back() == '.')) {
        label.pop_back();
    }
    if (label.size() > 32) {
        label.resize(32);
    }
    if (label.empty()) {
        label = "EZWIN";
    }
    return label;
}

std::string default_volume_label(Layout layout) {
    return layout == Layout::Fat32 ? "ESD-USB" : "EZWIN";
}

std::string find_bundled_file(const std::string& name) {
    std::vector<std::filesystem::path> dirs;
    if (const char* env = std::getenv("EZWIN_DATA_DIR")) {
        dirs.emplace_back(env);
    }
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        const std::filesystem::path exe = std::filesystem::path(buf).parent_path();
        dirs.push_back(exe / "res");
        dirs.push_back(exe);
        dirs.push_back(exe / ".." / "share" / "ezwin");
    }
    dirs.emplace_back("/usr/local/share/ezwin");
    dirs.emplace_back("/usr/share/ezwin");

    std::error_code ec;
    for (const auto& d : dirs) {
        const auto p = std::filesystem::weakly_canonical(d / name, ec);
        if (!ec && std::filesystem::is_regular_file(p)) {
            return p.string();
        }
    }
    throw Error("cannot find bundled file '" + name +
                "' (set EZWIN_DATA_DIR or install ezwin data files)");
}

const char* layout_name(Layout layout) {
    switch (layout) {
        case Layout::UefiNtfs:
            return "GPT: NTFS payload first + UEFI:NTFS stub (Rufus-style, UEFI-only)";
        case Layout::MbrNtfs:
            return "MBR: NTFS payload first + UEFI:NTFS stub (WoeUSB-style)";
        case Layout::Dual:
            return "MBR dual: FAT32 boot + NTFS payload";
        case Layout::Fat32:
            return "MBR + FAT32 (Media Creation Tool-style; split WIM if needed)";
    }
    return "unknown";
}

Progress::Progress(std::string prefix)
    : prefix_(std::move(prefix)),
      start_ns_(now_ns()),
      last_draw_ns_(0),
      last_sample_ns_(now_ns()),
      tty_(g_tty_err),
      utf8_(locale_is_utf8()) {
    g_live = this;
}

Progress::~Progress() {
    if (!finished_) {
        undraw();
    }
    hide_cursor(false);
    if (g_live == this) {
        g_live = nullptr;
    }
}

void Progress::hide_cursor(bool hide) {
    if (!tty_) {
        return;
    }
    if (hide && !cursor_hidden_) {
        std::cerr << "\033[?25l" << std::flush;
        cursor_hidden_ = true;
    } else if (!hide && cursor_hidden_) {
        std::cerr << "\033[?25h" << std::flush;
        cursor_hidden_ = false;
    }
}

int Progress::term_width() const {
    struct winsize ws {};
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 40) {
        return ws.ws_col;
    }
    return 80;
}

std::string Progress::make_bar(int width, double frac) const {
    if (width < 8) {
        width = 8;
    }
    if (frac < 0) {
        frac = 0;
    }
    if (frac > 1) {
        frac = 1;
    }

    std::string out;
    if (utf8_) {
        static const char* frac_blocks[] = {"▏", "▎", "▍", "▌", "▋", "▊", "▉"};
        const double cells = frac * static_cast<double>(width);
        const int full = static_cast<int>(cells);
        for (int i = 0; i < width; ++i) {
            if (i < full) {
                out += "█";
            } else if (i == full && frac < 1.0) {
                const int sub = static_cast<int>((cells - full) * 7.0);
                out += (sub <= 0) ? "░" : frac_blocks[sub > 6 ? 6 : sub];
            } else {
                out += "░";
            }
        }
    } else {
        const int fill = static_cast<int>(frac * width + 0.5);
        out.assign(static_cast<size_t>(width), '-');
        for (int i = 0; i < fill && i < width; ++i) {
            out[static_cast<size_t>(i)] = '=';
        }
        if (fill > 0 && fill < width) {
            out[static_cast<size_t>(fill - 1)] = '>';
        }
    }
    return out;
}

void Progress::set_total(uint64_t total) { total_ = total; }

void Progress::set_file_count(uint64_t total) { files_total_ = total; }

void Progress::set_item(std::string name) {
    item_ = std::move(name);
    if (files_total_ > 0) {
        ++files_done_;
    }
    draw(true);
}

void Progress::add(uint64_t n) {
    done_ += n;
    draw(false);
}

void Progress::set(uint64_t done, uint64_t total) {
    done_ = done;
    total_ = total;
    draw(false);
}

void Progress::undraw() {
    if (!tty_ || drawn_lines_ <= 0) {
        return;
    }
    std::cerr << "\033[" << drawn_lines_ << "A\033[J" << std::flush;
    drawn_lines_ = 0;
}

void Progress::redraw() { draw(true); }

void Progress::finish(std::string_view suffix) {
    if (finished_) {
        return;
    }
    finished_ = true;
    const uint64_t t = now_ns();
    const double elapsed = (t > start_ns_) ? static_cast<double>(t - start_ns_) / 1e9 : 0.001;
    const double avg = static_cast<double>(done_) / elapsed;
    undraw();
    hide_cursor(false);
    if (g_live == this) {
        g_live = nullptr;
    }

    std::cerr << c(kBold) << c(kGreen) << " + " << c(kReset) << prefix_;
    std::cerr << c(kDim) << "  " << format_bytes(done_);
    if (total_) {
        std::cerr << " / " << format_bytes(total_);
    }
    std::cerr << "  in " << format_duration(elapsed) << "  avg " << format_rate(avg);
    if (!suffix.empty()) {
        std::cerr << "  " << suffix;
    }
    std::cerr << c(kReset) << '\n';
}

void Progress::draw(bool force) {
    check_stop();
    const uint64_t t = now_ns();
    if (!force && t - last_draw_ns_ < 80ull * 1000000ull) {
        return;
    }
    last_draw_ns_ = t;

    const double elapsed = (t > start_ns_) ? static_cast<double>(t - start_ns_) / 1e9 : 0.001;
    const double sample_dt =
        (t > last_sample_ns_) ? static_cast<double>(t - last_sample_ns_) / 1e9 : 0;
    if (sample_dt >= 0.15) {
        const double instant =
            static_cast<double>(done_ - last_sample_done_) / (sample_dt > 0 ? sample_dt : 0.001);
        ema_bps_ = (ema_bps_ <= 0) ? instant : (ema_bps_ * 0.72 + instant * 0.28);
        last_sample_ns_ = t;
        last_sample_done_ = done_;
    }
    const double bps = (ema_bps_ > 1) ? ema_bps_ : (static_cast<double>(done_) / elapsed);
    const double frac = (total_ > 0) ? static_cast<double>(done_) / static_cast<double>(total_) : 0;
    const double pct = frac * 100.0;
    const double remain = (bps > 1 && total_ > done_) ? static_cast<double>(total_ - done_) / bps : -1;

    if (!tty_) {
        static uint64_t last_plain = 0;
        if (force || t - last_plain > 2000000000ull) {
            last_plain = t;
            std::cerr << prefix_;
            if (!item_.empty()) {
                std::cerr << "  " << item_;
            }
            std::cerr << "  " << static_cast<int>(pct + 0.5) << "%  " << format_bytes(done_);
            if (total_) {
                std::cerr << '/' << format_bytes(total_);
            }
            std::cerr << "  " << format_rate(bps) << '\n';
        }
        return;
    }

    hide_cursor(true);
    const int cols = term_width();
    const int bar_width = std::max(18, std::min(40, cols - 18));

    std::string title = std::string("  ") + prefix_;
    if (!item_.empty()) {
        title += std::string(c(kDim)) + "  ·  " + std::string(c(kReset)) +
                 ellipsize(item_, static_cast<size_t>(std::max(12, cols - 24)), utf8_);
    }

    char pctbuf[16];
    std::snprintf(pctbuf, sizeof(pctbuf), "%5.1f%%", pct);
    std::string bar_line = std::string("  ") + std::string(c(kCyan)) + (utf8_ ? "│" : "[") +
                           make_bar(bar_width, frac) + (utf8_ ? "│" : "]") + std::string(c(kReset)) +
                           "  " + std::string(c(kBold)) + pctbuf + std::string(c(kReset));

    std::string stats = std::string("  ") + format_bytes(done_);
    if (total_) {
        stats += " / " + format_bytes(total_);
    }
    stats += std::string(c(kDim)) + "  ·  " + std::string(c(kReset)) + format_rate(bps);
    stats += std::string(c(kDim)) + "  ·  " + std::string(c(kReset)) + "ETA " + format_duration(remain);
    stats += std::string(c(kDim)) + "  ·  " + std::string(c(kReset)) + format_duration(elapsed) +
             " elapsed";
    if (files_total_ > 0) {
        stats += std::string(c(kDim)) + "  ·  " + std::string(c(kReset)) +
                 std::to_string(std::min(files_done_, files_total_)) + "/" +
                 std::to_string(files_total_) + " files";
    }

    if (drawn_lines_ > 0) {
        std::cerr << "\033[" << drawn_lines_ << "A";
    }
    std::cerr << "\033[2K\r" << title << '\n';
    std::cerr << "\033[2K\r" << bar_line << '\n';
    std::cerr << "\033[2K\r" << stats << '\n' << std::flush;
    drawn_lines_ = 3;
}

}  // namespace ezwin
