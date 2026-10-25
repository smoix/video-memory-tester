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

#include <pc.h>

#include <algorithm>
#include <array>
#include <bit>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using error = std::runtime_error;
using word_t = std::uint32_t;

constexpr auto bits_per_byte = 8u;
constexpr auto bytes_per_mb = std::uint64_t{1024u * 1024u};
constexpr auto max_listed_errors = 16u;
constexpr auto max_listed_passes = 10u;
constexpr auto min_errors_for_address_bits = 16u;

// Murmur3 finalizer. It is a bijection, so every address gets its own
// pseudo random word.
constexpr auto mix(word_t x) -> word_t {
    x ^= x >> 16;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    x *= 0xC2B2AE35u;
    x ^= x >> 16;
    return x;
}

struct pattern_t {
    const char* name;
    // Returns the word expected at a 4-byte aligned address
    word_t (*word_at)(word_t addr, word_t seed);
};

constexpr auto patterns = std::array<pattern_t, 7>{{
    {"0x00", [](word_t, word_t) -> word_t { return 0x00000000; }},
    {"0xFF", [](word_t, word_t) -> word_t { return 0xFFFFFFFF; }},
    {"0xAA", [](word_t, word_t) -> word_t { return 0xAAAAAAAA; }},
    {"0x55", [](word_t, word_t) -> word_t { return 0x55555555; }},
    // Every word holds its own address, so aliased addresses (broken
    // address lines, too large --size) read back a wrong value
    {"address", [](word_t addr, word_t) -> word_t { return addr; }},
    {"~address", [](word_t addr, word_t) -> word_t { return ~addr; }},
    {"random",
     [](word_t addr, word_t seed) -> word_t { return mix(addr ^ seed); }},
}};

struct bad_byte_t {
    std::uint32_t addr;
    std::uint8_t expected;
    std::uint8_t actual;
    std::size_t pattern;
    unsigned pass;
};

struct chip_result_t {
    std::uint64_t errors{};
    // Bits set in addr_and are 1 in every error address, bits cleared in
    // addr_or are 0 in every error address
    std::uint32_t addr_and{0xFFFFFFFF};
    std::uint32_t addr_or{};
    std::uint32_t addr_min{0xFFFFFFFF};
    std::uint32_t addr_max{};
};

struct pass_result_t {
    std::uint64_t errors{};
    std::time_t seconds{};
    bool completed{};
};

struct test_result_t {
    std::vector<chip_result_t> chips;
    // Per byte lane of the bus: bits read as 1 instead of 0 and vice versa
    std::vector<std::uint8_t> lane_read_1;
    std::vector<std::uint8_t> lane_read_0;
    std::array<std::uint64_t, patterns.size()> pattern_errors{};
    std::vector<pass_result_t> passes;
    std::vector<bad_byte_t> first_errors;
};

auto find_best_mode() -> vbe::mode_info_t {

    for (const auto& mode : vbe::get_modes()) {
        if (mode.width >= 640 && mode.height >= 480 &&
            mode.bits_per_pixel >= 16) {
            return mode;
        }
    }
    throw error("No suitable VESA mode found.");
}

// Consumes all pending key presses, returns true if there were any
auto key_pressed() -> bool {
    auto pressed = false;
    while (kbhit()) {
        getkey();
        pressed = true;
    }
    return pressed;
}

auto test_video_memory(const std::uint16_t mode_id,
                       const std::uint16_t bus_width,
                       const std::uint8_t num_chips,
                       const std::size_t size,
                       const unsigned num_passes) -> test_result_t {

    const auto fb = vbe::framebuffer{mode_id, size};
    const auto bus_width_in_bytes = bus_width / bits_per_byte;
    const auto bytes_per_chip = bus_width_in_bytes / num_chips;

    auto result = test_result_t{};
    result.chips.resize(num_chips);
    result.lane_read_1.resize(bus_width_in_bytes);
    result.lane_read_0.resize(bus_width_in_bytes);

    auto block =
        std::vector<word_t>(bus_width_in_bytes * 1024u / sizeof(word_t));
    const auto block_size = block.size() * sizeof(word_t);

    const auto record_error = [&](const bad_byte_t& bad) {
        const auto lane = bad.addr % bus_width_in_bytes;
        auto& chip = result.chips[lane / bytes_per_chip];
        chip.errors++;
        chip.addr_and &= bad.addr;
        chip.addr_or |= bad.addr;
        chip.addr_min = std::min(chip.addr_min, bad.addr);
        chip.addr_max = std::max(chip.addr_max, bad.addr);
        result.lane_read_1[lane] |= bad.actual & ~bad.expected;
        result.lane_read_0[lane] |= bad.expected & ~bad.actual;
        result.pattern_errors[bad.pattern]++;
        result.passes.back().errors++;
        if (result.first_errors.size() < max_listed_errors) {
            result.first_errors.push_back(bad);
        }
    };

    const auto test_pattern = [&](const std::size_t index,
                                  const unsigned pass) {
        const auto& pattern = patterns[index];
        const auto seed = mix(pass);
        for (auto addr = word_t{0}; addr < size; addr += block_size) {
            const auto rest = std::min<std::size_t>(size - addr, block_size);
            for (auto i = 0u; i < rest / sizeof(word_t); i++) {
                block[i] = pattern.word_at(addr + i * sizeof(word_t), seed);
            }
            fb.write(addr, block.data(), rest);
        }
        for (auto addr = word_t{0}; addr < size; addr += block_size) {
            const auto rest = std::min<std::size_t>(size - addr, block_size);
            fb.read(addr, block.data(), rest);
            for (auto i = 0u; i < rest / sizeof(word_t); i++) {
                const auto word_addr =
                    static_cast<word_t>(addr + i * sizeof(word_t));
                const auto expected = pattern.word_at(word_addr, seed);
                if (block[i] == expected) {
                    continue;
                }
                for (auto byte = 0u; byte < sizeof(word_t); byte++) {
                    const auto shift = byte * bits_per_byte;
                    const auto want =
                        static_cast<std::uint8_t>(expected >> shift);
                    const auto got =
                        static_cast<std::uint8_t>(block[i] >> shift);
                    if (want != got) {
                        record_error({word_addr + byte, want, got, index,
                                      pass});
                    }
                }
            }
        }
    };

    for (auto pass = 1u; num_passes == 0 || pass <= num_passes; pass++) {
        const auto start = std::time(nullptr);
        result.passes.push_back({});
        auto stopped = false;
        for (auto i = 0u; i < patterns.size() && !stopped; i++) {
            test_pattern(i, pass);
            stopped = key_pressed();
        }
        result.passes.back().seconds = std::time(nullptr) - start;
        result.passes.back().completed = !stopped;
        if (stopped) {
            break;
        }
    }
    return result;
}

// Lists the data bits of the bus, e.g. " D32 D37", which are set in the
// given lanes
auto data_bits(const std::vector<std::uint8_t>& lane_bits,
               const std::size_t first_lane, const std::size_t num_lanes)
    -> std::string {
    auto names = std::string{};
    for (auto lane = first_lane; lane < first_lane + num_lanes; lane++) {
        for (auto bit = 0u; bit < bits_per_byte; bit++) {
            if (lane_bits[lane] & (1u << bit)) {
                names += " D" + std::to_string(lane * bits_per_byte + bit);
            }
        }
    }
    return names;
}

// Lists the address bits in [first_bit, end_bit) which have the same value
// in every error address of the chip, e.g. " A23=1 A8=0"
auto shared_address_bits(const chip_result_t& chip, const unsigned first_bit,
                         const unsigned end_bit) -> std::string {
    auto names = std::string{};
    for (auto bit = end_bit; bit-- > first_bit;) {
        const auto mask = std::uint32_t{1} << bit;
        if (chip.addr_and & mask) {
            names += " A" + std::to_string(bit) + "=1";
        } else if (!(chip.addr_or & mask)) {
            names += " A" + std::to_string(bit) + "=0";
        }
    }
    return names;
}

void log_result(const test_result_t& result,
                const std::size_t bus_width_in_bytes, const std::size_t size) {
    const auto bytes_per_chip = bus_width_in_bytes / result.chips.size();
    // Address bits below first_bit select the byte lane, which is already
    // covered by the chip number and data bits
    const auto first_bit =
        static_cast<unsigned>(std::bit_width(bus_width_in_bytes - 1));
    const auto end_bit = static_cast<unsigned>(std::bit_width(size - 1));

    log("\nResult:");
    log("----------");
    for (auto i = 0u; i < result.chips.size(); i++) {
        const auto& chip = result.chips[i];
        if (chip.errors == 0) {
            log("Chip %u: OK", i);
            continue;
        }
        log("Chip %u: BAD, %llu errors", i,
            static_cast<unsigned long long>(chip.errors));
        const auto first_lane = i * bytes_per_chip;
        const auto read_1 =
            data_bits(result.lane_read_1, first_lane, bytes_per_chip);
        const auto read_0 =
            data_bits(result.lane_read_0, first_lane, bytes_per_chip);
        if (!read_1.empty()) {
            log("  data bits reading 1 instead of 0:%s", read_1);
        }
        if (!read_0.empty()) {
            log("  data bits reading 0 instead of 1:%s", read_0);
        }
        log("  error addresses: 0x%08lX - 0x%08lX",
            static_cast<unsigned long>(chip.addr_min),
            static_cast<unsigned long>(chip.addr_max));
        const auto shared = shared_address_bits(chip, first_bit, end_bit);
        if (chip.errors >= min_errors_for_address_bits && !shared.empty()) {
            log("  address bits equal in all errors:%s", shared);
        }
    }

    auto total_errors = std::uint64_t{0};
    auto per_pattern = std::string{"Errors per pattern:"};
    for (auto i = 0u; i < patterns.size(); i++) {
        total_errors += result.pattern_errors[i];
        per_pattern += std::string{" "} + patterns[i].name + "=" +
                       std::to_string(result.pattern_errors[i]);
    }
    if (total_errors > 0) {
        log("\n%s", per_pattern);
        log("First errors (address: written -> read):");
        for (const auto& bad : result.first_errors) {
            log("  0x%08lX: 0x%02X -> 0x%02X  chip %u, pattern %s, pass %u",
                static_cast<unsigned long>(bad.addr), bad.expected,
                bad.actual,
                static_cast<unsigned>(bad.addr % bus_width_in_bytes /
                                      bytes_per_chip),
                patterns[bad.pattern].name, bad.pass);
        }
    }

    const auto& passes = result.passes;
    const auto completed =
        std::count_if(passes.begin(), passes.end(),
                      [](const auto& pass) { return pass.completed; });
    log("\nPasses completed: %d%s", static_cast<int>(completed),
        passes.back().completed ? "" : " (stopped by key press)");
    for (auto i = 0u; i < passes.size(); i++) {
        const auto& pass = passes[i];
        if (passes.size() <= max_listed_passes || pass.errors > 0) {
            log("  Pass %u: %llu errors, %ld s%s", i + 1,
                static_cast<unsigned long long>(pass.errors),
                static_cast<long>(pass.seconds),
                pass.completed ? "" : " (stopped)");
        }
    }
}

void check_arguments(const int bus_width, const int num_chips,
                     const int passes) {
    if (bus_width <= 0 ||
        bus_width > std::numeric_limits<std::uint16_t>::max()) {
        throw error("Memory bus width must be between 1 and 65535 bits");
    }
    if (num_chips <= 0 ||
        num_chips > std::numeric_limits<std::uint8_t>::max()) {
        throw error("Number of chips must be between 1 and 255");
    }
    if (bus_width % (num_chips * static_cast<int>(bits_per_byte)) != 0) {
        throw error("Memory bus width must split into whole bytes per chip, "
                    "e.g. 128-bit with 4 or 8 chips");
    }
    if (passes < 0) {
        throw error("Number of passes must be 0 (until a key is pressed) "
                    "or more");
    }
}

auto resolve_total_memory(const int size_override) -> std::size_t {
    if (size_override == 0) {
        const auto detected = vbe::get_total_memory_size();
        if (detected == 0) {
            throw error("VBE reports no video memory, use --size");
        }
        return detected;
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

void run(const int bus_width, const int num_chips, const int size_override,
         const int passes, const bool skip_prompt) {

    check_arguments(bus_width, num_chips, passes);

    const auto oem_info = vbe::get_oem_info();
    const auto detected_memory = vbe::get_total_memory_size();
    const auto total_memory = resolve_total_memory(size_override);
    const auto mode = find_best_mode();
    const auto to_mb = [](const std::size_t bytes) {
        return static_cast<unsigned long>(bytes / bytes_per_mb);
    };

    log("Test Info:");
    log("----------");
    log("OEM String: %s", oem_info.description);
    log("Vendor: %s", oem_info.vendor_name);
    log("Product: %s", oem_info.product_name);
    log("Revision: %s", oem_info.revision_name);
    if (size_override > 0) {
        log("Total Memory: %luMB (manual override, VBE reports %luMB)",
            to_mb(total_memory), to_mb(detected_memory));
    } else {
        log("Total Memory: %luMB", to_mb(total_memory));
    }
    log("Memory bus: %d-bit", bus_width);
    log("Number of chips: %d", num_chips);
    log("Test video mode: %#X [%dx%dx%d]", mode.id, mode.width, mode.height,
        mode.bits_per_pixel);
    log("Test passes: %s", passes == 0 ? std::string{"until a key is pressed"}
                                       : std::to_string(passes));
    if (total_memory > detected_memory) {
        log("warning: the size override is larger than what VBE reports.");
        log("warning: errors only above %luMB mean the override is too large.",
            to_mb(detected_memory));
    }

    std::puts("\nThe test can take up to several minutes per pass");
    std::puts("Press any key during the test to stop it");
    if (!skip_prompt) {
        std::puts("Press [ENTER] to continue");
        getchar();
    }
    const auto test_result = test_video_memory(mode.id, bus_width, num_chips,
                                               total_memory, passes);
    log_result(test_result, bus_width / bits_per_byte, total_memory);
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
            {"passes", false, 1,
             "Number of test passes, 0 = repeat until a key is pressed"},
            {"yes", false, false, "Start without waiting for [ENTER]"},
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
            args.get<int>("size"), args.get<int>("passes"),
            args.get<bool>("yes"));

        disable_log_file();
        return EXIT_SUCCESS;
    } catch (std::exception& ex) {
        log("error: %s", ex.what());
        disable_log_file();
        return EXIT_FAILURE;
    }
}
