import Foundation

enum HelperError: LocalizedError {
    case helperMissing
    case disconnected
    case command(String)

    var errorDescription: String? {
        switch self {
        case .helperMissing:
            return "Bundled onncam helper was not found"
        case .disconnected:
            return "Lost connection to the camera helper"
        case .command(let message):
            return message
        }
    }
}

/// Maintains a single long-lived `onncam serve` process so every camera
/// command is one line over a pipe instead of a fresh process spawn plus a
/// full USB device open. Commands run on a private serial queue; responses
/// are read synchronously on that queue (the helper always answers each line
/// with a terminating "ok"/"err" line).
final class HelperClient: @unchecked Sendable {
    private let queue = DispatchQueue(label: "OnnCamControl.HelperClient")
    private var process: Process?
    private var stdinHandle: FileHandle?
    private var stdoutHandle: FileHandle?
    private var buffer = Data()

    /// Sends one command line and returns any data lines preceding "ok".
    /// Throws `HelperError.command` for "err <message>" responses.
    func send(_ command: String) async throws -> [String] {
        try await withCheckedThrowingContinuation { continuation in
            queue.async {
                do {
                    continuation.resume(returning: try self.sendBlocking(command))
                } catch {
                    continuation.resume(throwing: error)
                }
            }
        }
    }

    func shutdown() {
        queue.async {
            if let stdinHandle = self.stdinHandle {
                try? stdinHandle.write(contentsOf: Data("quit\n".utf8))
            }
            self.reset()
        }
    }

    // MARK: - Queue-confined internals

    private func sendBlocking(_ command: String) throws -> [String] {
        do {
            return try attempt(command)
        } catch HelperError.disconnected {
            // Helper died (camera unplug, crash). One transparent retry with
            // a fresh process.
            return try attempt(command)
        }
    }

    private func attempt(_ command: String) throws -> [String] {
        try ensureRunning()
        guard let stdinHandle else { throw HelperError.disconnected }

        do {
            try stdinHandle.write(contentsOf: Data((command + "\n").utf8))
        } catch {
            reset()
            throw HelperError.disconnected
        }

        var lines: [String] = []
        while let line = try readLine() {
            if line == "ok" {
                return lines
            }
            if line.hasPrefix("err ") {
                throw HelperError.command(String(line.dropFirst(4)))
            }
            lines.append(line)
        }
        reset()
        throw HelperError.disconnected
    }

    private func readLine() throws -> String? {
        guard let stdoutHandle else { return nil }
        while true {
            if let newlineIndex = buffer.firstIndex(of: 0x0A) {
                let lineData = buffer[buffer.startIndex..<newlineIndex]
                let line = String(data: lineData, encoding: .utf8) ?? ""
                buffer.removeSubrange(buffer.startIndex...newlineIndex)
                return line
            }
            let chunk = stdoutHandle.availableData
            if chunk.isEmpty {
                return nil // EOF: helper exited
            }
            buffer.append(chunk)
        }
    }

    private func ensureRunning() throws {
        if let process, process.isRunning { return }
        reset()

        guard let helperURL = Self.helperURL else { throw HelperError.helperMissing }

        let stdinPipe = Pipe()
        let stdoutPipe = Pipe()
        let process = Process()
        process.executableURL = helperURL
        process.arguments = ["serve"]
        process.standardInput = stdinPipe
        process.standardOutput = stdoutPipe
        process.standardError = FileHandle.nullDevice
        try process.run()

        self.process = process
        self.stdinHandle = stdinPipe.fileHandleForWriting
        self.stdoutHandle = stdoutPipe.fileHandleForReading
        self.buffer = Data()
    }

    private func reset() {
        if let process, process.isRunning {
            process.terminate()
        }
        try? stdinHandle?.close()
        try? stdoutHandle?.close()
        process = nil
        stdinHandle = nil
        stdoutHandle = nil
        buffer = Data()
    }

    private static var helperURL: URL? {
        if let bundled = Bundle.main.url(forResource: "onncam", withExtension: nil) {
            return bundled
        }
        let dev = URL(fileURLWithPath: "/Users/tylermansmann/Documents/brainsim/onncam/onncam")
        return FileManager.default.isExecutableFile(atPath: dev.path) ? dev : nil
    }
}
