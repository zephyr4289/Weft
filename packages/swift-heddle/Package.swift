// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "WeftSwiftUI",
    platforms: [.macOS(.v14), .iOS(.v17)],
    products: [
        .library(name: "WeftSwiftUI", targets: ["WeftSwiftUI"])
    ],
    targets: [
        .target(
            name: "WeftSwiftUI",
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .testTarget(
            name: "WeftSwiftUITests",
            dependencies: ["WeftSwiftUI"]
        )
    ]
)
