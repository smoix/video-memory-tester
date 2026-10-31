# Necroware's Video Memory Tester

*Warning:* This is a very early development version which potentially could
break your graphics card, your house, your city, your country, and also come
home drunken and impregnate your cat. So please, use it only at your own risk. 

# Why do we need another video memory test tool?

First of all, this one is fast. Furthermore there are already tools out there
which just return broken addresses, where you have to derive which of the
memory chips on your card are broken. It often ends up in guessing games and
replacing the wrong chips. So this tool doesn't only tell if the memory of your
graphics card is broken, it also tells you which chip you should replace. This
tool runs in DOS and is theoretically able to test all kinds of graphics cards
which support VBE 2.0

# How it works?

The tool detects the graphics card and uses VBE 2.0 to allocate a linear frame
buffer over the total size of the video memory. Then it writes different
patterns into the video memory and validates the content. If any address turns
out to be bad the tool calculates which memory chip exactly is affected on the
card. Unfortunately it is not possible to tell which chips and how many of those
are installed on the tested card, so this information has to be provided by the
user and is used for the result calculations.

For the test the tool switches to the first VBE mode with a linear frame buffer
of at least 640x480 at 16 bits per pixel. The visible screen is part of the
tested memory, so it shows the test patterns while the test runs. Afterwards
the text mode is restored and the result is printed.

# Requirements

- DOS (see the limitations below) and a VBE 2.0 capable graphics card.
- `CWSDPMI.EXE` in the same directory as `nwvmt.exe` or in the `PATH`. It is
  the DPMI host every DJGPP program needs and is included in
  `dpmi/csdpmi7b.zip`.

CWSDPMI tries to create a swap file `c:\cwsdpmi.swp` and prints
`Warning: cannot open swap file` when that fails, e.g. when booting from a
floppy without a writable `C:`. The warning is harmless, the tool needs very
little memory. To get rid of it, either disable the swap file once with
`cwsparam cwsdpmi.exe` (included in the zip, answer `""` to the paging file
question), or load CWSDPMI yourself before the test with `cwsdpmi -p -s-` and
unload it afterwards with `cwsdpmi -u`.

# Usage

    nwvmt.exe --bus=<bits> --chips=<n> [--size=<MB>] [--passes=<n>] [--yes] [--log]

- `--bus=<int>` (required): Memory bus width of the card in bits.
- `--chips=<int>` (required): Number of memory chips installed on the card.
  The bus width has to split into whole bytes per chip.
- `--size=<int>` (optional): Manual override for the video memory size in MB,
  in case it isn't correctly detected via VBE. Some cards report less than they
  have, e.g. a Radeon 9600 PRO with 256MB whose VBE BIOS reports 16MB. If the
  override is larger than what VBE reports, a warning is printed. Only use the
  real size of the card: a larger value makes the test write past the card's
  memory into whatever else is mapped at those physical addresses, and the
  errors then all appear above the real size.
- `--passes=<int>` (optional): Number of test passes, default 1. `0` repeats
  the test until a key is pressed, which helps to catch faults that only show
  up once the card is warm.
- `--yes` (optional): Start the test without waiting for [ENTER].
- `--log` (optional): Enable logging of the card detection, test result and
  any error message to `nwvmt.log` in the current directory. The file is
  overwritten on every run.
- `--help`: Show usage information.

Values can also be passed as a separate argument (`--bus 128`), which is what
DOS batch files turn `--bus=128` into. Pressing any key during the test stops
it after the current pattern.

Running `nwvmt.exe` without any arguments reports the missing required
parameters and prints the usage information.

Examples:

    rem 128-bit card with 4 chips, one pass, log the result
    nwvmt.exe --bus=128 --chips=4 --log

    rem repeat until a key is pressed, to catch faults of a warm card
    nwvmt.exe --bus=128 --chips=4 --passes=0 --log

    rem VBE reports the wrong size, test 256MB, start without prompt
    nwvmt.exe --bus=128 --chips=8 --size=256 --yes --log

# Test patterns and result

Every pass writes the whole video memory and reads it back with the patterns
`0x00`, `0xFF`, `0xAA`, `0x55`, the address of every 32-bit word, its inverse
and a pseudo random pattern derived from the address. The address based
patterns give every word a unique value, so broken address lines or a too
large `--size` show up as errors.

For every chip the result shows:

- the number of bad bytes,
- the bus data bits (`D0`...) that read 1 instead of 0 and 0 instead of 1,
  which tells a single broken pin or solder joint from a dead chip,
- the lowest and highest bad address, and the address bits which have the
  same value in all errors. Such bits mean that only a part of the memory is
  affected, e.g. a second rank of chips, a broken address line, or a card
  whose memory controller interleaves channels so that the chip numbers don't
  match the physical chips.

It also lists the errors per pattern, the first 16 bad bytes, and the errors
and duration of every pass.

Chip numbers are logical slices of the bus (chip 0 holds the lowest bytes).
How they map to the chips on the PCB depends on the card, and boards often swap
data bits within a byte, so verify the position before replacing a chip.

# Current limitations

- Needs pure DOS without any memory managers loaded
- Supports only VBE 2.0 capable graphics cards
- The memory chips on the card have to be at least 8-bit or multiple of 8-bits
- No graphical interface or progress feedback during the tests

# Programming details

With this project I finally got to a point which I had on my TODO list for a
very long time: programming in modern C++ for DOS. So the tool is written in 
C++20 with STL, exceptions and almost all the fancy stuf. Only iostreams were
not used because just inclusion of the headers grew the binary footprint of the 
tool by 300%. The project utilizes the DJGPP C++ compiler (GCC 12 or newer, the
Docker image below currently ships GCC 14.2) and CMake (3.23 or newer).

# How to build

Using the same Docker image the CI build uses (no local DJGPP install needed):

    docker run --rm -v "$PWD":/work -w /work jagholin/dosbox-gcc:latest \
        bash -c "apt-get update -qq && apt-get install -y -qq cmake && \
                 cmake -B build -S src -DCMAKE_BUILD_TYPE=Release && cmake --build build"

The result is `build/nwvmt.exe`. The CI additionally compresses it with
`upx -9`, which shrinks it from about 560KB to about 150KB; this step is
optional. GitHub Actions builds every pull request, and pushing a `vX.Y.Z` tag
creates a release with the compressed `nwvmt.exe` attached.

# License

The project is licensed under GPL v3.0

