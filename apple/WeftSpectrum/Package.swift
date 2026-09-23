// swift-tools-version:5.9
// WeftSpectrum — Pillar 5 managed SDK (Swift/iOS/macOS lane).
import PackageDescription

let package = Package(
    name: "WeftSpectrum",
    platforms: [
        // @Observable (Observation) requires these minimums.
        .macOS(.v14), .iOS(.v17),
    ],
    products: [
        .library(name: "WeftSpectrum", targets: ["WeftSpectrum"]),
    ],
    targets: [
        .target(
            name: "WeftSpectrum",
            dependencies: [],
            path: "Sources/WeftSpectrum"
        ),
        .testTarget(
            name: "WeftSpectrumTests",
            dependencies: ["WeftSpectrum"],
            path: "Tests/WeftSpectrumTests"
        ),
    ]
)
