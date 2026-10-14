// StrikersVision: the match controls, a small window that floats beside the game while the immersive
// space is open.
//
// Why a window: the immersive space's only content is a CompositorLayer, which cannot hold SwiftUI
// views, attachments or ornaments. While an immersive space is open visionOS keeps showing all of
// the app's own windows (and hides other apps' windows), so a separate small WindowGroup is the
// supported way to have controls in reach mid-game. It opens with the space (SVLauncher.openSpace)
// and closes itself when the space goes away.

import SwiftUI

extension SVScene {
    static let controls = "match-controls"
}

/// How many launcher windows are on screen (SVApp counts them), so closing the controls never
/// leaves the app with no window at all after the player closed the launcher mid-game.
@MainActor
enum SVLauncherWindows {
    static var open = 0
}

struct SVImmersiveControlsView: View {
    @ObservedObject private var game = SVGame.shared
    @ObservedObject private var settings = SVSettings.shared
    @Environment(\.dismissWindow) private var dismissWindow
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @Environment(\.openWindow) private var openWindow
    @State private var paused = false
    @State private var showAll = false
    @State private var confirmQuit = false
    @State private var testRun = 0

    var body: some View {
        NavigationStack {
            Form {
                Section("View") {
                    Picker("View", selection: Binding(get: { game.viewMode }, set: { game.setViewMode($0) })) {
                        Text("Big screen").tag(Int32(SV_VIEW_JUMBOTRON_ROOM))
                        Text("Big screen · stadium").tag(Int32(SV_VIEW_JUMBOTRON_STADIUM))
                        Text("Table").tag(Int32(SV_VIEW_TABLETOP_ROOM))
                        Text("Table · stadium").tag(Int32(SV_VIEW_TABLETOP_STADIUM))
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    Picker("Big screen size", selection: binding("STRIKERS_XR_SCREEN_SIZE")) {
                        Text("Small").tag("0")
                        Text("Medium").tag("1")
                        Text("Large").tag("2")
                    }
                    .pickerStyle(.segmented)
                }
                Section {
                    DisclosureGroup("All live settings", isExpanded: $showAll) {
                        ForEach(SVSettings.live) { def in
                            SVSettingRow(def: def, region: nil)
                        }
                    }
                }
                Section("Diagnostics") {
                    // Runs a full goal celebration (cannons, fireworks, the replay jumbotron) without
                    // scoring, through the game's own path, and logs what is alive.
                    Button("Test the goal celebration") {
                        testRun += 1
                        let run = testRun
                        sv_setting_changed("STRIKERS_XR_FX_TEST", "\(run):goal 6 super;replay;state")
                        DispatchQueue.main.asyncAfter(deadline: .now() + 6) {
                            sv_setting_changed("STRIKERS_XR_FX_TEST", "\(run)e:end;state")
                        }
                    }
                    .disabled(game.phase != .running)
                }
                Section {
                    HStack {
                        Button(paused ? "Resume" : "Pause") {
                            paused.toggle()
                            sv_set_paused(paused ? 1 : 0)
                        }
                        .buttonStyle(.bordered)
                        Spacer()
                        Button("Leave the game view") {
                            Task { await dismissImmersiveSpace() }
                        }
                        .buttonStyle(.bordered)
                        Button("Quit game", role: .destructive) { confirmQuit = true }
                            .buttonStyle(.bordered)
                    }
                }
            }
            .navigationTitle("Match")
        }
        .confirmationDialog("Quit the game?", isPresented: $confirmQuit) {
            Button("Quit game", role: .destructive) {
                game.requestQuit()
                Task { await dismissImmersiveSpace() }
            }
        }
        .onAppear {
            // A window the system restored at launch, with no game view open: nothing to control.
            if !game.immersiveOpen { close() }
        }
        .onChange(of: game.immersiveOpen) { _, open in
            if !open { close() }
        }
        .onChange(of: game.phase) { _, phase in
            if phase != .running {
                if game.immersiveOpen { Task { await dismissImmersiveSpace() } }
                close()
            }
        }
    }

    private func close() {
        if paused {
            paused = false
            sv_set_paused(0)
        }
        if SVLauncherWindows.open == 0 { openWindow(id: SVScene.launcher) }
        dismissWindow(id: SVScene.controls)
    }

    private func binding(_ key: String) -> Binding<String> {
        Binding(get: { settings.value(key) }, set: { settings.set(key, $0) })
    }
}
