// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "WeftFintech",
    platforms: [.iOS(.v17), .macOS(.v14)],
    products: [.library(name: "WeftFintech", targets: ["WeftFintech"])],
    targets: [
        .target(name: "WeftFintech"),
        .testTarget(name: "WeftFintechTests", dependencies: ["WeftFintech"]),
    ]
)
