#include <filesystem>
#include <stdexcept>
#include <string>

// Exercise libc++/libc++abi and exception unwinding without touching any disks.
int main() {
    try {
        const std::filesystem::path path("LLVM/toolchain");
        throw std::runtime_error(path.filename().string());
    } catch (const std::exception& error) {
        return std::string(error.what()) == "toolchain" ? 0 : 1;
    }
}
