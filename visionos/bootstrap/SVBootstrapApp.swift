// StrikersVision bootstrap: a placeholder app that only proves libstrikers.a links into a visionOS
// app. It starts the game thread (no headset layer: the PortXr* stubs in sv_bootstrap_stubs.c) and
// shows nothing else. Replaced by visionos/app + visionos/xr + visionos/net.
import SwiftUI

@main
struct SVBootstrapApp: App {
    var body: some Scene {
        WindowGroup {
            SVBootstrapView()
        }
    }
}

struct SVBootstrapView: View {
    @State private var started = false

    var body: some View {
        VStack(spacing: 24) {
            Text("StrikersVision").font(.largeTitle)
            Text("Bootstrap build: game library linked, no headset layer yet.")
            Button(started ? "Running" : "Start game thread") {
                started = SVBootstrapView.start()
            }
            .disabled(started)
        }
        .padding(40)
    }

    private static func start() -> Bool {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let home = docs.appendingPathComponent("Strikers", isDirectory: true)
        try? FileManager.default.createDirectory(at: home, withIntermediateDirectories: true)
        return home.path.withCString { sv_game_start($0, nil) } == 1
    }
}
