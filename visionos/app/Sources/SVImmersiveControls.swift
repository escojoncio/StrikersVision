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
    static var controlsOpen = false
    /// openWindow called, the window not on screen yet: set synchronously, so two handlers in the same
    /// update never open the same window twice (the counters above change only when the window appears).
    static var launcherOpening = false
    static var controlsOpening = false

    static func openLauncher(_ action: OpenWindowAction) {
        guard open == 0 && !launcherOpening else { return }
        launcherOpening = true
        action(id: SVScene.launcher)
        clearOpeningLater()
    }

    /// If openWindow never creates the window, the flags would stay true for good: clear them after 2 s
    /// (SVApp also clears them when the app becomes active).
    static func clearOpeningLater() {
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            launcherOpening = false
            controlsOpening = false
        }
    }
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
    @State private var placeRun = 0

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
                    // Works with any controller (a DualSense has no tracked hands). With Sense controllers
                    // the left grip also carries the screen or the table.
                    HStack {
                        Button("Screen in front of me") { place("screen-here") }
                        Button("Screen back to its place") { place("screen-reset") }
                    }
                    .buttonStyle(.bordered)
                    if game.viewMode == Int32(SV_VIEW_TABLETOP_ROOM) || game.viewMode == Int32(SV_VIEW_TABLETOP_STADIUM) {
                        Button("Table in front of me") { place("table-here") }
                            .buttonStyle(.bordered)
                        HStack {
                            Button { place("table-near") } label: { Image(systemName: "arrow.down") }
                            Button { place("table-far") } label: { Image(systemName: "arrow.up") }
                            Button { place("table-left") } label: { Image(systemName: "rotate.left") }
                            Button { place("table-right") } label: { Image(systemName: "rotate.right") }
                            Button { place("table-down") } label: { Image(systemName: "chevron.down") }
                            Button { place("table-up") } label: { Image(systemName: "chevron.up") }
                        }
                        .buttonStyle(.bordered)
                    }
                } header: {
                    Text("Place")
                } footer: {
                    Text("Arrows: nearer, farther, turn, lower, higher. With Sense controllers, hold the left grip on the screen or the table to carry it.")
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
                Task { await game.quitApp { await dismissImmersiveSpace() } }
            }
        }
        .onAppear {
            SVLauncherWindows.controlsOpen = true
            SVLauncherWindows.controlsOpening = false
            // A window the system restored at launch, with no game view open: nothing to control.
            if !game.immersiveOpen { close() }
        }
        .onDisappear {
            SVLauncherWindows.controlsOpen = false
            SVLauncherWindows.controlsOpening = false
            // Hidden while paused: the game must not stay paused with no button to resume it.
            if paused {
                paused = false
                sv_set_paused(0)
            }
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
        if game.panelsVisible { SVLauncherWindows.openLauncher(openWindow) }
        dismissWindow(id: SVScene.controls)
    }

    private func place(_ command: String) {
        // A fresh tag every press (also across reopened windows): the XR layer runs each value once.
        placeRun += 1
        let tag = "\(Int(Date().timeIntervalSince1970 * 1000))-\(placeRun)"
        sv_setting_changed("STRIKERS_XR_PLACE", "\(tag):\(command)")
    }

    private func binding(_ key: String) -> Binding<String> {
        Binding(get: { settings.value(key) }, set: { settings.set(key, $0) })
    }
}
