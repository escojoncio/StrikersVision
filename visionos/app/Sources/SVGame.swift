// StrikersVision: the game's lifetime in this process, the C callbacks it calls back into, and the
// hand-over of the CompositorLayer's renderer.
//
// One game per process (sv_bridge.h): sv_game_start runs once; closing the immersive space only
// pauses it, and reopening the space hands the same game a new LayerRenderer.

import CompositorServices
import Foundation
import SwiftUI

@MainActor
final class SVGame: ObservableObject {
    static let shared = SVGame()

    enum Phase: Equatable {
        case idle                 // not started in this process
        case running
        case ended(String)        // the game thread is gone; a new game needs a new launch
    }

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var viewMode: Int32 = Int32(SV_VIEW_JUMBOTRON_ROOM)
    /// The XR layer's wish (set_upper_limbs_visible); the scene combines it with the player's setting.
    @Published private(set) var limbsWanted = true
    @Published var immersiveOpen = false
    /// The game booted far enough to draw (game_ready): the immersive space opens only then, so it
    /// never shows an empty view while the disc and the first scene load.
    @Published private(set) var ready = false
    /// The online room the game is in ("" = offline), and whether through SharePlay.
    @Published private(set) var room = ""
    @Published private(set) var roomViaSharePlay = false

    private init() {}

    /// Before the first start: the callbacks and the environment. Then the game thread.
    func start(disc: SVDiscImage?, pickedFolder: URL?) -> Bool {
        guard phase == .idle else { return phase == .running }
        registerCallbacks()

        let settings = SVSettings.shared
        settings.applyToEnvironment(region: disc?.region)
        let home = SVPaths.home.path
        setenv("STRIKERS_HOME", home, 1)
        setenv("STRIKERS_LOG", SVLog.gameLogURL.path, 1)
        setenv("STRIKERS_USER_DIR", SVPaths.saves.path, 1)
        setenv("STRIKERS_CACHE_DIR", SVPaths.cache.path, 1)
        // The exact image the launcher showed, so the game and the launcher agree on which one it is
        // (the port's own search looks beside its executable, which here is the read-only bundle).
        if let disc { setenv("STRIKERS_DATA", disc.url.path, 1) } else { unsetenv("STRIKERS_DATA") }
        // No message boxes on a headset: a fatal error ends the game thread and comes back through
        // game_ended, and its text is in the log.
        setenv("STRIKERS_NO_MESSAGEBOX", "1", 1)
        // The intro and attract movies need a video decoder this build does not have.
        setenv("STRIKERS_NO_MOVIES", "1", 1)

        SVLog.log("starting the game: home \(home), disc \(disc?.url.path ?? "none")"
                  + (disc?.gameID.map { " (\($0))" } ?? "")
                  + (pickedFolder.map { ", picked folder \($0.path)" } ?? ""))
        let ok: Bool
        if let folder = pickedFolder?.path {
            ok = sv_game_start(home, folder) != 0
        } else {
            ok = sv_game_start(home, nil) != 0
        }
        if ok {
            phase = .running
            SVLog.gameOwnsStderr = true
        } else {
            phase = .ended(String(localized: "The game could not start. See the log."))
            SVLog.log("sv_game_start failed")
        }
        return ok
    }

    /// The launcher's pick: "" offline, a code for the relay, or the SharePlay group. Takes effect at
    /// once before a match, or after the current one (xr_net.cpp).
    func setRoom(_ code: String, viaSharePlay: Bool) {
        room = code
        roomViaSharePlay = viaSharePlay && !code.isEmpty
        sv_net_set_room(code, roomViaSharePlay ? 1 : 0)
        SVLog.log(code.isEmpty ? "room: offline" : "room: \(code)\(roomViaSharePlay ? " (SharePlay)" : "")")
    }

    func setViewMode(_ mode: Int32) {
        sv_set_view_mode(mode)
    }

    func requestQuit() {
        guard phase == .running else { return }
        SVLog.log("quit requested")
        sv_game_request_quit()
    }

    /// Full quit: asks the game to stop, waits (at most ~3 s, without blocking the UI) for the game
    /// thread to end so saves are flushed, closes the immersive space and kills the process.
    /// Not running (idle or already ended): exits at once.
    func quitApp(dismissSpace: @escaping @MainActor () async -> Void) async {
        if phase == .running {
            requestQuit()
            let deadline = Date().addingTimeInterval(3)
            while phase == .running && Date() < deadline {
                try? await Task.sleep(nanoseconds: 50_000_000)
            }
            if phase == .running { SVLog.log("quit: the game did not end within 3 s") }
        }
        if immersiveOpen { await dismissSpace() }
        SVLog.log("quit: exiting the process")
        SVLog.flush()
        exit(0)
    }

    // MARK: callbacks from C (any thread)

    fileprivate nonisolated static func limbsVisible(_ visible: Bool) {
        DispatchQueue.main.async { MainActor.assumeIsolated { SVGame.shared.limbsWanted = visible } }
    }

    fileprivate nonisolated static func ended(_ reason: String) {
        SVLog.log("the game ended: \(reason)")
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                SVGame.shared.phase = .ended(reason)
                SVLog.gameOwnsStderr = false
            }
        }
    }

    fileprivate nonisolated static func gameReady() {
        SVLog.log("the game is ready to draw")
        DispatchQueue.main.async { MainActor.assumeIsolated { SVGame.shared.ready = true } }
    }

    fileprivate nonisolated static func viewModeChanged(_ mode: Int32) {
        DispatchQueue.main.async { MainActor.assumeIsolated { SVGame.shared.viewMode = mode } }
    }

    private func registerCallbacks() {
        // Kept for the life of the process: the C side may keep the pointer rather than copy it.
        let callbacks = UnsafeMutablePointer<SVAppCallbacks>.allocate(capacity: 1)
        callbacks.initialize(to: SVAppCallbacks(
            set_upper_limbs_visible: svCallbackLimbs,
            game_ended: svCallbackEnded,
            log_line: svCallbackLogLine,
            view_mode_changed: svCallbackViewMode,
            game_ready: svCallbackReady))
        sv_register_app_callbacks(callbacks)
    }
}

// Plain functions: a C function pointer cannot capture anything.
private func svCallbackLimbs(_ visible: Int32) { SVGame.limbsVisible(visible != 0) }
private func svCallbackEnded(_ reason: UnsafePointer<CChar>?) {
    SVGame.ended(reason.map { String(cString: $0) } ?? "unknown")
}
private func svCallbackLogLine(_ line: UnsafePointer<CChar>?) {
    guard let line else { return }
    SVLog.gameLine(String(cString: line))
}
private func svCallbackViewMode(_ mode: Int32) { SVGame.viewModeChanged(mode) }
private func svCallbackReady() { SVGame.gameReady() }

/// The CompositorLayer's renderer: handed to the game, kept alive here, and watched until it is
/// invalidated (the space closed), which the game hears through sv_layer_renderer_invalidated.
enum SVRendererHandOff {
    private static let lock = NSLock()
    nonisolated(unsafe) private static var current: LayerRenderer?
    nonisolated(unsafe) private static var generation = 0

    static func attach(_ renderer: LayerRenderer) {
        lock.lock()
        generation += 1
        let mine = generation
        current = renderer
        lock.unlock()
        SVLog.log("immersive space: renderer \(mine) attached")
        // The opaque pointer of the Objective-C object is the cp_layer_renderer_t the C side expects.
        sv_set_layer_renderer(Unmanaged.passUnretained(renderer).toOpaque())

        // Compositor Services reports the end of a layer only through its state; a light poll on a
        // thread of its own is enough (the game thread does the real waiting).
        let watcher = Thread {
            var lastState = renderer.state
            while true {
                let state = renderer.state
                if state != lastState {
                    SVLog.log("immersive space: renderer \(mine) is \(String(describing: state))")
                    lastState = state
                }
                if state == .invalidated { break }
                SVRendererHandOff.lock.lock()
                let superseded = SVRendererHandOff.generation != mine
                SVRendererHandOff.lock.unlock()
                if superseded { return }
                Thread.sleep(forTimeInterval: 0.1)
            }
            SVRendererHandOff.lock.lock()
            let stillCurrent = SVRendererHandOff.generation == mine
            if stillCurrent { SVRendererHandOff.current = nil }
            SVRendererHandOff.lock.unlock()
            if stillCurrent {
                sv_layer_renderer_invalidated()
                DispatchQueue.main.async { MainActor.assumeIsolated { SVGame.shared.immersiveOpen = false } }
            }
        }
        watcher.name = "strikers-layer-watch"
        watcher.start()
    }
}
