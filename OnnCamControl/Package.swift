// swift-tools-version: 6.2
import PackageDescription

let package = Package(
    name: "OnnCamControl",
    platforms: [
        .macOS(.v14),
    ],
    targets: [
        .executableTarget(
            name: "OnnCamControl",
            path: "Sources/OnnCamControl")
    ]
)
