// swift-tools-version:5.9
import PackageDescription

// The SwiftUI shell for Plan 1e's graphical configuration app. This package
// is a SwiftPM executable target (not an Xcode project) so it can be built
// and tested headlessly with `swift build` / `swift test` from this
// directory; see docs/PLAN-1E-DESIGN.md and Task 1e.5's brief. It depends on
// `BrscanConfigCore` (config round-trip library, Tasks 1e.1-1e.4) via a
// local path dependency, but Task 1e.5 doesn't bind any UI to it yet --
// that's Tasks 1e.6-1e.8.
let package = Package(
  name: "BrscanConfigApp",
  platforms: [
    .macOS(.v13)
  ],
  dependencies: [
    .package(path: "../BrscanConfigCore")
  ],
  targets: [
    .executableTarget(
      name: "BrscanConfigApp",
      dependencies: ["BrscanConfigCore"]),
    .testTarget(
      name: "BrscanConfigAppTests",
      dependencies: ["BrscanConfigApp"]),
  ]
)
