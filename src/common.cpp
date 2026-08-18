#include "common.hpp"

#include <chrono>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace ezwin {
namespace {

bool g_color = false;
bool g_verbose = false;
bool g_tty_err = false;
bool g_tty_out = false;

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

void on_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

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
    std::cerr << c(kDim) << "    " << msg << c(kReset) << '\n';
}

void info(std::string_view msg) {
    std::cerr << c(kBold) << c(kCyan) << " * " << c(kReset) << msg << '\n';
}

void warn(std::string_view msg) {
    std::cerr << c(kBold) << c(kYellow) << " ! " << c(kReset) << msg << '\n';
}

void error(std::string_view msg) {
    std::cerr << c(kBold) << c(kRed) << " x " << c(kReset) << msg << '\n';
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
    return iequals(filename, "install.wim") || iequals(filename, "install.esd");
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
        out = "EZWIN";
    }
    return out;
}

Progress::Progress(std::string prefix)
    : prefix_(std::move(prefix)), start_ns_(now_ns()), last_draw_ns_(0), tty_(g_tty_err) {}

Progress::~Progress() {
    if (!finished_ && tty_) {
        std::cerr << '\n';
    }
}

void Progress::set_total(uint64_t total) { total_ = total; }

void Progress::add(uint64_t n) {
    done_ += n;
    draw(false);
}

void Progress::set(uint64_t done, uint64_t total) {
    done_ = done;
    total_ = total;
    draw(false);
}

void Progress::finish(std::string_view suffix) {
    if (finished_) {
        return;
    }
    finished_ = true;
    draw(true);
    if (tty_) {
        std::cerr << "\033[2K\r";
    }
    std::cerr << c(kBold) << c(kGreen) << " + " << c(kReset) << prefix_;
    if (!suffix.empty()) {
        std::cerr << "  " << c(kDim) << suffix << c(kReset);
    }
    std::cerr << '\n';
}

void Progress::draw(bool force) {
    check_stop();
    const uint64_t t = now_ns();
    if (!force && t - last_draw_ns_ < 50ull * 1000000ull) {
        return;
    }
    last_draw_ns_ = t;

    const double elapsed = (t > start_ns_) ? static_cast<double>(t - start_ns_) / 1e9 : 0.001;
    const double bps = static_cast<double>(done_) / elapsed;
    const int pct = (total_ > 0) ? static_cast<int>((done_ * 100ull) / total_) : 0;

    if (!tty_) {
        if (force) {
            std::cerr << prefix_ << ' ' << pct << "% " << format_bytes(done_);
            if (total_) {
                std::cerr << '/' << format_bytes(total_);
            }
            std::cerr << '\n';
        }
        return;
    }

    const int width = 24;
    int fill = 0;
    if (total_ > 0) {
        fill = static_cast<int>((done_ * static_cast<uint64_t>(width)) / total_);
        if (fill > width) {
            fill = width;
        }
    }
    std::string bar(static_cast<size_t>(width), '-');
    for (int i = 0; i < fill; ++i) {
        bar[static_cast<size_t>(i)] = '=';
    }
    if (fill > 0 && fill < width) {
        bar[static_cast<size_t>(fill - 1)] = '>';
    }

    std::cerr << '\r' << c(kCyan) << "   [" << bar << "] " << c(kReset);
    char pctbuf[8];
    std::snprintf(pctbuf, sizeof(pctbuf), "%3d%%", pct);
    std::cerr << pctbuf << "  " << format_bytes(done_);
    if (total_) {
        std::cerr << " / " << format_bytes(total_);
    }
    std::cerr << "  " << format_bytes(static_cast<uint64_t>(bps)) << "/s   " << std::flush;
}

}  // namespace ezwin
