// StrikersVision: the app's log and the log viewer.
//
// There is no Mac to attach to the headset, so everything worth knowing ends up in a file the
// player can open in the app and share:
//   <Documents>/Strikers/strikers-log.txt      the game's own log (its stderr, STRIKERS_LOG)
//   <Documents>/Strikers/strikers-app-log.txt  the app's lines (launcher, SharePlay, network)
// The game opens its log with "w" when it starts, so the app keeps its own file; once the game runs,
// app lines also go to stderr, i.e. into the game log, so one file tells the whole story.

import Foundation
import SwiftUI
import UIKit
import Combine

@MainActor
final class SVLog: ObservableObject {
    static let shared = SVLog()

    /// The live lines shown in the viewer (app lines and the game's log_line callback).
    @Published private(set) var lines: [String] = []
    nonisolated static let maxLines = 4000

    private nonisolated(unsafe) static var handle: FileHandle?
    private nonisolated static let fileLock = NSLock()
    nonisolated(unsafe) static var gameOwnsStderr = false

    private init() {}

    nonisolated static var appLogURL: URL { SVPaths.home.appendingPathComponent("strikers-app-log.txt") }
    nonisolated static var gameLogURL: URL { SVPaths.home.appendingPathComponent("strikers-log.txt") }

    /// A fresh app log for this launch (the previous one is kept as strikers-app-log-previous.txt).
    nonisolated static func start() {
        fileLock.lock(); defer { fileLock.unlock() }
        let fm = FileManager.default
        try? fm.createDirectory(at: SVPaths.home, withIntermediateDirectories: true)
        let previous = SVPaths.home.appendingPathComponent("strikers-app-log-previous.txt")
        try? fm.removeItem(at: previous)
        try? fm.moveItem(at: appLogURL, to: previous)
        fm.createFile(atPath: appLogURL.path, contents: nil)
        handle = try? FileHandle(forWritingTo: appLogURL)
        let info = Bundle.main.infoDictionary ?? [:]
        let first = "StrikersVision \(info["CFBundleShortVersionString"] ?? "?") (\(info["CFBundleVersion"] ?? "?")), "
            + "\(ProcessInfo.processInfo.operatingSystemVersionString)\n"
        handle?.write(Data(first.utf8))
    }

    /// One app line: file, system log, game log (once the game owns stderr) and the viewer.
    nonisolated static func log(_ message: String) {
        NSLog("[app] %@", message)
        let stamp = ISO8601DateFormatter.string(from: Date(), timeZone: .current,
                                                formatOptions: [.withTime, .withColonSeparatorInTime])
        let line = "\(stamp) \(message)"
        fileLock.lock()
        handle?.write(Data((line + "\n").utf8))
        if gameOwnsStderr { fputs("[app] \(message)\n", stderr) }
        fileLock.unlock()
        enqueue(line)
    }

    /// One game line (the log_line callback; already in strikers-log.txt). Any thread.
    nonisolated static func gameLine(_ line: String) {
        enqueue(line)
    }

    // Lines from any thread are batched and handed to the viewer at most ten times a second: the
    // game can log hundreds of lines in a burst, and one main-thread hop per line would stall the UI.
    private nonisolated static let pendingLock = NSLock()
    private nonisolated(unsafe) static var pending: [String] = []
    private nonisolated(unsafe) static var flushScheduled = false

    private nonisolated static func enqueue(_ line: String) {
        pendingLock.lock()
        pending.append(line)
        if pending.count > maxLines { pending.removeFirst(pending.count - maxLines) }
        let schedule = !flushScheduled
        flushScheduled = true
        pendingLock.unlock()
        guard schedule else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
            SVLog.pendingLock.lock()
            let batch = SVLog.pending
            SVLog.pending.removeAll(keepingCapacity: true)
            SVLog.flushScheduled = false
            SVLog.pendingLock.unlock()
            MainActor.assumeIsolated { SVLog.shared.append(batch) }
        }
    }

    func append(_ batch: [String]) {
        lines.append(contentsOf: batch)
        if lines.count > Self.maxLines { lines.removeFirst(lines.count - Self.maxLines) }
    }

    /// The last `bytes` of a log file.
    nonisolated static func tail(_ url: URL, bytes: Int = 64 * 1024) -> String {
        guard let h = try? FileHandle(forReadingFrom: url) else { return "" }
        defer { try? h.close() }
        let size = (try? h.seekToEnd()) ?? 0
        try? h.seek(toOffset: size > UInt64(bytes) ? size - UInt64(bytes) : 0)
        return String(decoding: (try? h.readToEnd()) ?? Data(), as: UTF8.self)
    }

    /// The log files that exist, for sharing.
    nonisolated static var shareable: [URL] {
        [gameLogURL, appLogURL, SVPaths.home.appendingPathComponent("strikers-app-log-previous.txt")]
            .filter { FileManager.default.fileExists(atPath: $0.path) }
    }
}

/// The log viewer: the game log's tail, or the live lines, with copy and share.
struct SVLogView: View {
    enum Source: String, CaseIterable, Identifiable {
        case game, live, app
        var id: String { rawValue }
        var title: LocalizedStringKey {
            switch self {
            case .game: return "Game log"
            case .live: return "Live"
            case .app: return "App log"
            }
        }
    }

    @ObservedObject private var log = SVLog.shared
    @State private var source: Source = .game
    @State private var text = ""
    @State private var copied = false
    private let refresh = Timer.publish(every: 1.5, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Picker("Source", selection: $source) {
                    ForEach(Source.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)
                .frame(maxWidth: 420)
                Spacer()
                Button {
                    UIPasteboard.general.string = shown
                    copied = true
                } label: {
                    Label(copied ? "Copied" : "Copy", systemImage: copied ? "checkmark" : "doc.on.doc")
                }
                if let file = shareFile {
                    ShareLink(item: file) { Label("Share", systemImage: "square.and.arrow.up") }
                }
            }
            ScrollViewReader { proxy in
                ScrollView {
                    Text(shown.isEmpty ? String(localized: "Nothing here yet.") : shown)
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(8)
                    Color.clear.frame(height: 1).id("end")
                }
                .background(.black.opacity(0.25), in: RoundedRectangle(cornerRadius: 12))
                .onChange(of: shown) { _, _ in proxy.scrollTo("end", anchor: .bottom) }
                .onAppear { proxy.scrollTo("end", anchor: .bottom) }
            }
        }
        .padding()
        .onAppear(perform: reload)
        .onChange(of: source) { _, _ in reload(); copied = false }
        .onReceive(refresh) { _ in if source != .live { reload() } }
    }

    private var shown: String {
        source == .live ? log.lines.suffix(800).joined(separator: "\n") : text
    }

    private var shareFile: URL? {
        switch source {
        case .game, .live:
            return FileManager.default.fileExists(atPath: SVLog.gameLogURL.path) ? SVLog.gameLogURL : nil
        case .app:
            return SVLog.appLogURL
        }
    }

    private func reload() {
        switch source {
        case .game: text = SVLog.tail(SVLog.gameLogURL)
        case .app: text = SVLog.tail(SVLog.appLogURL)
        case .live: break
        }
    }
}
