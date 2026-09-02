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

## Trademark and affiliation

This project is unofficial and not affiliated with, authorized by, or
endorsed by Brother Industries, Ltd. "Brother" and model names such as
"MFC-J6920DW" are used only to describe hardware compatibility. All
trademarks are the property of their respective owners.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
