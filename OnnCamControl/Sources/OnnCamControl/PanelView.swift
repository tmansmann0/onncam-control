import AVFoundation
import SwiftUI

struct PanelView: View {
    @ObservedObject var model: CameraViewModel
    var panelHeight: CGFloat = 690

    @State private var showSavePreset = false
    @State private var presetName = ""
    @State private var selectedResolution: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            CameraPreviewView(controller: model.previewController)
                .frame(height: panelHeight < 600 ? 150 : 196)
                .clipShape(RoundedRectangle(cornerRadius: 10))

            statusRow

            if model.cameraFound {
                ScrollView(.vertical, showsIndicators: false) {
                    VStack(alignment: .leading, spacing: 10) {
                        exposureSection
                        colorSection
                        focusSection
                        imageSection
                        formatSection
                    }
                    .padding(.bottom, 2)
                }
            } else {
                missingCameraView
            }

            Divider()
            footer
        }
        .padding(14)
        .frame(width: 384, height: panelHeight)
        .alert("Save Preset", isPresented: $showSavePreset) {
            TextField("Preset name", text: $presetName)
            Button("Save") { model.saveCurrentPreset(named: presetName) }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Saves the current exposure, color, focus, and image values.")
        }
    }

    // MARK: - Header / footer

    private var statusRow: some View {
        HStack(spacing: 6) {
            Circle()
                .fill(model.cameraFound ? Color.green : Color.red)
                .frame(width: 7, height: 7)
            Text(statusSummary)
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .truncationMode(.tail)
            Spacer()
        }
    }

    private var statusSummary: String {
        var parts = [model.status]
        if isAutoExposure {
            parts.append("Auto exposure")
        } else {
            if let exposure = model.control("exposure")?.current { parts.append("Shutter \(exposure)") }
            if let gain = model.control("gain")?.current { parts.append("Gain \(gain)") }
        }
        return parts.joined(separator: " · ")
    }

    private var missingCameraView: some View {
        VStack(spacing: 8) {
            Spacer()
            Image(systemName: "video.slash")
                .font(.system(size: 28))
                .foregroundStyle(.secondary)
            Text("onn 4K Webcam not found")
                .font(.system(size: 13, weight: .medium))
            Text("Plug in the camera, then refresh.")
                .font(.system(size: 11))
                .foregroundStyle(.secondary)
            Button("Refresh") { Task { await model.refresh() } }
            Spacer()
        }
        .frame(maxWidth: .infinity)
    }

    private var footer: some View {
        HStack(spacing: 10) {
            Toggle("Start at login", isOn: Binding(
                get: { model.launchAtLogin },
                set: { model.setLaunchAtLogin($0) }
            ))
            .toggleStyle(.checkbox)
            .font(.system(size: 11))
            Spacer()
            presetsMenu
            Button("Refresh") { Task { await model.refresh() } }
            Button("Quit") { NSApplication.shared.terminate(nil) }
        }
        .controlSize(.small)
    }

    private var presetsMenu: some View {
        Menu("Presets") {
            if model.userPresets.isEmpty {
                Text("No saved presets")
            } else {
                ForEach(model.userPresets, id: \.name) { preset in
                    Button(preset.name) { model.applyUserPreset(preset) }
                }
            }
            Divider()
            Button("Save Current…") {
                presetName = ""
                showSavePreset = true
            }
            Button("Reset to Device Defaults") { model.resetToDeviceDefaults() }
            if !model.userPresets.isEmpty {
                Divider()
                Menu("Delete") {
                    ForEach(model.userPresets, id: \.name) { preset in
                        Button(preset.name) { model.deleteUserPreset(named: preset.name) }
                    }
                }
            }
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
    }

    // MARK: - Sections

    private var isAutoExposure: Bool {
        model.control("exposure-auto")?.current == 8
    }

    private var exposureSection: some View {
        section("Exposure", symbol: "camera.aperture") {
            Picker("", selection: intBinding("exposure-auto", normalize: { $0 == 8 ? 8 : 1 })) {
                Text("Manual").tag(1)
                Text("Auto").tag(8)
            }
            .pickerStyle(.segmented)
            .labelsHidden()

            ControlSliderRow(
                control: model.control("exposure"),
                maxOverride: 2000,
                logarithmic: true,
                disabled: isAutoExposure
            ) { model.setControl("exposure", to: $0) }

            ControlSliderRow(control: model.control("gain")) { model.setControl("gain", to: $0) }

            labeledRow("Anti-flicker") {
                Picker("", selection: intBinding("powerline")) {
                    Text("Off").tag(0)
                    Text("50 Hz").tag(1)
                    Text("60 Hz").tag(2)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
            }
        }
    }

    private var colorSection: some View {
        section("White Balance", symbol: "circle.lefthalf.filled") {
            toggleRow("Auto", name: "white-auto")
            ControlSliderRow(
                control: model.control("white"),
                unit: "K",
                disabled: (model.control("white-auto")?.current ?? 0) != 0
            ) { model.setControl("white", to: $0) }
        }
    }

    private var focusSection: some View {
        section("Focus", symbol: "scope") {
            toggleRow("Autofocus", name: "focus-auto")
            ControlSliderRow(
                control: model.control("focus"),
                disabled: (model.control("focus-auto")?.current ?? 0) != 0
            ) { model.setControl("focus", to: $0) }
            if model.zoomAvailable {
                ControlSliderRow(control: model.control("zoom")) { model.setControl("zoom", to: $0) }
                Text("Zoom is an ISP crop; it only applies at 1280x720 and below.")
                    .font(.system(size: 10))
                    .foregroundStyle(.tertiary)
            }
        }
    }

    private var imageSection: some View {
        section("Image", symbol: "slider.horizontal.3") {
            ForEach(["brightness", "contrast", "saturation", "sharpness", "gamma", "hue", "backlight"], id: \.self) { name in
                ControlSliderRow(control: model.control(name)) { model.setControl(name, to: $0) }
            }
        }
    }

    private var formatSection: some View {
        let grouped = Dictionary(grouping: model.formats, by: \.resolutionTitle)
        let resolutions = grouped.keys.sorted { lhs, rhs in
            let leftPixels = grouped[lhs]?.first.map { Int($0.width) * Int($0.height) } ?? 0
            let rightPixels = grouped[rhs]?.first.map { Int($0.width) * Int($0.height) } ?? 0
            return leftPixels == rightPixels ? lhs < rhs : leftPixels > rightPixels
        }
        let activeResolution = model.formats.first(where: { $0.id == model.activeFormatID })?.resolutionTitle
        let currentResolution = selectedResolution ?? activeResolution ?? resolutions.first ?? ""
        let frameRates = (grouped[currentResolution] ?? []).sorted {
            if $0.frameRate != $1.frameRate { return $0.frameRate > $1.frameRate }
            return $0.pixelFormat < $1.pixelFormat
        }

        return section("Format", symbol: "rectangle.ratio.16.to.9") {
            labeledRow("Resolution") {
                Picker("", selection: Binding(
                    get: { currentResolution },
                    set: { selectedResolution = $0 }
                )) {
                    ForEach(resolutions, id: \.self) { Text($0).tag($0) }
                }
                .labelsHidden()
            }
            labeledRow("Frame rate") {
                Picker("", selection: Binding<String?>(
                    get: { model.activeFormatID },
                    set: { if let id = $0 { model.applyFormat(id: id) } }
                )) {
                    ForEach(frameRates) { format in
                        Text(format.fpsTitle).tag(Optional(format.id))
                    }
                }
                .labelsHidden()
            }
            Text("Frame rate changes apply to the next app that opens the camera.")
                .font(.system(size: 10))
                .foregroundStyle(.tertiary)
            if currentResolution == "3840x2160" {
                Text("At 4K the sensor tops out around 25 fps, even with 30 selected.")
                    .font(.system(size: 10))
                    .foregroundStyle(.tertiary)
            }
        }
    }

    // MARK: - Building blocks

    private func section(_ title: String, symbol: String, @ViewBuilder content: () -> some View) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                content()
            }
            .padding(4)
            .frame(maxWidth: .infinity, alignment: .leading)
        } label: {
            Label(title, systemImage: symbol)
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(.secondary)
        }
    }

    private func labeledRow(_ title: String, @ViewBuilder content: () -> some View) -> some View {
        HStack(spacing: 8) {
            Text(title)
                .font(.system(size: 12))
                .frame(width: 88, alignment: .leading)
            content()
        }
    }

    private func toggleRow(_ title: String, name: String) -> some View {
        HStack {
            Text(title)
                .font(.system(size: 12))
            Spacer()
            Toggle("", isOn: Binding(
                get: { (model.control(name)?.current ?? 0) != 0 },
                set: { model.setControl(name, to: $0 ? 1 : 0) }
            ))
            .toggleStyle(.switch)
            .controlSize(.mini)
            .labelsHidden()
        }
    }

    private func intBinding(_ name: String, normalize: @escaping (Int) -> Int = { $0 }) -> Binding<Int> {
        Binding(
            get: { normalize(model.control(name)?.current ?? 0) },
            set: { model.setControl(name, to: $0) }
        )
    }
}

// MARK: - Slider row

private struct ControlSliderRow: View {
    let control: CameraControl?
    var maxOverride: Int?
    var logarithmic = false
    var unit = ""
    var disabled = false
    let onChange: (Int) -> Void

    init(
        control: CameraControl?,
        maxOverride: Int? = nil,
        logarithmic: Bool = false,
        unit: String = "",
        disabled: Bool = false,
        onChange: @escaping (Int) -> Void
    ) {
        self.control = control
        self.maxOverride = maxOverride
        self.logarithmic = logarithmic
        self.unit = unit
        self.disabled = disabled
        self.onChange = onChange
    }

    var body: some View {
        if let control, control.settable, let lowerBound = control.min, let rawUpper = control.max {
            let upperBound = maxOverride.map { min($0, rawUpper) } ?? rawUpper
            HStack(spacing: 8) {
                Text(control.label)
                    .font(.system(size: 12))
                    .frame(width: 88, alignment: .leading)
                Slider(value: sliderBinding(control: control, lowerBound: lowerBound, upperBound: upperBound),
                       in: sliderRange(lowerBound: lowerBound, upperBound: upperBound))
                Text("\(control.current)\(unit)")
                    .font(.system(size: 11).monospacedDigit())
                    .foregroundStyle(.secondary)
                    .frame(width: 44, alignment: .trailing)
            }
            .disabled(disabled)
            .opacity(disabled ? 0.45 : 1)
        }
    }

    private func sliderRange(lowerBound: Int, upperBound: Int) -> ClosedRange<Double> {
        if logarithmic {
            let safeLower = max(lowerBound, 1)
            return log(Double(safeLower))...log(Double(upperBound))
        }
        return Double(lowerBound)...Double(upperBound)
    }

    private func sliderBinding(control: CameraControl, lowerBound: Int, upperBound: Int) -> Binding<Double> {
        Binding(
            get: {
                let clamped = Double(min(max(control.current, lowerBound), upperBound))
                return logarithmic ? log(max(clamped, 1)) : clamped
            },
            set: { raw in
                let value = logarithmic ? exp(raw) : raw
                let intValue = min(max(Int(value.rounded()), lowerBound), upperBound)
                onChange(intValue)
            }
        )
    }
}

// MARK: - Preview

struct CameraPreviewView: NSViewRepresentable {
    let controller: PreviewSessionController

    func makeNSView(context: Context) -> PreviewNSView {
        PreviewNSView(controller: controller)
    }

    func updateNSView(_ nsView: PreviewNSView, context: Context) {}
}

@MainActor
final class PreviewNSView: NSView {
    private let controller: PreviewSessionController
    private let previewLayer = AVCaptureVideoPreviewLayer()

    init(controller: PreviewSessionController) {
        self.controller = controller
        super.init(frame: .zero)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        previewLayer.videoGravity = .resizeAspectFill
        previewLayer.session = controller.session
        layer?.addSublayer(previewLayer)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func layout() {
        super.layout()
        previewLayer.frame = bounds
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window == nil {
            controller.stop()
        } else {
            controller.start()
        }
    }
}
