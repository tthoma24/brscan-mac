// swift-tools-version:5.9
import PackageDescription

// The config round-trip library for Plan 1e's graphical configuration UI.
// See docs/PLAN-1E-DESIGN.md for the overall design. This package is
// intentionally independent of the project's CMake/C++ build -- it is built
// and tested with `swift build` / `swift test` from this directory.
let package = Package(
  name: "BrscanConfigCore",
  platforms: [
    .macOS(.v13)
  ],
  products: [
    .library(
      name: "BrscanConfigCore",
      targets: ["BrscanConfigCore"])
  ],
  targets: [
    .target(
      name: "BrscanConfigCore"),
    .testTarget(
      name: "BrscanConfigCoreTests",
      dependencies: ["BrscanConfigCore"],
      resources: [.copy("Fixtures")]),
  ]
)
