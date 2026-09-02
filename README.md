# brscan-mac

Native Apple Silicon scanning for older Brother network multifunction
printers that predate driverless AirScan (eSCL).

Brother shipped its last macOS scanner driver as an Intel-only binary built
against the macOS 10.10 SDK. On Apple Silicon that binary can't load, and
models like the MFC-J6920DW don't advertise eSCL, so macOS has no way to
scan from them. `brscan-mac` restores scanning by speaking Brother's raw
scan protocol from a native arm64 build.

## Status

Early development. The protocol core and a command-line scanning tool come
first, followed by an Image Capture device module and a SANE backend.

## Building

Requires macOS 26 or later on Apple Silicon, CMake 3.20 or later, a C++17
compiler, and libjpeg-turbo. On a machine with MacPorts:

```bash
/opt/local/bin/cmake -B build
/opt/local/bin/cmake --build build
ctest --test-dir build --output-on-failure
```

This builds the `brscan` protocol library, its test suite, and the
`brscan-cli` command-line tool, at `build/brscan-cli`.

A handful of tests connect to a real scanner and are skipped by default. To
run them too, set `BRSCAN_TEST_HOST` to your scanner's hostname or IP address
before running `ctest`:

```bash
BRSCAN_TEST_HOST=BRWxxxxxxxxxxxx.local ctest --test-dir build --output-on-failure
```

## Finding your scanner

`brscan-cli` needs your scanner's hostname or IP address on port 54921.
Brother's network scanners advertise themselves over Bonjour as
`_scanner._tcp`. Browse for them with `dns-sd`:

```bash
dns-sd -B _scanner._tcp
```

This lists the service names of scanners on your network (for example,
`BRW00AABBCCDDEE`). Resolve one to a hostname with:

```bash
dns-sd -G v4 BRW00AABBCCDDEE.local
```

You can pass either the `.local` hostname or the resolved IP address to
`brscan-cli --host`.

## Using brscan-cli

Scan a color image at 300 dpi from the flatbed to a JPEG file:

```bash
build/brscan-cli --host BRW00AABBCCDDEE.local --output scan.jpg
```

Scan in grayscale at a lower resolution:

```bash
build/brscan-cli --host BRW00AABBCCDDEE.local --mode gray --resolution 100 \
  --output scan.pgm
```

Scan from the document feeder instead of the flatbed:

```bash
build/brscan-cli --host BRW00AABBCCDDEE.local --source adf --output scan.jpg
```

Scan a specific region instead of the full page, in pixels at the scan
resolution:

```bash
build/brscan-cli --host BRW00AABBCCDDEE.local --area 0,0,1200,1600 \
  --output scan.jpg
```

Run `build/brscan-cli --help` for the full list of options. `--mode`
determines the output file format: `color` writes a JPEG, `gray` writes a
binary PGM. `brscan-cli` exits with a non-zero status and a message on
`stderr` if the scan fails -- for example, if the scanner is busy or the
document feeder is empty.

## Trademark and affiliation

This project is unofficial and not affiliated with, authorized by, or
endorsed by Brother Industries, Ltd. "Brother" and model names such as
"MFC-J6920DW" are used only to describe hardware compatibility. All
trademarks are the property of their respective owners.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
