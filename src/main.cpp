#include "common.hpp"
#include "devices.hpp"
#include "flash.hpp"

#include <cstdlib>
#include <getopt.h>
#include <iostream>
#include <unistd.h>
#include <vector>

namespace ezwin {
namespace {

void usage(std::ostream& out) {
    out << "ezwin " << kVersion << " — native Windows 11 USB flasher for Linux\n"
        << "\n"
        << "Usage:\n"
        << "  ezwin list [--all]\n"
        << "  ezwin flash [options] <windows.iso> [usb-device]\n"
        << "  ezwin [options] <windows.iso> [usb-device]\n"
        << "\n"
        << "If you omit the USB device and exactly one USB disk is plugged in, ezwin uses it.\n"
        << "\n"
        << "Options:\n"
        << "  -y, --yes            Do not prompt for confirmation\n"
        << "  -f, --force          Allow non-USB block devices (dangerous)\n"
        << "  -n, --dry-run        Validate and print the plan without writing\n"
        << "  -v, --verbose        Extra diagnostics\n"
        << "  -L, --label NAME     Volume label (default: ESD-USB)\n"
        << "      --layout MODE    fat32 (default) | ntfs | mbr | dual\n"
        << "      --split-size MB  Max install.swm part size in MiB (fat32 layout, default: 3800)\n"
        << "      --all            With list: show non-USB disks too\n"
        << "      --no-color       Disable ANSI color\n"
        << "  -h, --help           Show this help\n"
        << "  -V, --version        Show version\n"
        << "\n"
        << "Windows 11 ISOs are UDF hybrids, so dd/raw flash does not produce a UEFI-bootable\n"
        << "USB stick. The default matches Microsoft's Media Creation Tool: MBR, one FAT32\n"
        << "partition (32 GiB on larger USBs, labeled ESD-USB), and install.wim split to\n"
        << ".swm parts under the 4 GiB file size limit. Firmware boots \\EFI\\BOOT\\bootx64.efi\n"
        << "from that volume — no second partition.\n"
        << "\n"
        << "  fat32      MBR + FAT32 (default, Microsoft USB tool)\n"
        << "  ntfs       GPT + NTFS first + UEFI:NTFS stub (Rufus-style)\n"
        << "  mbr        MBR + NTFS first + UEFI:NTFS stub (WoeUSB-style; boot the UEFI entry)\n"
        << "  dual       MBR FAT32 boot + NTFS payload\n";
}

Options parse(int argc, char** argv) {
    Options opt;
    opt.color = true;

    static const option long_opts[] = {
        {"help", no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, 'V'},
        {"yes", no_argument, nullptr, 'y'},
        {"force", no_argument, nullptr, 'f'},
        {"dry-run", no_argument, nullptr, 'n'},
        {"verbose", no_argument, nullptr, 'v'},
        {"label", required_argument, nullptr, 'L'},
        {"layout", required_argument, nullptr, 4},
        {"split-size", required_argument, nullptr, 1},
        {"all", no_argument, nullptr, 2},
        {"no-color", no_argument, nullptr, 3},
        {nullptr, 0, nullptr, 0},
    };

    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "hVyfnvL:", long_opts, nullptr)) != -1) {
        switch (c) {
            case 'h':
                opt.cmd = Options::Cmd::Help;
                return opt;
            case 'V':
                opt.cmd = Options::Cmd::Version;
                return opt;
            case 'y':
                opt.yes = true;
                break;
            case 'f':
                opt.force = true;
                break;
            case 'n':
                opt.dry_run = true;
                break;
            case 'v':
                opt.verbose = true;
                break;
            case 'L':
                opt.label = optarg ? optarg : "";
                break;
            case 4: {
                const std::string mode = ascii_lower(optarg ? optarg : "");
                if (mode == "dual" || mode == "esp") {
                    opt.layout = Layout::Dual;
                } else if (mode == "fat32" || mode == "split" || mode == "mct" ||
                           mode == "windows" || mode == "microsoft") {
                    opt.layout = Layout::Fat32;
                } else if (mode == "mbr" || mode == "woeusb" || mode == "mbr-ntfs") {
                    opt.layout = Layout::MbrNtfs;
                } else if (mode == "uefi-ntfs" || mode == "uefintfs" || mode == "ntfs" ||
                           mode == "rufus" || mode == "gpt") {
                    opt.layout = Layout::UefiNtfs;
                } else {
                    throw Error("unknown --layout '" + std::string(optarg ? optarg : "") +
                                "' (use fat32, ntfs, mbr, or dual)");
                }
                break;
            }
            case 1: {
                const long mb = std::strtol(optarg, nullptr, 10);
                if (mb < 100 || mb > 4095) {
                    throw Error("--split-size must be between 100 and 4095 MiB");
                }
                opt.split_size = static_cast<uint64_t>(mb) * 1024ull * 1024ull;
                break;
            }
            case 2:
                opt.list_all = true;
                break;
            case 3:
                opt.color = false;
                break;
            default:
                throw Error("try 'ezwin --help'");
        }
    }

    std::vector<std::string> args;
    for (int i = optind; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    if (args.empty()) {
        opt.cmd = Options::Cmd::Help;
        return opt;
    }

    if (args[0] == "list") {
        opt.cmd = Options::Cmd::List;
        if (args.size() > 1) {
            throw Error("list takes no positional arguments");
        }
        return opt;
    }

    if (args[0] == "flash") {
        if (args.size() == 2) {
            opt.cmd = Options::Cmd::Flash;
            opt.iso = args[1];
            return opt;
        }
        if (args.size() == 3) {
            opt.cmd = Options::Cmd::Flash;
            opt.iso = args[1];
            opt.device = args[2];
            return opt;
        }
        throw Error("usage: ezwin flash <windows.iso> [usb-device]");
    }

    if (args.size() == 1) {
        opt.cmd = Options::Cmd::Flash;
        opt.iso = args[0];
        return opt;
    }

    if (args.size() == 2) {
        opt.cmd = Options::Cmd::Flash;
        opt.iso = args[0];
        opt.device = args[1];
        return opt;
    }

    throw Error("usage: ezwin flash <windows.iso> [usb-device]");
}

int run(int argc, char** argv) {
    Options opt = parse(argc, argv);
    init_tty(opt.color);
    set_verbose(opt.verbose);

    switch (opt.cmd) {
        case Options::Cmd::Help:
            usage(std::cout);
            return 0;
        case Options::Cmd::Version:
            std::cout << "ezwin " << kVersion << '\n';
            return 0;
        case Options::Cmd::List:
            print_device_table(list_block_disks(), !opt.list_all);
            return 0;
        case Options::Cmd::Flash:
            flash_windows_iso(opt);
            return 0;
    }
    return 0;
}

}  // namespace
}  // namespace ezwin

int main(int argc, char** argv) {
    ezwin::install_signal_handlers();
    try {
        return ezwin::run(argc, argv);
    } catch (const ezwin::Error& e) {
        ezwin::init_tty(true);
        ezwin::error(e.what());
        return 1;
    } catch (const std::exception& e) {
        ezwin::init_tty(true);
        ezwin::error(e.what());
        return 1;
    }
}
