// Necrowares's Video Memory Tester
// Copyright (C) 2025 by Necroware
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <cli.hpp>
#include <dpmi.hpp>
#include <log.hpp>
#include <vbe.hpp>
#include <version.h>

#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

using error = std::runtime_error;

constexpr auto bits_per_byte = 8u;
constexpr auto bytes_per_mb = std::uint64_t{1024u * 1024u};

auto find_best_mode() -> vbe::mode_info_t {

    for (const auto& mode : vbe::get_modes()) {
        if (mode.width >= 640 && mode.height >= 480 &&
            mode.bits_per_pixel >= 16) {
            return mode;
        }
    }
    throw error("No suitable VESA mode found.");
}

auto test_video_memory(const std::uint16_t mode_id,
                       const std::uint16_t bus_width,
                       const std::uint8_t num_chips,
                       const std::size_t size) {

    const auto fb = vbe::framebuffer{mode_id, size};
    const auto bus_width_in_bytes = bus_width / bits_per_byte;
    const auto bytes_per_chip = bus_width_in_bytes / num_chips;

    auto result = std::vector<bool>(num_chips, true);
    auto block = std::vector<std::uint8_t>(bus_width_in_bytes * 1024u, 0u);

    const auto test_pass = [&](const auto& generator) {
        for (auto addr = 0u; addr < size; addr += block.size()) {
            const auto rest = std::min(size - addr, block.size());
            for (auto i = 0u; i < rest; i++) {
                block[i] = generator(i);
            }
            fb.write(addr, block.data(), rest);
        }
        for (auto addr = 0u; addr < size; addr += block.size()) {
            const auto rest = std::min(size - addr, block.size());
            fb.read(addr, block.data(), rest);
            for (auto i = 0u; i < rest; i++) {
                if (block[i] != generator(i)) {
                    result[(i % bus_width_in_bytes) / bytes_per_chip] = false;
                }
            }
        }
    };

    test_pass([](std::uint8_t) { return 0x00; });
    test_pass([](std::uint8_t) { return 0xFF; });
    test_pass([](std::uint8_t) { return 0xAA; });
    test_pass([](std::uint8_t) { return 0x55; });
    test_pass([](std::uint8_t offs) { return offs; });

    return result;
}

void check_arguments(const int bus_width, const int num_chips) {
    if (bus_width <= 0 ||
        bus_width > std::numeric_limits<std::uint16_t>::max()) {
        throw error("Memory bus width must be between 1 and 65535 bits");
    }
    if (num_chips <= 0 ||
        num_chips > std::numeric_limits<std::uint8_t>::max()) {
        throw error("Number of chips must be between 1 and 255");
    }
    if (bus_width < bits_per_byte) {
        throw error("Memory bus width has to be at least 8-bit");
    }
    if (bus_width / num_chips < bits_per_byte) {
        throw error("Cards with less than a byte per chip are not supported");
    }
}

auto resolve_total_memory(const int size_override) -> std::size_t {
    if (size_override == 0) {
        return vbe::get_total_memory_size();
    }
    if (size_override < 0) {
        throw error("Memory size override must be a positive value");
    }
    // VBE reports total memory as a count of 64KB blocks in a 16-bit field,
    // so no VBE 2.0 card can ever legitimately have more memory than this.
    constexpr auto max_vbe_memory =
        std::uint64_t{std::numeric_limits<std::uint16_t>::max()} * 64u *
        1024u;
    const auto bytes =
        static_cast<std::uint64_t>(size_override) * bytes_per_mb;
    if (bytes > max_vbe_memory) {
        throw error("Memory size override exceeds the maximum size a VBE "
                    "2.0 card can report (4095 MB)");
    }
    return static_cast<std::size_t>(bytes);
}

auto wants_log_flag(int argc, const char* argv[]) -> bool {
    auto requested = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--log") {
            requested = true;
        } else if (arg == "--log=false" || arg == "--log=0") {
            requested = false;
        } else if (arg.starts_with("--log=")) {
            requested = true;
        }
    }
    return requested;
}

void run(const int bus_width, const int num_chips, const int size_override) {

    check_arguments(bus_width, num_chips);

    const auto oem_info = vbe::get_oem_info();
    const auto total_memory = resolve_total_memory(size_override);
    const auto mode = find_best_mode();

    log("Test Info:");
    log("----------");
    log("OEM String: %s", oem_info.description);
    log("Vendor: %s", oem_info.vendor_name);
    log("Product: %s", oem_info.product_name);
    log("Revision: %s", oem_info.revision_name);
    log("Total Memory: %dMB%s", total_memory / (1024u * 1024u),
        size_override > 0 ? " (manual override)" : "");
    log("Memory bus: %d-bit", bus_width);
    log("Number of chips: %d", num_chips);
    log("Test video mode: %#X [%dx%dx%d]", mode.id, mode.width, mode.height,
        mode.bits_per_pixel);

    std::puts("\nThe test can take up to several minutes");
    std::puts("Press [ENTER] to continue");
    getchar();
    const auto test_result =
        test_video_memory(mode.id, bus_width, num_chips, total_memory);

    log("\nResult:");
    log("----------");
    for (auto i = 0u; i < test_result.size(); i++) {
        log("Chip %d: %s", i, test_result[i] ? "OK" : "BAD");
    }
}

} // namespace

int main(int argc, const char* argv[]) {
    try {
        // Detect --log from the raw arguments before parsing, so that any
        // parsing or validation error below is captured in the log file too.
        if (wants_log_flag(argc, argv)) {
            enable_log_file();
        }

        const auto params = std::vector<cli::param_decl>{
            {"chips", true, 0, "Number of chips on the card"},
            {"bus", true, 0, "Memory bus width in bits"},
            {"size", false, 0, "Manual override for video memory size in MB"},
            {"log", false, false, "Enable logging to nwvmt.log file"},
        };

        const auto args = cli::args_parser{argc, argv, params};
        if (args.wants_help()) {
            args.print_usage();
            disable_log_file();
            return EXIT_SUCCESS;
        }

        if (!args.validate()) {
            for (const auto& name : args.missing_required()) {
                std::fprintf(stderr, "error: missing required parameter: --%s\n",
                             name.c_str());
            }
            args.print_usage();
            disable_log_file();
            return EXIT_FAILURE;
        }

        log("Necroware's Video Memory Tester");
        log("Version " PROJECT_VERSION " (build date " PROJECT_BUILD_DATE ")\n");

        run(args.get<int>("bus"), args.get<int>("chips"),
            args.get<int>("size"));

        disable_log_file();
        return EXIT_SUCCESS;
    } catch (std::exception& ex) {
        log("error: %s", ex.what());
        disable_log_file();
        return EXIT_FAILURE;
    }
}
