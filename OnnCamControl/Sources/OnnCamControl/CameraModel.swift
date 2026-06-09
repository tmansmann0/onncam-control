import AVFoundation
import Foundation
import ServiceManagement

struct CameraControl: Hashable, Sendable {
    let name: String
    let label: String
    var current: Int
    let min: Int?
    let max: Int?
    let step: Int?
    let defaultValue: Int?
    let settable: Bool
}

struct CameraFormat: Hashable, Sendable, Identifiable {
    let id: String
    let deviceUniqueID: String
    let formatIndex: Int
    let width: Int32
    let height: Int32
    let pixelFormat: String
    let frameRate: Double

    var title: String {
        "\(width)x\(height) @ \(Self.normalizedRate(frameRate)) fps \(pixelFormat)"
    }

    var resolutionTitle: String {
        "\(width)x\(height)"
    }

    var fpsTitle: String {
        "\(Self.normalizedRate(frameRate)) fps \(pixelFormat)"
    }

    private static func normalizedRate(_ rate: Double) -> String {
        let rounded = round(rate)
        if abs(rate - rounded) < 0.01 {
            return "\(Int(rounded))"
        }
        return String(format: "%.2f", rate)
    }
}

struct UserPreset: Codable, Hashable, Sendable {
    let name: String
    let values: [String: Int]
    let createdAt: Date
}

private let controlLabels: [String: String] = [
    "exposure-auto": "Exposure Mode",
    "exposure": "Shutter",
    "focus": "Focus",
    "focus-auto": "Autofocus",
    "zoom": "Zoom",
    "backlight": "Backlight",
    "brightness": "Brightness",
    "contrast": "Contrast",
    "gain": "Gain",
    "powerline": "Anti-flicker",
    "hue": "Hue",
    "saturation": "Saturation",
    "sharpness": "Sharpness",
    "gamma": "Gamma",
    "white": "Temperature",
    "white-auto": "Auto White Balance"
]

/// Mode-style controls that must be applied before value controls (e.g. the
/// camera rejects a manual exposure value while auto exposure is engaged).
private let modeControlNames = ["exposure-auto", "white-auto", "focus-auto"]

@MainActor
final class CameraViewModel: ObservableObject {
    @Published private(set) var controls: [String: CameraControl] = [:]
    @Published private(set) var formats: [CameraFormat] = []
    @Published private(set) var activeFormatID: String?
    @Published private(set) var userPresets: [UserPreset] = []
    @Published private(set) var status = "Connecting…"
    @Published private(set) var cameraFound = false
    @Published private(set) var launchAtLogin = SMAppService.mainApp.status == .enabled

    let previewController = PreviewSessionController()

    private let helper = HelperClient()
    private let presetStore = UserPresetStore()
    private var pendingSets: [String: Int] = [:]
    private var drainTask: Task<Void, Never>?

    func control(_ name: String) -> CameraControl? {
        controls[name]
    }

    // MARK: - Refresh

    func refresh() async {
        do {
            let lines = try await helper.send("list")
            let parsed = lines.compactMap(Self.parseControlLine)
            controls = Dictionary(parsed.map { ($0.name, $0) }, uniquingKeysWith: { first, _ in first })
            cameraFound = !parsed.isEmpty
            status = cameraFound ? "Ready" : "Camera reported no controls"
        } catch {
            controls = [:]
            cameraFound = false
            status = Self.friendlyMessage(error)
        }

        let formatState = await Task.detached { FormatManager.load() }.value
        formats = formatState.formats
        activeFormatID = formatState.activeID
        userPresets = presetStore.load()
        launchAtLogin = SMAppService.mainApp.status == .enabled
    }

    // MARK: - Launch at login

    func setLaunchAtLogin(_ enabled: Bool) {
        do {
            if enabled {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
        } catch {
            status = "Login item: \(error.localizedDescription)"
        }
        launchAtLogin = SMAppService.mainApp.status == .enabled
    }

    // MARK: - Control changes (live, coalesced)

    /// Updates local state immediately and queues the hardware write. While a
    /// write is in flight, newer values for the same control replace the
    /// pending one, so dragging a slider only sends the values the camera can
    /// actually keep up with.
    func setControl(_ name: String, to value: Int) {
        guard var control = controls[name] else { return }
        guard control.current != value else { return }
        control.current = value
        controls[name] = control

        pendingSets[name] = value
        if drainTask == nil {
            drainTask = Task { await drainPendingSets() }
        }
    }

    private func drainPendingSets() async {
        while let name = pendingSets.keys.first, let value = pendingSets.removeValue(forKey: name) {
            do {
                if name == "exposure" {
                    try await ensureManualExposure()
                }
                let lines = try await helper.send("set \(name) \(value)")
                applyReadback(lines, name: name, sentValue: value)
                status = "Ready"
            } catch {
                status = Self.friendlyMessage(error)
            }
        }
        drainTask = nil
    }

    /// The camera clamps some values; trust its readback unless the user has
    /// already queued a newer value.
    private func applyReadback(_ lines: [String], name: String, sentValue: Int) {
        guard pendingSets[name] == nil,
              let line = lines.first(where: { $0.hasPrefix("\(name)=") }),
              let readback = Int(line.dropFirst(name.count + 1)),
              readback != sentValue
        else { return }
        controls[name]?.current = readback
    }

    private func ensureManualExposure() async throws {
        guard controls["exposure-auto"]?.current != 1 else { return }
        _ = try await helper.send("set exposure-auto 1")
        controls["exposure-auto"]?.current = 1
        try? await Task.sleep(nanoseconds: 100_000_000)
    }

    // MARK: - Formats

    func applyFormat(id: String) {
        guard let format = formats.first(where: { $0.id == id }) else { return }
        status = "Setting \(format.title)…"
        Task {
            let errorMessage = await Task.detached { FormatManager.apply(format) }.value
            if let errorMessage {
                status = errorMessage
            } else {
                activeFormatID = id
                status = "Ready"
            }
        }
    }

    // MARK: - Presets

    func saveCurrentPreset(named name: String) {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        let values = Dictionary(uniqueKeysWithValues: controls.values
            .filter { $0.settable && $0.name != "zoom" }
            .map { ($0.name, $0.current) })
        presetStore.save(UserPreset(name: trimmed, values: values, createdAt: Date()))
        userPresets = presetStore.load()
        status = "Saved \(trimmed)"
    }

    func applyUserPreset(_ preset: UserPreset) {
        applyValues(preset.values, title: preset.name)
    }

    func deleteUserPreset(named name: String) {
        presetStore.delete(named: name)
        userPresets = presetStore.load()
        status = "Deleted \(name)"
    }

    func resetToDeviceDefaults() {
        let defaults = Dictionary(uniqueKeysWithValues: controls.values
            .filter { $0.settable }
            .compactMap { control in control.defaultValue.map { (control.name, $0) } })
        applyValues(defaults, title: "device defaults")
    }

    private func applyValues(_ values: [String: Int], title: String) {
        Task {
            status = "Applying \(title)…"
            var ordered: [(String, Int)] = []
            for name in modeControlNames {
                if let value = values[name] { ordered.append((name, value)) }
            }
            for (name, value) in values.sorted(by: { $0.key < $1.key }) where !modeControlNames.contains(name) && name != "zoom" {
                ordered.append((name, value))
            }

            var firstError: String?
            for (name, value) in ordered {
                do {
                    _ = try await helper.send("set \(name) \(value)")
                } catch {
                    // Some values are rejected depending on mode (e.g. manual
                    // shutter while auto exposure). Apply the rest anyway.
                    if firstError == nil { firstError = Self.friendlyMessage(error) }
                }
            }
            await refresh()
            status = firstError ?? "Applied \(title)"
        }
    }

    // MARK: - Helpers

    private static func parseControlLine(_ line: String) -> CameraControl? {
        let parts = line.split(whereSeparator: { $0 == " " || $0 == "\t" }).map(String.init)
        guard let name = parts.first, let label = controlLabels[name] else { return nil }
        func intValue(_ key: String) -> Int? {
            parts.first(where: { $0.hasPrefix("\(key)=") }).flatMap { Int($0.dropFirst(key.count + 1)) }
        }
        return CameraControl(
            name: name,
            label: label,
            current: intValue("cur") ?? 0,
            min: intValue("min"),
            max: intValue("max"),
            step: intValue("step"),
            defaultValue: intValue("default"),
            settable: parts.contains("set=yes")
        )
    }

    private static func friendlyMessage(_ error: Error) -> String {
        let text = error.localizedDescription
        let lower = text.lowercased()
        if lower.contains("not found") {
            return "Camera not connected"
        }
        if lower.contains("libusb_error_pipe") || lower.contains("set_cur failed") {
            return "The camera rejected that setting"
        }
        return text
    }
}

// MARK: - Capture formats

enum FormatManager {
    static func load() -> (formats: [CameraFormat], activeID: String?) {
        guard let camera = findCamera() else { return ([], nil) }

        var activeID: String?
        var result: [CameraFormat] = []
        for (index, format) in camera.formats.enumerated() {
            let dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription)
            let pixelFormat = pixelFormatName(format.formatDescription)
            for rate in concreteRates(from: format.videoSupportedFrameRateRanges) {
                let item = CameraFormat(
                    id: "\(camera.uniqueID)|\(index)|\(rateKey(rate))",
                    deviceUniqueID: camera.uniqueID,
                    formatIndex: index,
                    width: dimensions.width,
                    height: dimensions.height,
                    pixelFormat: pixelFormat,
                    frameRate: rate
                )
                result.append(item)

                if format == camera.activeFormat && rateMatches(rate, camera.activeVideoMaxFrameDuration) {
                    activeID = item.id
                }
            }
        }

        let sorted = result
            .filter { $0.width > 0 && $0.height > 0 && $0.frameRate >= 15 }
            .sorted {
                if $0.width * $0.height != $1.width * $1.height {
                    return $0.width * $0.height > $1.width * $1.height
                }
                if $0.frameRate != $1.frameRate {
                    return $0.frameRate > $1.frameRate
                }
                return $0.pixelFormat < $1.pixelFormat
            }

        // The camera exposes near-duplicate formats (same size, pixel format,
        // and nominal rate); keep one per displayed combination.
        var seen = Set<String>()
        let unique = sorted.filter { seen.insert("\($0.resolutionTitle)|\($0.fpsTitle)").inserted }
        return (unique, activeID)
    }

    /// Returns an error message, or nil on success.
    static func apply(_ selected: CameraFormat) -> String? {
        let devices = AVCaptureDevice.DiscoverySession(
            deviceTypes: [.external],
            mediaType: .video,
            position: .unspecified
        ).devices

        guard let camera = devices.first(where: { $0.uniqueID == selected.deviceUniqueID }) else {
            return "Camera disappeared"
        }
        guard selected.formatIndex < camera.formats.count else {
            return "Format is no longer available"
        }

        let format = camera.formats[selected.formatIndex]

        // The list shows rates rounded for display (the camera reports e.g.
        // 29.99994 fps), so match with tolerance instead of exact bounds.
        let tolerance = 0.05
        guard let range = format.videoSupportedFrameRateRanges.first(where: {
            selected.frameRate >= $0.minFrameRate - tolerance && selected.frameRate <= $0.maxFrameRate + tolerance
        }) else {
            return "\(selected.title) is not supported by the camera"
        }

        do {
            try camera.lockForConfiguration()
            defer { camera.unlockForConfiguration() }
            camera.activeFormat = format
            let duration: CMTime
            if abs(selected.frameRate - range.maxFrameRate) <= tolerance {
                // Targeting the range's top rate: use the camera's exact
                // native frame duration rather than reconstructing it.
                duration = range.minFrameDuration
            } else {
                let clamped = min(max(selected.frameRate, range.minFrameRate), range.maxFrameRate)
                duration = CMTime(value: 1_000_000, timescale: CMTimeScale((clamped * 1_000_000).rounded()))
            }
            camera.activeVideoMinFrameDuration = duration
            camera.activeVideoMaxFrameDuration = duration
            return nil
        } catch {
            return error.localizedDescription
        }
    }

    static func findCamera() -> AVCaptureDevice? {
        let devices = AVCaptureDevice.DiscoverySession(
            deviceTypes: [.external],
            mediaType: .video,
            position: .unspecified
        ).devices
        return devices.first(where: { $0.localizedName.localizedCaseInsensitiveContains("onn") })
            ?? devices.first(where: { $0.localizedName.localizedCaseInsensitiveContains("webcam") })
    }

    private static func concreteRates(from ranges: [AVFrameRateRange]) -> [Double] {
        let commonRates = [15.0, 24.0, 25.0, 30.0, 50.0, 60.0]
        var rates = Set<Double>()
        for range in ranges {
            if range.maxFrameRate >= 15 {
                rates.insert(round(range.maxFrameRate * 100) / 100)
            }
            for rate in commonRates where rate >= range.minFrameRate && rate <= range.maxFrameRate {
                rates.insert(rate)
            }
        }
        return Array(rates).filter { $0 >= 15 }.sorted(by: >)
    }

    private static func rateKey(_ rate: Double) -> String {
        String(format: "%.2f", rate)
    }

    private static func rateMatches(_ rate: Double, _ duration: CMTime) -> Bool {
        guard duration.isValid && duration.seconds > 0 else { return false }
        return abs((1.0 / duration.seconds) - rate) < 0.1
    }

    private static func pixelFormatName(_ description: CMFormatDescription) -> String {
        let subtype = CMFormatDescriptionGetMediaSubType(description)
        let chars = [
            UInt8((subtype >> 24) & 0xff),
            UInt8((subtype >> 16) & 0xff),
            UInt8((subtype >> 8) & 0xff),
            UInt8(subtype & 0xff)
        ]
        let raw = String(bytes: chars, encoding: .macOSRoman) ?? "????"
        switch raw {
        case "420v": return "NV12"
        case "yuvs": return "YUY2"
        default: return raw
        }
    }
}

// MARK: - Preset persistence

final class UserPresetStore: @unchecked Sendable {
    private let key = "UserPresets.v1"
    private let defaults = UserDefaults.standard

    func load() -> [UserPreset] {
        guard let data = defaults.data(forKey: key),
              let presets = try? JSONDecoder().decode([UserPreset].self, from: data) else {
            return []
        }
        return presets.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    func save(_ preset: UserPreset) {
        var presets = load().filter { $0.name != preset.name }
        presets.append(preset)
        if let data = try? JSONEncoder().encode(presets) {
            defaults.set(data, forKey: key)
        }
    }

    func delete(named name: String) {
        let presets = load().filter { $0.name != name }
        if let data = try? JSONEncoder().encode(presets) {
            defaults.set(data, forKey: key)
        }
    }
}

// MARK: - Preview session

final class PreviewSessionController: @unchecked Sendable {
    let session = AVCaptureSession()
    private let sessionQueue = DispatchQueue(label: "OnnCamControl.PreviewSession")
    private var configured = false

    func start() {
        sessionQueue.async { [weak self] in
            guard let self else { return }
            do {
                try self.configureIfNeeded()
                if !self.session.isRunning {
                    self.session.startRunning()
                }
            } catch {
                // Keep the panel usable even if preview access is unavailable.
            }
        }
    }

    func stop() {
        sessionQueue.async { [weak self] in
            guard let self else { return }
            if self.session.isRunning {
                self.session.stopRunning()
            }
        }
    }

    private func configureIfNeeded() throws {
        guard !configured else { return }
        guard let camera = FormatManager.findCamera() else { return }

        let input = try AVCaptureDeviceInput(device: camera)
        session.beginConfiguration()
        session.sessionPreset = .vga640x480
        if session.canAddInput(input) {
            session.addInput(input)
        }
        session.commitConfiguration()
        configured = true
    }
}
