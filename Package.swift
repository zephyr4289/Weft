// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "Weft",
    platforms: [
        .iOS(.v15),
        .macOS(.v12),
        .tvOS(.v15),
        .watchOS(.v8),
        .visionOS(.v1)
    ],
    products: [
        .library(
            name: "Weft",
            targets: ["WeftCore", "WeftSwiftUI"]
        ),
        .library(
            name: "CWeft",
            targets: ["CWeft"]
        ),
        .library(
            name: "WeftCore",
            targets: ["WeftCore"]
        ),
        .library(
            name: "WeftSwiftUI",
            targets: ["WeftSwiftUI"]
        )
    ],
    dependencies: [
        .package(url: "https://github.com/apple/swift-atomics.git", from: "1.2.0")
    ],
    targets: [
        .target(
            name: "CWeft",
            path: "core/c",
            sources: ["weft.c"],
            publicHeadersPath: "."
        ),
        .target(
            name: "WeftCore",
            dependencies: [
                "CWeft",
                .product(name: "Atomics", package: "swift-atomics")
            ],
            path: "core/swift",
            exclude: ["README.md"]
        ),
        .target(
            name: "WeftSwiftUI",
            dependencies: [
                "WeftCore"
            ],
            path: "Sources/WeftSwiftUI"
        ),
        .testTarget(
            name: "WeftTests",
            dependencies: ["WeftCore", "WeftSwiftUI"],
            path: "Tests/WeftTests"
        )
    ]
)
